#include <string.h>
#include <unity.h>

#include <vector>

#include "config.h"
#include "usb/ch376.h"

// Mock CH376S. Чіп приймає кадри з порту лише на тій швидкості, на якій працює сам (допуск 2 %).
// Перемикання SET_BAUDRATE: команда і відповідь іще на старій швидкості, потім чіп переходить на
// швидкість 6 000 000 / (256 − K), якщо вона дозволена у supported. Час керований: readByte/delayMs зсувають його.

namespace {

bool sameBaud(uint32_t a, uint32_t b) {
  return a > b ? (a - b) * 100u <= 2u * b : (b - a) * 100u <= 2u * a;
}

// Ін'єкція збоїв.
struct Faults {
  bool silent = false;         // чіп не відповідає ні на що
  bool badCheck = false;       // CHECK_EXIST повертає байт без інверсії
  bool badMode = false;        // SET_USB_MODE повертає 0x5F
  int junkBefore = 0;          // сміття (0x7E) перед кожною відповіддю, що фільтрується
  bool diskGone = false;       // DISK_CONNECT/MOUNT: диска немає (0x82)
  uint8_t mountCode = 0x14;    // код відповіді DISK_MOUNT (0x14 = успіх)
  bool dieAfterBaud = false;   // після SET_BAUDRATE з перемиканням чіп мовчить до апаратного скиду
  int vanishAt = -1;           // номер команди (з 1), після якої пристрій зникає назавжди
  bool rstPresent = true;      // чи є RST (hwReset)
};

class MockLink : public Ch376Link {
 public:
  // Стан чіпа
  uint32_t chipBaud = 9600;
  uint32_t portBaud = 9600;
  bool dead = false;   // мовчить до апаратного скиду
  bool gone = false;   // зник назавжди
  std::vector<uint32_t> supported;  // швидкості, які чіп приймає після SET_BAUDRATE

  // Журнал
  std::vector<std::vector<uint8_t>> frames;
  int cmds = 0;
  int resets = 0;
  int hwResets = 0;

  // Час і події (мкс): подія стає видимою з моменту at
  uint32_t nowV = 0;
  struct Ev {
    uint32_t at;
    uint8_t code;
  };
  std::vector<Ev> events;

  bool abortFlag = false;
  Faults f;
  std::vector<uint8_t> rx;

  void write(const uint8_t* d, size_t n) override {
    frames.emplace_back(d, d + n);
    if (gone) return;
    cmds++;
    if (f.vanishAt > 0 && cmds >= f.vanishAt) {
      gone = true;
      return;
    }
    if (dead || f.silent) return;
    if (!sameBaud(portBaud, chipBaud)) return;  // чіп не розпізнає кадр на чужій швидкості
    if (n < 3 || d[0] != 0x57 || d[1] != 0xAB) return;
    switch (d[2]) {
      case 0x05:  // RESET_ALL
        resets++;
        chipBaud = 9600;
        events.clear();
        break;
      case 0x06:  // CHECK_EXIST
        replyFiltered(f.badCheck ? d[3] : (uint8_t)~d[3]);
        break;
      case 0x15:  // SET_USB_MODE
        replyFiltered(f.badMode ? 0x5F : 0x51);
        break;
      case 0x02: {  // SET_BAUDRATE: відповідь ще на старій швидкості
        replyFiltered(0x51);
        if (d[3] == 0x03) {
          const uint32_t b = 6000000u / (256u - d[4]);
          for (uint32_t s : supported) {
            if (sameBaud(s, b)) {
              chipBaud = b;
              break;
            }
          }
          if (f.dieAfterBaud && chipBaud != 9600) dead = true;
        }
        break;
      }
      case 0x22:  // GET_STATUS: повертає першу видиму подію і стирає її
        replyRaw(takeEvent());
        break;
      case 0x30:  // DISK_CONNECT
        events.push_back(Ev{nowV, f.diskGone ? (uint8_t)0x82 : (uint8_t)0x14});
        break;
      case 0x31:  // DISK_MOUNT
        events.push_back(Ev{nowV, f.diskGone ? (uint8_t)0x82 : f.mountCode});
        break;
      default:
        break;
    }
  }

  bool readByte(uint8_t& b, uint32_t tmoMs) override {
    if (!rx.empty()) {
      b = rx.front();
      rx.erase(rx.begin());
      return true;
    }
    nowV += tmoMs * 1000u;
    return false;
  }

  void flushRx() override { rx.clear(); }

  bool setBaud(uint32_t baud) override {
    portBaud = baud;
    return true;
  }

  bool hasIntPin() const override { return false; }
  bool intLow() const override { return false; }

  bool hwReset() override {
    if (!f.rstPresent) return false;
    hwResets++;
    if (!gone) {
      dead = false;
      chipBaud = 9600;
      events.clear();
    }
    return true;
  }

  bool abortRequested() const override { return abortFlag; }
  void delayMs(uint32_t ms) override { nowV += ms * 1000u; }
  uint32_t nowUs() const override { return nowV; }

 private:
  void replyRaw(uint8_t b) { rx.push_back(b); }
  void replyFiltered(uint8_t b) {
    for (int i = 0; i < f.junkBefore; i++) rx.push_back(0x7E);
    rx.push_back(b);
  }
  uint8_t takeEvent() {
    for (size_t i = 0; i < events.size(); i++) {
      if (events[i].at <= nowV) {
        uint8_t c = events[i].code;
        events.erase(events.begin() + (long)i);
        return c;
      }
    }
    return kCh376IntNone;
  }
};

// Типова таблиця: усі дільники дозволені чіпу.
void allSupported(MockLink& L) { L.supported = {1000000, 460800, 230400, 115200}; }

}  // namespace

// ---------------------------------------------------------------- байти команд

void test_bytes_check_exist() {
  MockLink L;
  Ch376 d(L);
  TEST_ASSERT_EQUAL_INT((int)Ch376Err::Ok, (int)d.checkExist());
  TEST_ASSERT_EQUAL_UINT32(1, L.frames.size());
  const uint8_t want[] = {0x57, 0xAB, 0x06, 0x55};
  TEST_ASSERT_EQUAL_UINT32(sizeof want, L.frames[0].size());
  TEST_ASSERT_TRUE(memcmp(L.frames[0].data(), want, sizeof want) == 0);
}

void test_bytes_set_usb_mode() {
  MockLink L;
  Ch376 d(L);
  TEST_ASSERT_EQUAL_INT((int)Ch376Err::Ok, (int)d.setUsbMode(kCh376ModeHost));
  const uint8_t want[] = {0x57, 0xAB, 0x15, kCh376ModeHost};
  TEST_ASSERT_EQUAL_UINT32(sizeof want, L.frames[0].size());
  TEST_ASSERT_TRUE(memcmp(L.frames[0].data(), want, sizeof want) == 0);
}

void test_bytes_disk_mount() {
  MockLink L;
  Ch376 d(L);
  TEST_ASSERT_EQUAL_INT((int)Ch376Err::Ok, (int)d.diskMount());
  const uint8_t want[] = {0x57, 0xAB, 0x31};
  TEST_ASSERT_EQUAL_UINT32(sizeof want, L.frames[0].size());
  TEST_ASSERT_TRUE(memcmp(L.frames[0].data(), want, sizeof want) == 0);
}

void test_bytes_negotiate_frames_order() {
  // reset → SET_BAUDRATE 0x03,K → CHECK_EXIST: порядок і байти кадрів.
  MockLink L;
  allSupported(L);
  Ch376 d(L);
  TEST_ASSERT_EQUAL_INT((int)Ch376Err::Ok, (int)d.reset());
  const Ch376BaudDiv one[1] = {{460800, 0x03, 0xF3}};
  Ch376Attempt att[1];
  size_t n = 0;
  TEST_ASSERT_EQUAL_INT((int)Ch376Err::Ok, (int)d.negotiateBaud(one, 1, att, 1, n));
  TEST_ASSERT_EQUAL_UINT32(3, L.frames.size());
  const uint8_t rst[] = {0x57, 0xAB, 0x05};
  const uint8_t sb[] = {0x57, 0xAB, 0x02, 0x03, 0xF3};
  const uint8_t ce[] = {0x57, 0xAB, 0x06, 0x55};
  TEST_ASSERT_TRUE(memcmp(L.frames[0].data(), rst, sizeof rst) == 0);
  TEST_ASSERT_TRUE(memcmp(L.frames[1].data(), sb, sizeof sb) == 0);
  TEST_ASSERT_TRUE(memcmp(L.frames[2].data(), ce, sizeof ce) == 0);
}

// ---------------------------------------------------------------- скид

void test_reset_to_9600() {
  MockLink L;
  L.chipBaud = 115200;
  L.portBaud = 115200;
  Ch376 d(L);
  TEST_ASSERT_EQUAL_INT((int)Ch376Err::Ok, (int)d.reset());
  TEST_ASSERT_EQUAL_UINT32(1, L.frames.size());
  TEST_ASSERT_EQUAL_UINT32(9600, L.chipBaud);
  TEST_ASSERT_EQUAL_UINT32(9600, L.portBaud);
  TEST_ASSERT_EQUAL_UINT32(9600, d.baud());
}

void test_reset_idempotent() {
  MockLink L;
  Ch376 d(L);
  TEST_ASSERT_EQUAL_INT((int)Ch376Err::Ok, (int)d.reset());
  TEST_ASSERT_EQUAL_INT((int)Ch376Err::Ok, (int)d.reset());
  TEST_ASSERT_EQUAL_INT((int)Ch376Err::Ok, (int)d.checkExist());
  TEST_ASSERT_EQUAL_INT(2, L.resets);
}

// ---------------------------------------------------------------- CHECK_EXIST

void test_check_exist_ok() {
  MockLink L;
  Ch376 d(L);
  TEST_ASSERT_EQUAL_INT((int)Ch376Err::Ok, (int)d.checkExist());
}

void test_check_exist_bad_byte() {
  MockLink L;
  L.f.badCheck = true;
  Ch376 d(L);
  TEST_ASSERT_EQUAL_INT((int)Ch376Err::BadReply, (int)d.checkExist());
}

void test_check_exist_silent() {
  MockLink L;
  L.f.silent = true;
  Ch376 d(L);
  TEST_ASSERT_EQUAL_INT((int)Ch376Err::NoReply, (int)d.checkExist());
}

void test_junk_15_before_reply_ok() {
  MockLink L;
  L.f.junkBefore = kCh376JunkMax - 1;  // 15 байтів сміття: відповідь ще в межах ліміту
  Ch376 d(L);
  TEST_ASSERT_EQUAL_INT((int)Ch376Err::Ok, (int)d.checkExist());
}

void test_junk_16_before_reply_bad() {
  MockLink L;
  L.f.junkBefore = kCh376JunkMax;  // ліміт вичерпано до відповіді
  Ch376 d(L);
  TEST_ASSERT_EQUAL_INT((int)Ch376Err::BadReply, (int)d.checkExist());
}

// ---------------------------------------------------------------- SET_USB_MODE

void test_mode_ok() {
  MockLink L;
  Ch376 d(L);
  TEST_ASSERT_EQUAL_INT((int)Ch376Err::Ok, (int)d.setUsbMode(kCh376ModeHost));
  TEST_ASSERT_EQUAL_UINT32(1, L.frames.size());
}

void test_mode_bad_code() {
  MockLink L;
  L.f.badMode = true;
  Ch376 d(L);
  TEST_ASSERT_EQUAL_INT((int)Ch376Err::BadReply, (int)d.setUsbMode(kCh376ModeHost));
}

void test_mode_silent() {
  MockLink L;
  L.f.silent = true;
  Ch376 d(L);
  TEST_ASSERT_EQUAL_INT((int)Ch376Err::NoReply, (int)d.setUsbMode(kCh376ModeHost));
}

// ---------------------------------------------------------------- події

void test_wait_connect_event() {
  MockLink L;
  L.events.push_back({200000u, kCh376IntConnect});
  Ch376 d(L);
  TEST_ASSERT_EQUAL_INT((int)Ch376Err::Ok, (int)d.waitConnect(1000));
  TEST_ASSERT_TRUE(L.nowV >= 200000u);
}

void test_wait_connect_timeout() {
  MockLink L;
  Ch376 d(L);
  TEST_ASSERT_EQUAL_INT((int)Ch376Err::Timeout, (int)d.waitConnect(1000));
  TEST_ASSERT_TRUE(L.nowV >= 1000000u);
}

void test_wait_connect_disconnect() {
  MockLink L;
  L.events.push_back({100000u, kCh376IntDisconnect});
  Ch376 d(L);
  TEST_ASSERT_EQUAL_INT((int)Ch376Err::Disconnected, (int)d.waitConnect(1000));
}

void test_wait_time_budget_3s() {
  // Тайм-аут 3 с (ліміт умови): повертається не раніше 3 с і не пізніше ~3,01 с.
  MockLink L;
  Ch376 d(L);
  TEST_ASSERT_EQUAL_INT((int)Ch376Err::Timeout, (int)d.waitConnect(kCh376ConnectWaitMs));
  TEST_ASSERT_TRUE(L.nowV >= 3000000u);
  TEST_ASSERT_TRUE(L.nowV < 3010000u);
}

void test_vanish_during_wait_gives_timeout() {
  // Пристрій зникає посеред очікування: це Timeout, а не Disconnected (події відключення не було).
  MockLink L;
  L.f.vanishAt = 3;
  Ch376 d(L);
  TEST_ASSERT_EQUAL_INT((int)Ch376Err::Timeout, (int)d.waitConnect(500));
  TEST_ASSERT_TRUE(L.gone);
}

void test_aborted() {
  MockLink L;
  L.abortFlag = true;
  Ch376 d(L);
  TEST_ASSERT_EQUAL_INT((int)Ch376Err::Aborted, (int)d.checkExist());
  TEST_ASSERT_EQUAL_INT((int)Ch376Err::Aborted, (int)d.waitConnect(100));
}

// ---------------------------------------------------------------- DISK_CONNECT / DISK_MOUNT

void test_disk_connect_ok() {
  MockLink L;
  Ch376 d(L);
  TEST_ASSERT_EQUAL_INT((int)Ch376Err::Ok, (int)d.diskConnect());
}

void test_disk_connect_no_disk() {
  MockLink L;
  L.f.diskGone = true;
  Ch376 d(L);
  TEST_ASSERT_EQUAL_INT((int)Ch376Err::NoDisk, (int)d.diskConnect());
}

void test_disk_mount_ok() {
  MockLink L;
  Ch376 d(L);
  TEST_ASSERT_EQUAL_INT((int)Ch376Err::Ok, (int)d.diskMount());
}

void test_disk_mount_no_disk() {
  MockLink L;
  L.f.diskGone = true;
  Ch376 d(L);
  TEST_ASSERT_EQUAL_INT((int)Ch376Err::NoDisk, (int)d.diskMount());
}

void test_disk_mount_bad_fs() {
  // 0x41: довільний код помилки, не 0x14/0x16/0x82 → непідтримувана ФС.
  MockLink L;
  L.f.mountCode = 0x41;
  Ch376 d(L);
  TEST_ASSERT_EQUAL_INT((int)Ch376Err::BadFs, (int)d.diskMount());
}

// ---------------------------------------------------------------- узгодження швидкості

void test_negotiate_reaches_1000000() {
  MockLink L;
  allSupported(L);
  Ch376 d(L);
  TEST_ASSERT_EQUAL_INT((int)Ch376Err::Ok, (int)d.reset());
  Ch376Attempt att[kCh376BaudTableN];
  size_t n = 0;
  TEST_ASSERT_EQUAL_INT((int)Ch376Err::Ok, (int)d.negotiateBaud(kCh376BaudTable, kCh376BaudTableN, att, kCh376BaudTableN, n));
  TEST_ASSERT_EQUAL_UINT32(1, n);
  TEST_ASSERT_EQUAL_UINT32(1000000, d.baud());
  TEST_ASSERT_EQUAL_UINT32(1000000, L.portBaud);
}

void test_negotiate_fallback_to_230400() {
  // Чіп приймає лише 230400 і 115200: 1 000 000 і 460 800 відкочуються, 230 400 досягається.
  MockLink L;
  L.supported = {230400, 115200};
  Ch376 d(L);
  TEST_ASSERT_EQUAL_INT((int)Ch376Err::Ok, (int)d.reset());
  Ch376Attempt att[kCh376BaudTableN];
  size_t n = 0;
  TEST_ASSERT_EQUAL_INT((int)Ch376Err::Ok, (int)d.negotiateBaud(kCh376BaudTable, kCh376BaudTableN, att, kCh376BaudTableN, n));
  TEST_ASSERT_EQUAL_UINT32(3, n);
  TEST_ASSERT_EQUAL_UINT32(1000000, att[0].baud);
  TEST_ASSERT_EQUAL_INT((int)Ch376Err::Ok, (int)att[2].res);
  TEST_ASSERT_EQUAL_UINT32(230400, d.baud());
  TEST_ASSERT_EQUAL_UINT32(230400, L.portBaud);
}

void test_negotiate_chip_silent_stays_9600() {
  MockLink L;
  allSupported(L);
  L.f.silent = true;
  Ch376 d(L);
  TEST_ASSERT_EQUAL_INT((int)Ch376Err::Ok, (int)d.reset());  // reset не потребує відповіді
  Ch376Attempt att[kCh376BaudTableN];
  size_t n = 0;
  TEST_ASSERT_EQUAL_INT((int)Ch376Err::BaudFail, (int)d.negotiateBaud(kCh376BaudTable, kCh376BaudTableN, att, kCh376BaudTableN, n));
  TEST_ASSERT_EQUAL_UINT32(kCh376BaudTableN, n);
  TEST_ASSERT_EQUAL_UINT32(9600, d.baud());
  TEST_ASSERT_EQUAL_UINT32(9600, L.portBaud);
}

void test_negotiate_no_supported_stays_9600() {
  // Чіп відповідає на SET_BAUDRATE, але не перемикається: кожна спроба відкочується через checkExist.
  MockLink L;
  Ch376 d(L);
  TEST_ASSERT_EQUAL_INT((int)Ch376Err::Ok, (int)d.reset());
  Ch376Attempt att[kCh376BaudTableN];
  size_t n = 0;
  TEST_ASSERT_EQUAL_INT((int)Ch376Err::BaudFail, (int)d.negotiateBaud(kCh376BaudTable, kCh376BaudTableN, att, kCh376BaudTableN, n));
  TEST_ASSERT_EQUAL_UINT32(kCh376BaudTableN, n);
  TEST_ASSERT_EQUAL_UINT32(9600, d.baud());
  TEST_ASSERT_EQUAL_UINT32(9600, L.portBaud);
  TEST_ASSERT_EQUAL_INT((int)Ch376Err::Ok, (int)d.checkExist());  // після відкату працює
}

void test_negotiate_bad_args() {
  MockLink L;
  Ch376 d(L);
  size_t n = 99;
  TEST_ASSERT_EQUAL_INT((int)Ch376Err::BadArg, (int)d.negotiateBaud(nullptr, 0, nullptr, 0, n));
  TEST_ASSERT_EQUAL_UINT32(0, n);
  const Ch376BaudDiv one[1] = {{115200, 0x03, 0xCC}};
  TEST_ASSERT_EQUAL_INT((int)Ch376Err::BadArg, (int)d.negotiateBaud(one, 0, nullptr, 0, n));
}

// ---------------------------------------------------------------- відкат

void test_rollback_hw_reset() {
  // Чіп перемкнувся на 1 000 000 і вийшов із ладу: відкат доходить до апаратного скиду.
  MockLink L;
  L.supported = {1000000};
  L.f.dieAfterBaud = true;
  Ch376 d(L);
  TEST_ASSERT_EQUAL_INT((int)Ch376Err::Ok, (int)d.reset());
  const Ch376BaudDiv one[1] = {{1000000, 0x03, 0xFA}};
  Ch376Attempt att[1];
  size_t n = 0;
  TEST_ASSERT_EQUAL_INT((int)Ch376Err::BaudFail, (int)d.negotiateBaud(one, 1, att, 1, n));
  TEST_ASSERT_EQUAL_INT(1, L.hwResets);
  TEST_ASSERT_EQUAL_UINT32(9600, d.baud());
  TEST_ASSERT_EQUAL_UINT32(9600, L.portBaud);
  TEST_ASSERT_EQUAL_INT((int)Ch376Err::Ok, (int)d.checkExist());
}

void test_rollback_no_rst_gives_baudfail() {
  // Той самий збій без RST: відкат не може відновити чіп, порт лишається на 9600.
  MockLink L;
  L.supported = {1000000};
  L.f.dieAfterBaud = true;
  L.f.rstPresent = false;
  Ch376 d(L);
  TEST_ASSERT_EQUAL_INT((int)Ch376Err::Ok, (int)d.reset());
  const Ch376BaudDiv one[1] = {{1000000, 0x03, 0xFA}};
  Ch376Attempt att[1];
  size_t n = 0;
  TEST_ASSERT_EQUAL_INT((int)Ch376Err::BaudFail, (int)d.negotiateBaud(one, 1, att, 1, n));
  TEST_ASSERT_EQUAL_INT(0, L.hwResets);
  TEST_ASSERT_EQUAL_UINT32(9600, L.portBaud);
  TEST_ASSERT_EQUAL_UINT32(9600, d.baud());
}

// ---------------------------------------------------------------- сміття і помилки в перемиканні

void test_negotiate_with_junk_still_works() {
  MockLink L;
  allSupported(L);
  L.f.junkBefore = 3;
  Ch376 d(L);
  TEST_ASSERT_EQUAL_INT((int)Ch376Err::Ok, (int)d.reset());
  Ch376Attempt att[kCh376BaudTableN];
  size_t n = 0;
  TEST_ASSERT_EQUAL_INT((int)Ch376Err::Ok, (int)d.negotiateBaud(kCh376BaudTable, kCh376BaudTableN, att, kCh376BaudTableN, n));
  TEST_ASSERT_EQUAL_UINT32(1000000, d.baud());
}

void test_reset_twice_after_switch_returns_9600() {
  MockLink L;
  allSupported(L);
  Ch376 d(L);
  TEST_ASSERT_EQUAL_INT((int)Ch376Err::Ok, (int)d.reset());
  const Ch376BaudDiv one[1] = {{460800, 0x03, 0xF3}};
  Ch376Attempt att[1];
  size_t n = 0;
  TEST_ASSERT_EQUAL_INT((int)Ch376Err::Ok, (int)d.negotiateBaud(one, 1, att, 1, n));
  TEST_ASSERT_EQUAL_INT((int)Ch376Err::Ok, (int)d.reset());
  TEST_ASSERT_EQUAL_UINT32(9600, L.chipBaud);
  TEST_ASSERT_EQUAL_UINT32(9600, L.portBaud);
}

// ---------------------------------------------------------------- довідкове

void test_err_names() {
  for (int e = (int)Ch376Err::Ok; e <= (int)Ch376Err::BaudFail; e++) {
    TEST_ASSERT_NOT_NULL(ch376ErrName((Ch376Err)e));
    TEST_ASSERT_TRUE(strlen(ch376ErrName((Ch376Err)e)) > 0);
  }
  TEST_ASSERT_EQUAL_STRING("ok", ch376ErrName(Ch376Err::Ok));
  TEST_ASSERT_EQUAL_STRING("baudfail", ch376ErrName(Ch376Err::BaudFail));
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_bytes_check_exist);
  RUN_TEST(test_bytes_set_usb_mode);
  RUN_TEST(test_bytes_disk_mount);
  RUN_TEST(test_bytes_negotiate_frames_order);
  RUN_TEST(test_reset_to_9600);
  RUN_TEST(test_reset_idempotent);
  RUN_TEST(test_check_exist_ok);
  RUN_TEST(test_check_exist_bad_byte);
  RUN_TEST(test_check_exist_silent);
  RUN_TEST(test_junk_15_before_reply_ok);
  RUN_TEST(test_junk_16_before_reply_bad);
  RUN_TEST(test_mode_ok);
  RUN_TEST(test_mode_bad_code);
  RUN_TEST(test_mode_silent);
  RUN_TEST(test_wait_connect_event);
  RUN_TEST(test_wait_connect_timeout);
  RUN_TEST(test_wait_connect_disconnect);
  RUN_TEST(test_wait_time_budget_3s);
  RUN_TEST(test_vanish_during_wait_gives_timeout);
  RUN_TEST(test_aborted);
  RUN_TEST(test_disk_connect_ok);
  RUN_TEST(test_disk_connect_no_disk);
  RUN_TEST(test_disk_mount_ok);
  RUN_TEST(test_disk_mount_no_disk);
  RUN_TEST(test_disk_mount_bad_fs);
  RUN_TEST(test_negotiate_reaches_1000000);
  RUN_TEST(test_negotiate_fallback_to_230400);
  RUN_TEST(test_negotiate_chip_silent_stays_9600);
  RUN_TEST(test_negotiate_no_supported_stays_9600);
  RUN_TEST(test_negotiate_bad_args);
  RUN_TEST(test_rollback_hw_reset);
  RUN_TEST(test_rollback_no_rst_gives_baudfail);
  RUN_TEST(test_negotiate_with_junk_still_works);
  RUN_TEST(test_reset_twice_after_switch_returns_9600);
  RUN_TEST(test_err_names);
  return UNITY_END();
}

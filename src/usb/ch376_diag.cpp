#include "usb/ch376_diag.h"

#include "config.h"

#if ENABLE_USB_CH376

#include <Arduino.h>
#include <stdlib.h>
#include <strings.h>

#include "usb/ch376.h"
#include "usb/ch376_uart.h"
#include "pins.h"
#include "util/diag.h"
#include "util/log.h"

namespace {

constexpr const char* TAG = "usb";
constexpr long kMaxPing = 50;  // ping блокує консоль: 50 × тайм-аут 50 мс ≤ 2,5 с

Ch376Err s_lastErr = Ch376Err::Ok;
bool s_present = false;  // флешка за останнім init (DISK_MOUNT успішний)

bool toInt(const char* s, long& v, int base) {
  if (!s || !*s) return false;
  char* e = nullptr;
  v = strtol(s, &e, base);
  return *e == 0;
}

void usbHelp() {
  LOGI(TAG, "DBG,usb | DBG,usb,init | DBG,usb,reset | DBG,usb,ping,<N 1..50>");
  LOGI(TAG, "DBG,usb,baud (список) | DBG,usb,baud,<бод>,<K hex>");
  LOGI(TAG, "K hex 00..FF, бод 9600..1000000; формула 6000000/(256-K)");
}

// Стан драйвера без звернення до чіпа.
void usbState() {
  Ch376& d = ch376_uart::driver();
  LOGI(TAG, "usb: запуск=%d швидкість=%u флешка=%d остання=%s подія=0x%02X", ch376_uart::started() ? 1 : 0,
       (unsigned)d.baud(), s_present ? 1 : 0, ch376ErrName(s_lastErr), (unsigned)d.lastEvent());
  LOGI(TAG, "usb: RX=%u Б RST_PIN=%d INT_PIN=%d режим=%s", (unsigned)kCh376RxBuf, (int)CH_RST_PIN, (int)CH_INT_PIN,
       kCh376IntPin ? "INT" : "опитування");
  LOGI(TAG, "usb: версія чіпа не читається (GET_IC_VER не реалізовано)");
}

// Рядок кроку init: номер, назва, результат, тривалість у мкс (≤ ~70 Б).
void usbStep(int n, const char* name, Ch376Err e, uint32_t t0) {
  s_lastErr = e;
  LOGI(TAG, "init %d %s: %s, %u мкс", n, name, ch376ErrName(e), (unsigned)(micros() - t0));
}

// Повна послідовність: скид → CHECK_EXIST → режим host → підключення (≤3 с) → DISK_CONNECT → DISK_MOUNT.
// DISK_CONNECT додано понад план: DISK_MOUNT без нього, за типовою схемою CH376, не працює (середня впевненість).
void usbInit() {
  ch376_uart::begin();
  Ch376& d = ch376_uart::driver();
  s_present = false;
  uint32_t t0 = micros();
  Ch376Err e = d.reset();
  usbStep(1, "reset", e, t0);
  if (e == Ch376Err::Ok) {
    t0 = micros();
    e = d.checkExist();
    usbStep(2, "check", e, t0);
  }
  if (e == Ch376Err::Ok) {
    t0 = micros();
    e = d.setUsbMode(kCh376ModeHost);
    usbStep(3, "host", e, t0);
  }
  if (e == Ch376Err::Ok) {
    t0 = micros();
    e = d.waitConnect(kCh376ConnectWaitMs);
    usbStep(4, "connect", e, t0);
  }
  if (e == Ch376Err::Ok) {
    t0 = micros();
    e = d.diskConnect();
    usbStep(5, "dconn", e, t0);
  }
  if (e == Ch376Err::Ok) {
    t0 = micros();
    e = d.diskMount();
    usbStep(6, "mount", e, t0);
    s_present = (e == Ch376Err::Ok);
  }
  LOGI(TAG, "init: %s", e == Ch376Err::Ok ? "готово" : "зупинено на помилці");
  Diag::logHeap("usb");
}

void resetCmd() {
  ch376_uart::begin();
  Ch376& d = ch376_uart::driver();
  uint32_t t0 = micros();
  Ch376Err e = d.reset();
  if (e == Ch376Err::Ok) e = d.checkExist();
  s_lastErr = e;
  LOGI(TAG, "reset: %s, порт %u Бод, %u мкс", ch376ErrName(e), (unsigned)d.baud(), (unsigned)(micros() - t0));
}

// Оцінка затримки командного обміну: N разів CHECK_EXIST, середнє і максимум успішних.
void usbPing(long n) {
  ch376_uart::begin();
  Ch376& d = ch376_uart::driver();
  uint32_t sum = 0, mx = 0;
  long ok = 0;
  for (long i = 0; i < n; i++) {
    uint32_t t0 = micros();
    Ch376Err e = d.checkExist();
    uint32_t dt = (uint32_t)(micros() - t0);
    s_lastErr = e;
    if (e == Ch376Err::Ok) {
      ok++;
      sum += dt;
      if (dt > mx) mx = dt;
    }
  }
  LOGI(TAG, "ping: ok %ld/%ld, сер. %u мкс, макс. %u мкс", ok, n, ok ? (unsigned)(sum / (uint32_t)ok) : 0u,
       (unsigned)mx);
  if (ok < n) LOGI(TAG, "ping: остання помилка %s", ch376ErrName(s_lastErr));
}

// Перебір таблиці kCh376BaudTable після скиду. Кожна спроба — рядок лога з K.
void baudAll() {
  ch376_uart::begin();
  Ch376& d = ch376_uart::driver();
  Ch376Err e = d.reset();
  if (e != Ch376Err::Ok) {
    s_lastErr = e;
    LOGI(TAG, "baud: скид не вдався (%s)", ch376ErrName(e));
    return;
  }
  Ch376Attempt att[kCh376BaudTableN];
  size_t nAtt = 0;
  e = d.negotiateBaud(kCh376BaudTable, kCh376BaudTableN, att, kCh376BaudTableN, nAtt);
  for (size_t i = 0; i < nAtt && i < kCh376BaudTableN; i++) {
    LOGI(TAG, "baud %u K=%02X: %s", (unsigned)att[i].baud, (unsigned)kCh376BaudTable[i].hi, ch376ErrName(att[i].res));
  }
  s_lastErr = e;
  LOGI(TAG, "baud: итог %u Бод (%s), спроб %u", (unsigned)d.baud(), ch376ErrName(e), (unsigned)nAtt);
}

// Одна явна пара (бод, K) для підбору дільника на залізі. Той самий шлях negotiateBaud з одним елементом.
void baudOne(uint32_t bod, uint8_t k) {
  ch376_uart::begin();
  Ch376& d = ch376_uart::driver();
  Ch376Err e = d.reset();
  Ch376Err res = e;  // результат самої спроби (без відкату); для reset — його результат
  if (e == Ch376Err::Ok) {
    Ch376BaudDiv t{bod, kCh376BaudDivPrefix, k};
    Ch376Attempt att[1];
    size_t n = 0;
    e = d.negotiateBaud(&t, 1, att, 1, n);
    res = att[0].res;
  }
  s_lastErr = res;
  LOGI(TAG, "baud1 %u K=%02X: %s, порт %u Бод", (unsigned)bod, (unsigned)k, ch376ErrName(res), (unsigned)d.baud());
}

}  // namespace

void Ch376Diag::handle(const proto::Msg& m) {
  if (m.argc < 2) {
    usbState();
    return;
  }
  const char* s = m.args[1];
  if (!strcasecmp(s, "init") && m.argc == 2) {
    usbInit();
  } else if (!strcasecmp(s, "reset") && m.argc == 2) {
    resetCmd();
  } else if (!strcasecmp(s, "ping") && m.argc == 3) {
    long n = 0;
    if (!toInt(m.args[2], n, 10) || n < 1 || n > kMaxPing) {
      LOGW(TAG, "DBG,usb,ping,<1..50>");
      return;
    }
    usbPing(n);
  } else if (!strcasecmp(s, "baud") && m.argc == 2) {
    baudAll();
  } else if (!strcasecmp(s, "baud") && m.argc == 4) {
    long bod = 0, k = 0;
    if (!toInt(m.args[2], bod, 10) || bod < 9600 || bod > 1000000 || !toInt(m.args[3], k, 16) || k < 0 || k > 255) {
      LOGW(TAG, "DBG,usb,baud,<9600..1000000>,<K hex 00..FF>");
      return;
    }
    baudOne((uint32_t)bod, (uint8_t)k);
  } else {
    usbHelp();
  }
}

#endif  // ENABLE_USB_CH376
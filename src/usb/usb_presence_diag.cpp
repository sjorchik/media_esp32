#include "usb/usb_presence_diag.h"

#include "config.h"

#if ENABLE_USB_CH376

#include <Arduino.h>
#include <esp_heap_caps.h>
#include <strings.h>

#include "pins.h"
#include "usb/ch376_uart.h"
#include "usb/usb_chip_uart.h"
#include "usb/usb_presence.h"
#include "util/log.h"

namespace {
constexpr const char* TAG = "usb";

uint32_t freeHeap() { return (uint32_t)heap_caps_get_free_size(MALLOC_CAP_8BIT); }

// (a) повна ініціалізація, як у фоні: RESET_ALL → CHECK_EXIST → 2 Мбод (резерв 1 Мбод) → host; далі паркування.
void park() {
  UsbPresence::setManual(true);
  const uint32_t h0 = freeHeap();
  if (!ch376_uart::started()) ch376_uart::begin();
  Ch376& d = ch376_uart::driver();
  Ch376Err e = d.checkExist();
  if (e != Ch376Err::Ok) {
    d.rollback(kCh376DefaultBaud);
    e = d.checkExist();
  }
  if (e == Ch376Err::Ok) e = d.reset();
  if (e == Ch376Err::Ok) e = d.checkExist();
  const uint32_t h1 = freeHeap();
  if (e == Ch376Err::Ok) {
    Ch376Err b = uartChip().setBaud(kUsbBaudFast);
    if (b != Ch376Err::Ok) b = uartChip().setBaud(kUsbBaudRef);
    LOGI(TAG, "park: швидкість → %u Бод (%s)", (unsigned)d.baud(), ch376ErrName(b));
    e = d.setUsbMode(kCh376ModeHost);
  }
  if (e != Ch376Err::Ok) {
    LOGW(TAG, "park: ініціалізація не вдалась (%s); порт закрито без паркування", ch376ErrName(e));
    ch376_uart::stop();
    return;
  }
  uint8_t st = kCh376IntNone;
  d.getStatus(st);  // забрати подію підключення, якщо флешка вже стоїть (INT# → HIGH)
  ch376_uart::park();
  const uint32_t h2 = freeHeap();
  LOGI(TAG, "park: чіп host=%d на %u Бод, UART2 вимкнено; heap free: до %u, з драйвером %u, запарковано %u Б; статус при паркуванні 0x%02X; INT#=%d",
       d.host() ? 1 : 0, (unsigned)d.baud(), (unsigned)h0, (unsigned)h1, (unsigned)h2, (unsigned)st, uartChip().intLow() ? 0 : 1);
  LOGI(TAG, "park: тепер вставте/витягніть флешку й дивіться DBG,usb,int; фонова присутність призупинена (DBG,usb,pres,auto — відновити)");
}

// (c)(d)(e) wake: порт на збереженій швидкості без нового узгодження; сміття в RX; CHECK_EXIST; GET_STATUS.
void wake() {
  UsbPresence::setManual(true);
  if (ch376_uart::started()) {
    LOGW(TAG, "wake: порт уже відкрито (спершу DBG,usb,park)");
    return;
  }
  const uint32_t h0 = freeHeap();
  const uint32_t t0 = micros();
  ch376_uart::begin();
  const uint32_t tBegin = micros() - t0;
  const uint32_t h1 = freeHeap();
  const int junk = Serial2.available();  // (d) байти в RX відразу після begin
  Ch376& d = ch376_uart::driver();
  const uint32_t t1 = micros();
  const Ch376Err e = d.checkExist();
  const uint32_t tPing = micros() - t1;
  uint8_t st = kCh376IntNone;
  const uint32_t t2 = micros();
  const Ch376Err es = e == Ch376Err::Ok ? d.getStatus(st) : e;
  const uint32_t tSt = micros() - t2;
  LOGI(TAG, "wake: begin на %u Бод за %u мкс; heap free %u → %u Б (−%u); сміття в RX після begin: %d Б", (unsigned)d.baud(), (unsigned)tBegin, (unsigned)h0,
       (unsigned)h1, (unsigned)(h0 > h1 ? h0 - h1 : 0), junk);
  LOGI(TAG, "wake: CHECK_EXIST %s (%u мкс); GET_STATUS %s подія=0x%02X (%u мкс); INT#=%d; усього %u мкс", ch376ErrName(e), (unsigned)tPing, ch376ErrName(es),
       (unsigned)st, (unsigned)tSt, uartChip().intLow() ? 0 : 1, (unsigned)(micros() - t0));
  LOGI(TAG, "wake: порт лишився відкритим; DBG,usb,park повторно ініціалізує й паркує, DBG,usb,stop скидає чіп на 9600");
}

// (b) INT#: рівень і лічильник спадних фронтів з ISR; ,reset — обнулити відлік (запамʼятати поточний).
uint32_t s_edgesBase = 0;
void intInfo(bool reset) {
  const uint32_t n = uartChip().intEdges();
  if (reset) s_edgesBase = n;
  LOGI(TAG, "int: INT# GPIO%d = %s (LOW = подія чіпа), спадних фронтів з ISR: %u (від скидання відліку: %u), UART2 %s", (int)CH_INT_PIN,
       uartChip().intLow() ? "LOW" : "HIGH", (unsigned)n, (unsigned)(n - s_edgesBase), ch376_uart::started() ? "відкрито" : "вимкнено");
}
}  // namespace

namespace UsbPresDiag {

bool handle(const proto::Msg& m) {
  if (m.argc < 2) return false;
  const char* s = m.args[1];
  if (!strcasecmp(s, "park") && m.argc == 2) {
    park();
  } else if (!strcasecmp(s, "wake") && m.argc == 2) {
    wake();
  } else if (!strcasecmp(s, "int") && (m.argc == 2 || (m.argc == 3 && !strcasecmp(m.args[2], "reset")))) {
    intInfo(m.argc == 3);
  } else if (!strcasecmp(s, "pres")) {
    if (m.argc == 3 && !strcasecmp(m.args[2], "auto")) {
      UsbPresence::setManual(false);
      LOGI(TAG, "pres: фонова присутність відновлена");
    } else if (m.argc == 3 && !strcasecmp(m.args[2], "init")) {
      UsbPresence::setManual(false);
      UsbPresence::restartInit();
      LOGI(TAG, "pres: повторна ініціалізація чіпа запланована");
    } else if (m.argc != 2) {
      LOGW(TAG, "DBG,usb,pres[,auto|init]");
    }
    UsbPresence::dbgPrint();
  } else {
    return false;
  }
  return true;
}

}  // namespace UsbPresDiag

#endif  // ENABLE_USB_CH376
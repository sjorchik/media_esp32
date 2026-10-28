#include "usb/ch376_diag.h"

#include "config.h"

#if ENABLE_USB_CH376

#include <Arduino.h>
#include <stdio.h>
#include <stdlib.h>
#include <strings.h>

#include "pins.h"
#include "usb/ch376.h"
#include "usb/ch376_uart.h"
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

// Текст для логів: рівень INT, перший байт відповіді, сирі байти.
void intText(int v, char* b, size_t cap) {
  if (v < 0) snprintf(b, cap, "-");
  else snprintf(b, cap, "%d", v);
}
void rxText(int rx, char* b, size_t cap) {
  if (rx < 0) snprintf(b, cap, "--");
  else snprintf(b, cap, "%02X", (unsigned)rx);
}
void hexText(const uint8_t* d, size_t n, char* b, size_t cap) {
  if (n == 0) {
    snprintf(b, cap, "-");
    return;
  }
  size_t p = 0;
  for (size_t i = 0; i < n && p + 4 < cap; i++) {
    int w = snprintf(b + p, cap - p, i ? " %02X" : "%02X", (unsigned)d[i]);
    if (w < 0) break;
    p += (size_t)w;
  }
}

void usbHelp() {
  LOGI(TAG, "DBG,usb | init | reset | ping,<N 1..50> | stop");
  LOGI(TAG, "baud | baud,<бод>,<K hex> | baudraw,<C hex>,<K hex>[,<бод>]");
  LOGI(TAG, "K hex 00..FF, швидкість 6000000/(256-K), C — перший байт (звичайно 03)");
}

// Стан драйвера без звернення до чіпа.
void usbState() {
  Ch376& d = ch376_uart::driver();
  char ib[8];
  intText(ch376_uart::intLevel(), ib, sizeof ib);
  LOGI(TAG, "usb: запуск=%d швидкість=%u флешка=%d остання=%s подія=0x%02X", ch376_uart::started() ? 1 : 0,
       (unsigned)d.baud(), s_present ? 1 : 0, ch376ErrName(s_lastErr), (unsigned)d.lastEvent());
  LOGI(TAG, "usb: RX=%u Б RST_PIN=%d INT_PIN=%d INT=%s режим=%s", (unsigned)kCh376RxBuf, (int)CH_RST_PIN,
       (int)CH_INT_PIN, ib, kCh376IntPin ? "INT" : "опитування");
  LOGI(TAG, "usb: втрачено=%d відкат_на=%u", d.chipLost() ? 1 : 0, (unsigned)d.recoveredAt());
}

// Рядок кроку init (без INT).
void usbStep(int n, const char* name, Ch376Err e, uint32_t t0) {
  s_lastErr = e;
  LOGI(TAG, "init %d %s: %s, %u мкс", n, name, ch376ErrName(e), (unsigned)(micros() - t0));
}

// Рядок кроку init з рівнем INT до і після кроку.
void usbStepI(int n, const char* name, Ch376Err e, uint32_t t0, int i0, int i1) {
  s_lastErr = e;
  char a[8], b[8];
  intText(i0, a, sizeof a);
  intText(i1, b, sizeof b);
  LOGI(TAG, "init %d %s: %s, %u мкс, INT %s>%s", n, name, ch376ErrName(e), (unsigned)(micros() - t0), a, b);
}

// Повна послідовність: скид → CHECK_EXIST → режим host → підключення (≤3 с) → DISK_CONNECT → DISK_MOUNT.
void usbInit() {
  ch376_uart::begin();
  Ch376& d = ch376_uart::driver();
  s_present = false;
  Ch376Err e = Ch376Err::Ok;
  uint32_t t0 = micros();
  e = d.reset();
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
    const int i0 = ch376_uart::intLevel();
    e = d.waitConnect(kCh376ConnectWaitMs);
    usbStepI(4, "connect", e, t0, i0, ch376_uart::intLevel());
  }
  if (e == Ch376Err::Ok) {
    t0 = micros();
    e = d.diskConnect();
    usbStep(5, "dconn", e, t0);
  }
  if (e == Ch376Err::Ok) {
    t0 = micros();
    const int i0 = ch376_uart::intLevel();
    e = d.diskMount();
    usbStepI(6, "mount", e, t0, i0, ch376_uart::intLevel());
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

// Затримка командного обміну: N × CHECK_EXIST, середнє і максимум успішних.
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

// Підсумок відкату: або втрата чіпа (помітне попередження), або на якій швидкості знайдено.
void usbRecovery(const Ch376& d) {
  if (d.chipLost()) {
    LOGW(TAG, "ЧІП ВТРАЧЕНО: вимкни живлення модуля (порт 9600)");
    return;
  }
  if (d.recoveredAt()) LOGI(TAG, "відкат: чіп знайдено на %u, повернуто на 9600", (unsigned)d.recoveredAt());
}

// Перебір таблиці kCh376NegotiateTable після скиду; у кожній спробі — сирий перший байт відповіді.
void usbBaudAll() {
  ch376_uart::begin();
  Ch376& d = ch376_uart::driver();
  Ch376Err e = d.reset();
  if (e != Ch376Err::Ok) {
    s_lastErr = e;
    LOGI(TAG, "baud: скид не вдався (%s)", ch376ErrName(e));
    return;
  }
  Ch376Attempt att[kCh376NegotiateN];
  size_t nAtt = 0;
  e = d.negotiateBaud(kCh376NegotiateTable, kCh376NegotiateN, att, kCh376NegotiateN, nAtt);
  for (size_t i = 0; i < nAtt && i < kCh376NegotiateN; i++) {
    char rb[8];
    rxText(att[i].rx, rb, sizeof rb);
    LOGI(TAG, "baud %u K=%02X: %s, rx=%s", (unsigned)att[i].baud, (unsigned)kCh376NegotiateTable[i].hi,
         ch376ErrName(att[i].res), rb);
  }
  usbRecovery(d);
  s_lastErr = e;
  LOGI(TAG, "baud: итог %u Бод (%s), спроб %u", (unsigned)d.baud(), ch376ErrName(e), (unsigned)nAtt);
}

// Одна спроба з явною парою (бод, K): reset → спроба → відкат, з сирим байтом.
void usbBaudOne(uint32_t bod, uint8_t k) {
  ch376_uart::begin();
  Ch376& d = ch376_uart::driver();
  Ch376Err e = d.reset();
  int rx = -1;
  Ch376Err res = e;
  if (e == Ch376Err::Ok) {
    Ch376BaudDiv t{bod, kCh376BaudDivPrefix, k};
    Ch376Attempt att[1];
    size_t n = 0;
    e = d.negotiateBaud(&t, 1, att, 1, n);
    res = att[0].res;
    rx = att[0].rx;
  }
  s_lastErr = res;
  char rb[8];
  rxText(rx, rb, sizeof rb);
  LOGI(TAG, "baud1 %u K=%02X: %s, rx=%s, порт %u Бод", (unsigned)bod, (unsigned)k, ch376ErrName(res), rb,
       (unsigned)d.baud());
  usbRecovery(d);
}

// Сирий обмін для з'ясування, що відповідає чіп на SET_BAUDRATE.
// A — відповідь на поточній швидкості (9600); B — після перемикання порту на newBod.
// Після кожної спроби — програмний відкат.
void usbBaudRaw(uint8_t c, uint8_t k, uint32_t newBod) {
  ch376_uart::begin();
  Ch376& d = ch376_uart::driver();
  const uint32_t tried = newBod ? newBod : 6000000u / (256u - k);
  if (d.portTo(kCh376DefaultBaud) != Ch376Err::Ok) {
    LOGW(TAG, "baudraw: порт не прийняв 9600");
    return;
  }
  const uint8_t args[2] = {c, k};
  uint8_t buf[16];
  size_t n = 0;
  char hb[64];
  if (newBod == 0) {
    n = d.rawExchange(kCh376CmdSetBaud, args, 2, kCh376RawWinMs, buf, sizeof buf);
    hexText(buf, n < sizeof buf ? n : sizeof buf, hb, sizeof hb);
    LOGI(TAG, "raw A 02 %02X %02X на 9600: %u байт, %s", (unsigned)c, (unsigned)k, (unsigned)n, hb);
  } else {
    n = d.rawSwitchExchange(kCh376CmdSetBaud, args, 2, newBod, kCh376RawWinMs, buf, sizeof buf);
    hexText(buf, n < sizeof buf ? n : sizeof buf, hb, sizeof hb);
    LOGI(TAG, "raw B 02 %02X %02X, порт %u: %u байт, %s", (unsigned)c, (unsigned)k, (unsigned)newBod, (unsigned)n,
         hb);
  }
  d.rollback(tried);
  if (d.chipLost() || d.recoveredAt()) {
    usbRecovery(d);
  } else {
    LOGI(TAG, "відкат: чіп на 9600");
  }
  s_lastErr = d.chipLost() ? Ch376Err::BaudFail : Ch376Err::Ok;
}

// Зупинка драйвера: закрити Serial2 (звільнити RX-буфер) і показати heap.
void usbStop() {
  ch376_uart::stop();
  s_present = false;
  Diag::logHeap("usb-stop");
  LOGI(TAG, "stop: драйвер зупинено, порт закрито");
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
    usbBaudAll();
  } else if (!strcasecmp(s, "baud") && m.argc == 4) {
    long bod = 0, k = 0;
    if (!toInt(m.args[2], bod, 10) || bod < 9600 || bod > 2000000 || !toInt(m.args[3], k, 16) || k < 0 || k > 255) {
      LOGW(TAG, "DBG,usb,baud,<9600..2000000>,<K hex 00..FF>");
      return;
    }
    usbBaudOne((uint32_t)bod, (uint8_t)k);
  } else if (!strcasecmp(s, "baudraw") && (m.argc == 4 || m.argc == 5)) {
    long c = 0, k = 0, nb = 0;
    if (!toInt(m.args[2], c, 16) || c < 0 || c > 255 || !toInt(m.args[3], k, 16) || k < 0 || k > 255 ||
        (m.argc == 5 && (!toInt(m.args[4], nb, 10) || nb < 9600 || nb > 2000000))) {
      LOGW(TAG, "DBG,usb,baudraw,<C hex>,<K hex>[,<бод 9600..2000000>]");
      return;
    }
    usbBaudRaw((uint8_t)c, (uint8_t)k, (uint32_t)nb);
  } else if (!strcasecmp(s, "stop") && m.argc == 2) {
    usbStop();
  } else {
    usbHelp();
  }
}

#endif  // ENABLE_USB_CH376

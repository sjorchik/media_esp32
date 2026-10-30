#include "usb/ch376_diag.h"

#include "config.h"

#if ENABLE_USB_CH376

#include <Arduino.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "pins.h"
#include "usb/ch376.h"
#include "usb/ch376_fs.h"
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
  LOGI(TAG, "DBG,usb | init[,<бод>|auto] | reset | ping,<N 1..50> | stop");
  LOGI(TAG, "baud | baud,<бод>,<K hex> | baudraw,<C hex>,<K hex>[,<бод>]");
  LOGI(TAG, "trace,on|off|dump | gap[,<мкс>] | fopen,<імʼя> | cmd,<hex>[,<hex арг>] | mode,<4..7>");
  LOGI(TAG, "ls[,<каталог>] | read,<шлях>[,<КБ>] | seek,<шлях>,<зсув>[,<N>] | seq,<шлях>[,<КБ>] | wait[,int|poll]");
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

void usbRecovery(const Ch376& d);

// Швидкість, дозволена для init (лише перевірені на залізі: kCh376NegotiateTable).
bool negotiable(long b) {
  for (size_t i = 0; i < kCh376NegotiateN; i++)
    if ((long)kCh376NegotiateTable[i].baud == b) return true;
  return false;
}

// Узгодження швидкості (M4b1) одразу після CHECK_EXIST, ДО режиму host: порядок, перевірений у DBG,usb,baud (M4a2).
// baudArg < 0 — список kCh376NegotiateTable (1000000 → 460800 → 115200), інакше одна задана швидкість.
Ch376Err usbInitBaud(Ch376& d, long baudArg) {
  Ch376BaudDiv one{0, 0, 0};
  const Ch376BaudDiv* tab = kCh376NegotiateTable;
  size_t n = kCh376NegotiateN;
  if (baudArg > 0) {
    for (size_t i = 0; i < kCh376NegotiateN; i++)
      if ((long)kCh376NegotiateTable[i].baud == baudArg) one = kCh376NegotiateTable[i];
    tab = &one;
    n = 1;
  }
  Ch376Attempt att[kCh376NegotiateN];
  size_t nAtt = 0;
  const uint32_t t0 = micros();
  Ch376Err e = d.negotiateBaud(tab, n, att, kCh376NegotiateN, nAtt);
  s_lastErr = e;
  LOGI(TAG, "init 2b baud: %s, %u Бод, спроб %u, %u мкс", ch376ErrName(e), (unsigned)d.baud(), (unsigned)nAtt,
       (unsigned)(micros() - t0));
  usbRecovery(d);
  return e;
}

// Повна послідовність: скид → CHECK_EXIST → [швидкість] → режим host → підключення (≤3 с) → DISK_CONNECT → DISK_MOUNT.
// baudArg: 0 — лишити 9600 (як у M4a), -1 — auto, інакше конкретна швидкість зі списку.
void usbInit(long baudArg) {
  ch376_uart::begin();
  Ch376& d = ch376_uart::driver();
  s_present = false;
  {  // файловий шар: скид чіпа закриває файл; режим очікування (INT/опитування) зберігається
    const bool m = ch376_uart::fs().useInt();
    const uint32_t g = ch376_uart::fs().gapUs();
    ch376_uart::fs().reset();
    ch376_uart::fs().setUseInt(m);
    ch376_uart::fs().setGapUs(g);
    ch376_uart::reader().reset();
  }
  Ch376Err e = Ch376Err::Ok;
  uint32_t t0 = micros();
  e = d.reset();
  usbStep(1, "reset", e, t0);
  if (e == Ch376Err::Ok) {
    t0 = micros();
    e = d.checkExist();
    usbStep(2, "check", e, t0);
    if (e != Ch376Err::Ok) {  // чіп міг лишитись на швидкості попереднього сеансу (перезапуск ESP, stop без скиду): відкат і одна повторна спроба
      d.rollback(kCh376DefaultBaud);
      usbRecovery(d);
      t0 = micros();
      e = d.reset();
      if (e == Ch376Err::Ok) e = d.checkExist();
      usbStep(2, "check (повтор після відкату)", e, t0);
    }
  }
  if (e == Ch376Err::Ok && baudArg != 0) e = usbInitBaud(d, baudArg);
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
  s_present = false;  // скид чіпа знімає mount
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
  s_present = false;  // скид чіпа знімає mount
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
  s_present = false;  // скид чіпа знімає mount
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

// ---------------------------------------------------------------- M4b1: файловий шар (діагностика)

// Потрібна змонтована флешка (після DBG,usb,init). Драйвер файловий шар сам не запускає.
bool needDisk() {
  if (!ch376_uart::started() || !s_present) {
    LOGW(TAG, "флешка не змонтована: спершу DBG,usb,init[,<бод>|auto]");
    return false;
  }
  return true;
}

void fsFail(const char* what, FsErr e) {
  LOGW(TAG, "%s: %s (статус 0x%02X)", what, ch376FsErrName(e), (unsigned)ch376_uart::fs().lastStatus());
  if (e == FsErr::Disconnected) {
    s_present = false;
    LOGW(TAG, "флешку відʼєднано; після повернення — DBG,usb,init");
  }
}

const char* waitMode() { return ch376_uart::fs().useInt() && ch376_uart::fs().intUsable() ? "INT" : "опитування"; }

// КБ/с ×10 за байтами й мкс.
uint32_t kbps10(uint32_t bytes, uint32_t us) {
  return us ? (uint32_t)((uint64_t)bytes * 10000000ull / (1024ull * us)) : 0u;
}

bool lsCb(const Ch376DirEntry& e, void*) {
  if (e.isDir()) LOGI(TAG, " D %-12s <DIR>", e.name);
  else LOGI(TAG, " F %-12s %10u Б", e.name, (unsigned)e.size);
  return true;
}

void usbLs(const char* dir) {
  if (!needDisk()) return;
  uint16_t n = 0;
  bool more = false;
  const uint32_t t0 = micros();
  LOGI(TAG, "ls %s:", dir);
  FsErr e = ch376_uart::fs().list(dir, lsCb, nullptr, kCh376DiagLsMax, n, more);
  const uint32_t dt = (uint32_t)(micros() - t0);
  s_lastErr = Ch376Err::Ok;
  if (e != FsErr::Ok) {
    fsFail("ls", e);
    return;
  }
  LOGI(TAG, "ls: записів %u%s, %u мкс (разом з виводом)", (unsigned)n, more ? ", ОБРІЗАНО (є ще, ліміт 64)" : "", (unsigned)dt);
}

// Вимір пропускної здатності: читання з початку файлу порціями, без обробки даних (read(nullptr)).
void usbRead(const char* path, uint32_t kb) {
  if (!needDisk()) return;
  Ch376& d = ch376_uart::driver();
  Ch376Fs& fs = ch376_uart::fs();
  Ch376FileReader& r = ch376_uart::reader();
  Diag::logHeap("read-до");
  const unsigned stk0 = (unsigned)uxTaskGetStackHighWaterMark(nullptr);
  fs.resetStats();
  if (!r.open(path)) {
    fsFail("open", r.lastErr());
    return;
  }
  const uint64_t want = (uint64_t)kb * 1024ull;
  const uint32_t target = want > r.size() ? r.size() : (uint32_t)want;
  LOGI(TAG, "read %s: розмір %u Б, читаю %u Б, %u Бод, очікування: %s", path, (unsigned)r.size(), (unsigned)target,
       (unsigned)d.baud(), waitMode());
  const size_t step = d.baud() >= 115200 ? kCh376DiagChunk : 512;  // на 9600 порція менша, щоб вкластись у стелю часу
  uint32_t got = 0;
  bool cut = false;
  const uint32_t t0 = micros();
  while (got < target) {
    if ((uint32_t)(micros() - t0) >= kCh376DiagMeasMs * 1000u) {
      cut = true;
      break;
    }
    const size_t want1 = target - got < step ? (size_t)(target - got) : step;
    const size_t n = r.read(nullptr, want1);
    got += (uint32_t)n;
    if (n < want1) break;
  }
  const uint32_t dt = (uint32_t)(micros() - t0);
  const FsErr err = r.lastErr();
  const bool bad = r.failed();
  r.close();
  const uint32_t k10 = kbps10(got, dt);
  const Ch376FsStats& s = fs.stats();
  LOGI(TAG, "read: %u Б за %u мкс = %u.%u КБ/с%s", (unsigned)got, (unsigned)dt, (unsigned)(k10 / 10), (unsigned)(k10 % 10),
       cut ? " (ПЕРЕРВАНО за стелею часу)" : "");
  if (s.blocks) {
    LOGI(TAG, "read: блок мкс мін/сер/макс %u/%u/%u, блоків %u (сер. %u Б)", (unsigned)s.blkMinUs,
         (unsigned)(s.blkSumUs / s.blocks), (unsigned)s.blkMaxUs, (unsigned)s.blocks, (unsigned)(s.bytes / s.blocks));
    LOGI(TAG, "read: на блок сер.: очікування %u + статус %u + дані %u мкс", (unsigned)(s.waitUs / s.blocks),
         (unsigned)(s.statUs / s.blocks), (unsigned)(s.xferUs / s.blocks));
  }
  LOGI(TAG, "read: polls=%u INT=%u без_INT=%u помилок=%u повторів=%u", (unsigned)s.polls, (unsigned)s.intWaits,
       (unsigned)s.intMisses, (unsigned)s.errors, (unsigned)s.retries);
  if (bad || (got < target && !cut)) fsFail("read", err);
  Diag::logHeap("read-після");
  LOGI(TAG, "read: стек console, мін. вільно: до %u, після %u Б", stk0, (unsigned)uxTaskGetStackHighWaterMark(nullptr));
}

// Seek і читання n байтів (перші 16 показуються): навігація FF/FR і резюме.
void usbSeek(const char* path, uint32_t off, uint32_t n) {
  if (!needDisk()) return;
  Ch376FileReader& r = ch376_uart::reader();
  if (!r.open(path)) {
    fsFail("open", r.lastErr());
    return;
  }
  if (off > r.size()) {
    LOGW(TAG, "зсув %u більший за розмір %u", (unsigned)off, (unsigned)r.size());
    r.close();
    return;
  }
  uint32_t t0 = micros();
  const bool ok = r.seek(off);
  const uint32_t dts = (uint32_t)(micros() - t0);
  LOGI(TAG, "seek %s: розмір %u, до %u: %s, %u мкс", path, (unsigned)r.size(), (unsigned)off, ok ? "ok" : "ПОМИЛКА",
       (unsigned)dts);
  if (!ok) {
    fsFail("seek", r.lastErr());
    return;
  }
  uint8_t b[16];
  const size_t k = n < sizeof b ? (size_t)n : sizeof b;
  t0 = micros();
  size_t got = r.read(b, k);
  const size_t head = got;
  if (n > k && got == k) got += r.read(nullptr, n - k);
  const uint32_t dtr = (uint32_t)(micros() - t0);
  char hb[16 * 3 + 1];
  hexText(b, head, hb, sizeof hb);
  const uint32_t k10 = kbps10((uint32_t)got, dtr);
  LOGI(TAG, "seek: прочитано %u з %u Б за %u мкс = %u.%u КБ/с, поз. %u", (unsigned)got, (unsigned)n, (unsigned)dtr,
       (unsigned)(k10 / 10), (unsigned)(k10 % 10), (unsigned)r.pos());
  LOGI(TAG, "seek: перші байти: %s", hb);
  if (r.failed()) fsFail("read", r.lastErr());
  r.close();
}

// Читання «як у плеєра»: шматок ≈1,1 КБ на кадр MP3 320 кбіт/с (період 26,1 мс). Між шматками — очікування кадру.
void usbSeq(const char* path, uint32_t kb) {
  if (!needDisk()) return;
  Ch376Fs& fs = ch376_uart::fs();
  Ch376FileReader& r = ch376_uart::reader();
  fs.resetStats();
  if (!r.open(path)) {
    fsFail("open", r.lastErr());
    return;
  }
  const uint64_t want = (uint64_t)kb * 1024ull;
  const uint32_t target = want > r.size() ? r.size() : (uint32_t)want;
  LOGI(TAG, "seq %s: %u Б, шматок %u Б / %u мкс (≈40 КБ/с), %u Бод, очікування: %s", path, (unsigned)target,
       (unsigned)kCh376SeqChunk, (unsigned)kCh376SeqPeriodUs, (unsigned)ch376_uart::driver().baud(), waitMode());
  uint32_t got = 0, k = 0, worst = 0, sum = 0, over = 0, late = 0;
  int32_t maxLag = 0;
  bool cut = false;
  const uint32_t t0 = micros();
  while (got < target) {
    const uint32_t due = k * kCh376SeqPeriodUs;  // коли плеєр попросив би цей шматок
    for (;;) {
      const uint32_t el = (uint32_t)(micros() - t0);
      if (el >= due) break;
      if (due - el > 1500) delay(1);
    }
    const size_t want1 = target - got < kCh376SeqChunk ? (size_t)(target - got) : kCh376SeqChunk;
    const uint32_t ts = micros();
    const size_t n = r.read(nullptr, want1);
    const uint32_t te = micros();
    const uint32_t dt = (uint32_t)(te - ts);
    if (dt > worst) worst = dt;
    if (dt > kCh376SeqPeriodUs) over++;
    sum += dt;
    got += (uint32_t)n;
    k++;
    const int32_t lag = (int32_t)((uint32_t)(te - t0) - k * kCh376SeqPeriodUs);  // >0: шматок готовий пізніше за початок наступного кадру
    if (lag > maxLag) maxLag = lag;
    if (lag > 0) late++;
    if (n < want1) break;
    if ((uint32_t)(te - t0) >= kCh376DiagMeasMs * 1000u) {
      cut = true;
      break;
    }
  }
  const uint32_t tot = (uint32_t)(micros() - t0);
  const FsErr err = r.lastErr();
  const bool bad = r.failed();
  r.close();
  const Ch376FsStats& s = fs.stats();
  LOGI(TAG, "seq: шматків %u, %u Б за %u мс%s", (unsigned)k, (unsigned)got, (unsigned)(tot / 1000),
       cut ? " (ПЕРЕРВАНО за стелею часу)" : "");
  LOGI(TAG, "seq: час шматка сер/макс %u/%u мкс (період %u), довших за період: %u", (unsigned)(k ? sum / k : 0),
       (unsigned)worst, (unsigned)kCh376SeqPeriodUs, (unsigned)over);
  LOGI(TAG, "seq: макс. відставання %d мкс (допуск %u), запізнілих шматків %u, помилок %u, повторів %u", (int)maxLag,
       (unsigned)kCh376SeqLagOkUs, (unsigned)late, (unsigned)s.errors, (unsigned)s.retries);
  if (bad || (got < target && !cut)) fsFail("seq", err);
  else LOGI(TAG, "seq: потік 40 КБ/с %s", (maxLag <= (int32_t)kCh376SeqLagOkUs && !cut) ? "ТРИМАЄТЬСЯ" : "НЕ ТРИМАЄТЬСЯ");
}

// Сирий FILE_OPEN з імʼям «як є» (діагностика припущень про чіп): статус і, якщо 0x1D, запис каталогу.
void usbFopen(const char* name) {
  if (!needDisk()) return;
  Ch376Fs& fs = ch376_uart::fs();
  uint8_t st = 0, ent[kCh376DirInfoLen];
  bool has = false;
  const int i0 = ch376_uart::intLevel();
  const uint32_t t0 = micros();
  const FsErr e = fs.rawOpen(name, st, ent, has);
  LOGI(TAG, "fopen: рівень INT до=%d після=%d, %u мкс", i0, ch376_uart::intLevel(), (unsigned)(micros() - t0));
  if (e != FsErr::Ok) {
    fsFail("fopen", e);
    return;
  }
  LOGI(TAG, "fopen \"%s\": статус 0x%02X, пауза імʼя→open %u мкс", name, (unsigned)st, (unsigned)fs.gapUs());
  if (has) {
    char hb[16 * 3 + 1];
    hexText(ent, 16, hb, sizeof hb);
    LOGI(TAG, "fopen: запис[0..15] %s", hb);
    hexText(ent + 16, 16, hb, sizeof hb);
    LOGI(TAG, "fopen: запис[16..31] %s", hb);
  }
  fs.close();
}

// Довільна команда чіпа: DBG,usb,cmd,<hex код>[,<hex арг>…≤4]. Друкує статус, рівень INT і дані (напр. 3F DISK_QUERY, 3E DISK_CAPACITY).
void usbCmd(const proto::Msg& m) {
  if (!needDisk()) return;
  long v = 0;
  uint8_t b[5];
  size_t n = 0;
  for (int i = 2; i < m.argc && n < 5; i++) {
    if (!toInt(m.args[i], v, 16) || v < 0 || v > 255) {
      LOGW(TAG, "DBG,usb,cmd,<hex код>[,<hex арг>…≤4]");
      return;
    }
    b[n++] = (uint8_t)v;
  }
  uint8_t st = 0, data[kCh376StageMax];
  size_t dl = 0;
  const int i0 = ch376_uart::intLevel();
  const uint32_t t0 = micros();
  const FsErr e = ch376_uart::fs().rawCmd(b[0], b + 1, n - 1, st, data, dl);
  const uint32_t dt = (uint32_t)(micros() - t0);
  if (e != FsErr::Ok) {
    fsFail("cmd", e);
    return;
  }
  LOGI(TAG, "cmd %02X: статус 0x%02X, INT до=%d після=%d, довжина=0x%02X, даних %u Б, %u мкс", (unsigned)b[0],
       (unsigned)st, i0, ch376_uart::intLevel(), (unsigned)ch376_uart::fs().lastLenByte(), (unsigned)dl, (unsigned)dt);
  for (size_t o = 0; o < dl; o += 16) {
    char hb[16 * 3 + 1];
    hexText(data + o, dl - o < 16 ? dl - o : 16, hb, sizeof hb);
    LOGI(TAG, "cmd: [%u] %s", (unsigned)o, hb);
  }
}

// Сирий сектор через DISK_READ (діагностика розкладки тому): DBG,usb,sec,<LBA>[,<блок 0..7>]; без блоку — усі 8.
void usbSec(const proto::Msg& m) {
  if (!needDisk()) return;
  long lba = 0, blk = -1;
  if (!toInt(m.args[2], lba, 10) || lba < 0 || (m.argc == 4 && (!toInt(m.args[3], blk, 10) || blk < 0 || blk > 7))) {
    LOGW(TAG, "DBG,usb,sec,<LBA десятковий>[,<блок 0..7>]");
    return;
  }
  Ch376Fs& fs = ch376_uart::fs();
  for (long b = (blk < 0 ? 0 : blk); b <= (blk < 0 ? 7 : blk); b++) {
    uint8_t d[kCh376StageMax];
    size_t n = 0;
    const FsErr e = fs.rawSector((uint32_t)lba, (uint8_t)b, d, n);
    if (e != FsErr::Ok) {
      fsFail("sec", e);
      return;
    }
    for (size_t o = 0; o < n; o += 16) {
      char hb[16 * 3 + 1];
      hexText(d + o, n - o < 16 ? n - o : 16, hb, sizeof hb);
      LOGI(TAG, "sec %ld+%03u: %s", lba, (unsigned)((size_t)b * kCh376StageMax + o), hb);
    }
  }
}

// SET_USB_MODE з консолі (напр. 7 → пауза → 6: скид шини USB перед повторним mount, як радить даташит 5.9).
void usbMode(long mode) {
  if (!ch376_uart::started()) {
    LOGW(TAG, "драйвер не запущено: DBG,usb,init");
    return;
  }
  Ch376& d = ch376_uart::driver();
  const uint32_t t0 = micros();
  const Ch376Err e = d.setUsbMode((uint8_t)mode);
  s_lastErr = e;
  LOGI(TAG, "mode %ld: %s, %u мкс", mode, ch376ErrName(e), (unsigned)(micros() - t0));
  if (mode != 6) s_present = false;  // після зміни режиму потрібен повторний mount (cmd,31)
  else s_present = true;
}

void usbGap(const char* v) {
  Ch376Fs& fs = ch376_uart::fs();
  if (v) {
    long us = 0;
    if (!toInt(v, us, 10) || us < 0 || us > 100000) {
      LOGW(TAG, "DBG,usb,gap[,<мкс 0..100000>]");
      return;
    }
    fs.setGapUs((uint32_t)us);
  }
  LOGI(TAG, "gap: пауза між SET_FILE_NAME і FILE_OPEN = %u мкс", (unsigned)fs.gapUs());
}

// Режим очікування статусу файлових операцій: INT# чи опитування GET_STATUS (для порівняння КБ/с).
void usbWait(const char* mode) {
  Ch376Fs& fs = ch376_uart::fs();
  if (mode) {
    if (!strcasecmp(mode, "int")) {
      if (!fs.intUsable()) {
        LOGW(TAG, "INT: пін не підключено (CH_INT_PIN < 0)");
        return;
      }
      fs.setUseInt(true);
    } else if (!strcasecmp(mode, "poll")) {
      fs.setUseInt(false);
    } else {
      LOGW(TAG, "DBG,usb,wait[,int|poll]");
      return;
    }
  }
  LOGI(TAG, "wait: режим %s, INT_PIN=%d рівень=%d", waitMode(), (int)CH_INT_PIN, ch376_uart::intLevel());
}

// Зупинка драйвера: закрити Serial2 (звільнити RX-буфер) і показати heap.
void usbStop() {
  // Чіп лишається на узгодженій швидкості: скидаємо його на ній (порт → 9600), інакше наступний init його не знайде.
  if (ch376_uart::started() && ch376_uart::driver().baud() != kCh376DefaultBaud) {
    Ch376& d = ch376_uart::driver();
    if (d.reset() != Ch376Err::Ok) d.rollback(d.baud());
  }
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
  if (!strcasecmp(s, "init") && (m.argc == 2 || m.argc == 3)) {
    long b = 0;
    if (m.argc == 3) {
      if (!strcasecmp(m.args[2], "auto")) b = -1;
      else if (!toInt(m.args[2], b, 10) || !negotiable(b)) {
        LOGW(TAG, "DBG,usb,init[,1000000|460800|115200|auto]");
        return;
      }
    }
    usbInit(b);
  } else if (!strcasecmp(s, "ls") && (m.argc == 2 || m.argc == 3)) {
    usbLs(m.argc == 3 ? m.args[2] : "/");
  } else if (!strcasecmp(s, "read") && (m.argc == 3 || m.argc == 4)) {
    long kb = (long)kCh376DiagReadKbDef;
    if (m.argc == 4 && (!toInt(m.args[3], kb, 10) || kb < 1 || kb > (long)kCh376DiagKbMax)) {
      LOGW(TAG, "DBG,usb,read,<шлях>[,<КБ 1..%u>]", (unsigned)kCh376DiagKbMax);
      return;
    }
    usbRead(m.args[2], (uint32_t)kb);
  } else if (!strcasecmp(s, "seek") && (m.argc == 4 || m.argc == 5)) {
    long off = 0, n = (long)kCh376DiagSeekN;
    if (!toInt(m.args[3], off, 10) || off < 0 || (m.argc == 5 && (!toInt(m.args[4], n, 10) || n < 1 || n > 1048576))) {
      LOGW(TAG, "DBG,usb,seek,<шлях>,<зсув ≥ 0>[,<N 1..1048576>]");
      return;
    }
    usbSeek(m.args[2], (uint32_t)off, (uint32_t)n);
  } else if (!strcasecmp(s, "seq") && (m.argc == 3 || m.argc == 4)) {
    long kb = (long)kCh376SeqKbDef;
    if (m.argc == 4 && (!toInt(m.args[3], kb, 10) || kb < 1 || kb > 400)) {
      LOGW(TAG, "DBG,usb,seq,<шлях>[,<КБ 1..400>] (400 КБ ≈ 10 с)");
      return;
    }
    usbSeq(m.args[2], (uint32_t)kb);
  } else if (!strcasecmp(s, "trace") && m.argc == 3) {
    if (!strcasecmp(m.args[2], "on")) {
      ch376_uart::begin();
      LOGI(TAG, "trace: %s", ch376_uart::trace(true) ? "ввімкнено" : "немає памʼяті");
    } else if (!strcasecmp(m.args[2], "off")) {
      ch376_uart::trace(false);
      LOGI(TAG, "trace: вимкнено");
    } else if (!strcasecmp(m.args[2], "dump")) {
      ch376_uart::traceDump();
    } else {
      LOGW(TAG, "DBG,usb,trace,on|off|dump");
    }
  } else if (!strcasecmp(s, "cmd") && m.argc >= 3 && m.argc <= 7) {
    usbCmd(m);
  } else if (!strcasecmp(s, "sec") && (m.argc == 3 || m.argc == 4)) {
    usbSec(m);
  } else if (!strcasecmp(s, "mode") && m.argc == 3) {
    long md = 0;
    if (!toInt(m.args[2], md, 10) || md < 4 || md > 7) {
      LOGW(TAG, "DBG,usb,mode,<4..7> (6 — host з SOF, 7 — скид шини)");
      return;
    }
    usbMode(md);
  } else if (!strcasecmp(s, "gap") && (m.argc == 2 || m.argc == 3)) {
    usbGap(m.argc == 3 ? m.args[2] : nullptr);
  } else if (!strcasecmp(s, "fopen") && m.argc == 3) {
    usbFopen(m.args[2]);
  } else if (!strcasecmp(s, "wait") && (m.argc == 2 || m.argc == 3)) {
    usbWait(m.argc == 3 ? m.args[2] : nullptr);
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


// Тести M4c2: неблокуючий bring-up, відкриття по кроках (диференціально проти usbOpenCore), автомат присутності флешки.
// Запуск: pio test -e native -f test_usb_m4c2
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unity.h>

#include <new>
#include <vector>

#include "config.h"
#include "usb/usb_bringup.h"
#include "usb/usb_presence_core.h"
#include "usb/usb_volume_core.h"

static long g_live = 0;
void* operator new(size_t n) {
  void* p = malloc(n ? n : 1);
  if (!p) abort();
  g_live++;
  return p;
}
void* operator new[](size_t n) { return operator new(n); }
void* operator new(size_t n, const std::nothrow_t&) noexcept {
  void* p = malloc(n ? n : 1);
  if (p) g_live++;
  return p;
}
void* operator new[](size_t n, const std::nothrow_t&) noexcept { return operator new(n, std::nothrow); }
void operator delete(void* p) noexcept { if (p) { g_live--; free(p); } }
void operator delete[](void* p) noexcept { operator delete(p); }
void operator delete(void* p, size_t) noexcept { operator delete(p); }
void operator delete[](void* p, size_t) noexcept { operator delete(p); }
void operator delete(void* p, const std::nothrow_t&) noexcept { operator delete(p); }
void operator delete[](void* p, const std::nothrow_t&) noexcept { operator delete(p); }

void setUp() {}
void tearDown() {}

namespace {

// ======================================================== FakeChip (IUsbChip)
struct FakeChip : IUsbChip {
  // «фізика»
  bool flash = false, host = false, open = false;
  uint32_t baud = 9600;
  uint8_t pend = 0;        // статус, що чекає в чіпі (INT# LOW, поки не прочитано)
  int evDelay = 0;         // скільки опитувань status() повертають 0 перед подією
  int delayLeft = 0;
  uint32_t edges = 0;
  UsbOpenFail probeRes = UsbOpenFail::None;
  bool pingOk = true, wakeOk = true, statusOk = true, connectSendOk = true;
  uint32_t uartBaudAtWake = 0;
  // лічильники
  int wakes = 0, parks = 0, pings = 0, statuses = 0, probes = 0, resets = 0, recovers = 0, connects = 0, mounts = 0, touches = 0;
  bool mountEvent = true;

  void raise(uint8_t st) { if (host) { if (pend == 0) edges++; pend = st; } }
  void insert() { flash = true; raise(kCh376IntConnect); }
  void remove() { flash = false; raise(kCh376IntDisconnect); }

  bool hasInt() const override { return true; }
  bool intLow() const override { return host && pend != 0; }
  uint32_t intEdges() const override { return edges; }
  bool portWake() override { if (!wakeOk) return false; wakes++; open = true; uartBaudAtWake = baud; return true; }
  void portPark() override { parks++; open = false; }
  bool portOpen() const override { return open; }
  uint32_t chipBaud() const override { return baud; }
  bool chipHost() const override { return host; }
  Ch376Err ping() override { touches++; pings++; return (open && pingOk) ? Ch376Err::Ok : Ch376Err::NoReply; }
  Ch376Err recover() override { touches++; recovers++; baud = 9600; return Ch376Err::Ok; }
  Ch376Err resetSend() override { touches++; resets++; host = false; pend = 0; return Ch376Err::Ok; }
  Ch376Err resetFinish() override { baud = 9600; return Ch376Err::Ok; }
  Ch376Err setBaud(uint32_t b) override { touches++; baud = b ? b : 460800; return Ch376Err::Ok; }
  Ch376Err setHost() override { touches++; host = true; if (flash) raise(kCh376IntConnect); return Ch376Err::Ok; }
  Ch376Err status(uint8_t& st) override {
    touches++;
    statuses++;
    if (!open || !statusOk) return Ch376Err::NoReply;
    if (delayLeft > 0) { delayLeft--; st = 0; return Ch376Err::Ok; }
    st = pend;
    pend = 0;
    return Ch376Err::Ok;
  }
  Ch376Err diskConnectSend() override {
    touches++;
    connects++;
    if (!connectSendOk) return Ch376Err::NoReply;
    pend = flash ? kCh376IntSuccess : kCh376IntDisconnect;
    edges++;  // власний обмін теж смикає INT#
    delayLeft = evDelay;
    return Ch376Err::Ok;
  }
  Ch376Err diskMountSend() override {
    touches++;
    mounts++;
    pend = flash ? (mountEvent ? kCh376IntSuccess : kCh376ErrDiskDisc) : kCh376IntDisconnect;
    edges++;
    delayLeft = evDelay;
    return Ch376Err::Ok;
  }
  UsbOpenFail probe() override { touches++; probes++; return flash ? probeRes : UsbOpenFail::NoMedia; }
};

struct Clock { uint32_t t = 0; };

// ======================================================== bring-up по кроках
void runBring(UsbBringUp& b, uint32_t& t, int maxMs, int& steps) {
  steps = 0;
  while (t < (uint32_t)maxMs) {
    steps++;
    if (b.step(t)) return;
    t++;
  }
}

void test_bring_ok_with_flash() {
  FakeChip c;
  c.open = true;
  c.flash = true;
  UsbBringUp b(c);
  b.begin(kUsbBaudFast);
  uint32_t t = 0;
  int steps = 0;
  // чіп після скиду переходить у host і сам повідомляє CONNECT (flash уже стоїть)
  runBring(b, t, 10000, steps);
  TEST_ASSERT_EQUAL((int)Ch376Err::Ok, (int)b.err());
  TEST_ASSERT_EQUAL_UINT32(kUsbBaudFast, c.baud);
  TEST_ASSERT_TRUE(c.host);
  TEST_ASSERT_TRUE(t >= kCh376ResetWaitMs);  // пауза скиду витримана таймером
  TEST_ASSERT_EQUAL_INT(1, c.connects);
  TEST_ASSERT_EQUAL_INT(1, c.mounts);
}

void test_bring_no_flash_times_out_after_3s_without_blocking() {
  FakeChip c;
  c.open = true;
  UsbBringUp b(c);
  b.begin(kUsbBaudRef);
  uint32_t t = 0;
  int steps = 0;
  runBring(b, t, 10000, steps);
  TEST_ASSERT_EQUAL((int)Ch376Err::Timeout, (int)b.err());
  TEST_ASSERT_TRUE(t >= kCh376ConnectWaitMs && t < kCh376ConnectWaitMs + kCh376ResetWaitMs + 100);
  TEST_ASSERT_EQUAL_INT(0, c.connects);
}

void test_bring_disconnect_and_nodisk() {
  FakeChip c;
  c.open = true;
  c.flash = true;
  c.mountEvent = false;  // DISK_MOUNT → «диска немає»
  UsbBringUp b(c);
  b.begin(kUsbBaudRef);
  uint32_t t = 0;
  int steps;
  runBring(b, t, 10000, steps);
  TEST_ASSERT_EQUAL((int)Ch376Err::NoDisk, (int)b.err());
}

void test_bring_check_fail_recovers_once() {
  FakeChip c;
  c.open = true;
  c.flash = true;
  c.pingOk = false;
  UsbBringUp b(c);
  b.begin(kUsbBaudRef);
  uint32_t t = 0;
  int steps;
  runBring(b, t, 10000, steps);
  TEST_ASSERT_EQUAL((int)Ch376Err::NoReply, (int)b.err());
  TEST_ASSERT_EQUAL_INT(1, c.recovers);  // один відкат, друга невдача — помилка
  TEST_ASSERT_EQUAL_INT(2, c.resets);
}

void test_bring_abort_stops() {
  FakeChip c;
  c.open = true;
  UsbBringUp b(c);
  b.begin(kUsbBaudRef);
  for (uint32_t t = 0; t < 200; t++) b.step(t);
  const int st = c.statuses;
  b.abort();
  TEST_ASSERT_FALSE(b.active());
  for (uint32_t t = 200; t < 400; t++) b.step(t);
  TEST_ASSERT_EQUAL_INT(st, c.statuses);
}

// ======================================================== відкриття по кроках: диференціально
void put16(uint8_t* p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
void put32(uint8_t* p, uint32_t v) { for (int i = 0; i < 4; i++) p[i] = (uint8_t)(v >> (8 * i)); }

struct FakeHw : IUsbOpenHw {
  std::vector<uint8_t> img;
  uint32_t cap;
  bool portOk = true;
  Ch376Err r1M = Ch376Err::Ok, r2M = Ch376Err::Ok, rAuto = Ch376Err::BaudFail, back1M = Ch376Err::Ok;
  bool flip2M = false;
  DiskErr rd2M = DiskErr::Ok, rd1M = DiskErr::Ok, capErr = DiskErr::Ok;
  uint32_t cur = 0;
  int cnt1M = 0, portEnds = 0, aborts = 0;
  int slow = 0;  // скільки bringStep повертають «ще триває» перед результатом (0 — типове блокуюче)
  int slowLeft = 0;
  uint32_t pendB = 0;
  bool async = false;

  explicit FakeHw(uint32_t sectors) : img((size_t)64 * 512u, 0), cap(sectors) {
    uint8_t* s = &img[0];
    s[0] = 0xEB; s[1] = 0x58; s[2] = 0x90;
    put16(s + 11, 512); s[13] = 1; put16(s + 14, 32); s[16] = 2;
    put32(s + 32, sectors); put32(s + 36, 500); put32(s + 44, 2);
    memcpy(s + 0x47, "MEDIA      ", 11);
    s[510] = 0x55; s[511] = 0xAA;
  }
  bool portBegin() override { return portOk; }
  Ch376Err bringUp(uint32_t b) override {
    Ch376Err r;
    if (b == kUsbBaudRef) { cnt1M++; r = cnt1M == 1 ? r1M : back1M; if (r == Ch376Err::Ok) cur = b; }
    else if (b == kUsbBaudFast) { r = r2M; if (r == Ch376Err::Ok) cur = b; }
    else { r = rAuto; if (r == Ch376Err::Ok) cur = 460800; }
    return r;
  }
  void bringBegin(uint32_t b) override { pendB = b; slowLeft = async ? slow : 0; }
  bool bringStep(uint32_t, Ch376Err& res) override {
    if (slowLeft > 0) { slowLeft--; return false; }
    res = bringUp(pendB);
    return true;
  }
  void bringAbort() override { aborts++; }
  uint32_t baud() const override { return cur; }
  DiskErr capacity(uint32_t& s) override { s = cap; return capErr; }
  DiskErr readSector(uint32_t lba, uint8_t* dst) override {
    if (cur >= kUsbBaudFast && rd2M != DiskErr::Ok) return rd2M;
    if (cur == kUsbBaudRef && rd1M != DiskErr::Ok) return rd1M;
    if (lba >= cap) return DiskErr::ReadError;
    if (lba < 64) memcpy(dst, &img[(size_t)lba * 512u], 512);
    else memset(dst, 0, 512);
    if (cur >= kUsbBaudFast && flip2M && lba == 0) dst[100] ^= 0x40;
    return DiskErr::Ok;
  }
  void portEnd() override { portEnds++; }
};

struct DummyLink : Ch376Link {
  void write(const uint8_t*, size_t) override {}
  bool readByte(uint8_t&, uint32_t) override { return false; }
  void flushRx() override {}
  bool setBaud(uint32_t) override { return true; }
  bool hasIntPin() const override { return false; }
  bool intLow() const override { return false; }
  bool hwReset() override { return false; }
  bool abortRequested() const override { return false; }
  void delayMs(uint32_t) override {}
  uint32_t nowUs() const override { return 0; }
};

typedef void (*Cfg)(FakeHw&);
void cOk(FakeHw&) {}
void cNoMedia1M(FakeHw& h) { h.r1M = Ch376Err::Timeout; }
void cNoMediaDisc(FakeHw& h) { h.r1M = Ch376Err::Disconnected; }
void cFlip(FakeHw& h) { h.flip2M = true; }
void cFastFail(FakeHw& h) { h.r2M = Ch376Err::BaudFail; }
void cFastRead(FakeHw& h) { h.rd2M = DiskErr::Timeout; }
void cBackFail(FakeHw& h) { h.flip2M = true; h.back1M = Ch376Err::BadReply; }
void cRefFailFastOk(FakeHw& h) { h.r1M = Ch376Err::BaudFail; }
void cRefFailFastFail(FakeHw& h) { h.r1M = Ch376Err::BaudFail; h.r2M = Ch376Err::BaudFail; }
void cRefFailAll(FakeHw& h) { h.r1M = Ch376Err::BaudFail; h.r2M = Ch376Err::BaudFail; h.rAuto = Ch376Err::BaudFail; }
void cRefFailNoMedia2(FakeHw& h) { h.r1M = Ch376Err::BaudFail; h.r2M = Ch376Err::NoDisk; }
void cRefRead(FakeHw& h) { h.rd1M = DiskErr::Disconnected; }
void cPort(FakeHw& h) { h.portOk = false; }
void cCap(FakeHw& h) { h.capErr = DiskErr::ReadError; }
void cNoPart(FakeHw& h) { memset(&h.img[0], 0, 512); }
const Cfg kCfgs[] = {cOk, cNoMedia1M, cNoMediaDisc, cFlip, cFastFail, cFastRead, cBackFail, cRefFailFastOk, cRefFailFastFail, cRefFailAll,
                     cRefFailNoMedia2, cRefRead, cPort, cCap, cNoPart};
const int kNCfg = sizeof(kCfgs) / sizeof(kCfgs[0]);

void test_open_steps_equal_blocking_core() {
  for (int i = 0; i < kNCfg; i++) {
    for (int slow = 0; slow <= 3; slow += 3) {
      FakeHw a(70000), b(70000);
      kCfgs[i](a);
      kCfgs[i](b);
      b.async = true;
      b.slow = slow;
      uint8_t sa[512], sb[512];
      ProbeReport pa, pb;
      UsbOpenInfo ia, ib;
      const UsbOpenFail fa = usbOpenCore(a, sa, pa, ia);
      UsbOpenSteps st;
      st.begin(b, sb, &pb, ib);
      UsbOpenFail fb = UsbOpenFail::AlreadyOpen;
      OpenPoll r = OpenPoll::Pending;
      int n = 0;
      while (r == OpenPoll::Pending && n++ < 1000) r = st.step((uint32_t)n, fb);
      TEST_ASSERT_TRUE_MESSAGE(r != OpenPoll::Pending, "автомат завис");
      TEST_ASSERT_EQUAL_MESSAGE((int)fa, (int)fb, "код відмови");
      TEST_ASSERT_EQUAL((int)(fa == UsbOpenFail::None), (int)(r == OpenPoll::Done));
      TEST_ASSERT_EQUAL_UINT32(ia.baud, ib.baud);
      TEST_ASSERT_EQUAL((int)ia.selfTest, (int)ib.selfTest);
      TEST_ASSERT_EQUAL_UINT32(ia.capacity, ib.capacity);
      TEST_ASSERT_EQUAL_UINT32(ia.partStart, ib.partStart);
      TEST_ASSERT_EQUAL_UINT32(ia.partSectors, ib.partSectors);
      TEST_ASSERT_EQUAL_UINT8(ia.nSteps, ib.nSteps);
      for (int k = 0; k < ia.nSteps; k++) {
        TEST_ASSERT_EQUAL_UINT32(ia.steps[k].baud, ib.steps[k].baud);
        TEST_ASSERT_EQUAL((int)ia.steps[k].err, (int)ib.steps[k].err);
      }
    }
  }
}

void test_session_steps_pending_then_done_and_no_leak() {
  DummyLink L;
  FakeHw h(70000);
  const long live0 = g_live;
  h.async = true;
  h.slow = 4;
  UsbSession s;
  TEST_ASSERT_EQUAL((int)UsbOpenFail::None, (int)s.openBegin(L, h));
  TEST_ASSERT_TRUE(s.opening());
  UsbOpenFail f = UsbOpenFail::None;
  OpenPoll r = OpenPoll::Pending;
  int pend = 0;
  for (uint32_t t = 0; t < 1000 && r == OpenPoll::Pending; t++) {
    r = s.openStep(t, f);
    if (r == OpenPoll::Pending) pend++;
  }
  TEST_ASSERT_EQUAL((int)OpenPoll::Done, (int)r);
  TEST_ASSERT_TRUE(pend >= 8);  // відкриття розбито на багато коротких кроків
  TEST_ASSERT_FALSE(s.opening());
  TEST_ASSERT_TRUE(s.isOpen());
  s.close();
  TEST_ASSERT_EQUAL_INT(live0, g_live);
}

void test_session_abort_at_every_step_frees_everything() {
  for (int k = 0; k < 40; k++) {
    DummyLink L;
    FakeHw h(70000);
    const long live0 = g_live;
    h.async = true;
    h.slow = 2;
    UsbSession s;
    TEST_ASSERT_EQUAL((int)UsbOpenFail::None, (int)s.openBegin(L, h));
    UsbOpenFail f;
    OpenPoll r = OpenPoll::Pending;
    for (int i = 0; i < k && r == OpenPoll::Pending; i++) r = s.openStep((uint32_t)i, f);
    if (r == OpenPoll::Pending) {
      s.openAbort();
      TEST_ASSERT_EQUAL_INT(1, h.portEnds);
      TEST_ASSERT_FALSE(s.opening());
      TEST_ASSERT_FALSE(s.isOpen());
    } else {
      s.close();
    }
    TEST_ASSERT_EQUAL_INT(live0, g_live);
  }
}

void test_session_failure_closes_port() {
  DummyLink L;
  FakeHw h(70000);
  const long live0 = g_live;
  cNoMedia1M(h);
  UsbSession s;
  s.openBegin(L, h);
  UsbOpenFail f = UsbOpenFail::None;
  OpenPoll r = OpenPoll::Pending;
  for (uint32_t t = 0; t < 100 && r == OpenPoll::Pending; t++) r = s.openStep(t, f);
  TEST_ASSERT_EQUAL((int)OpenPoll::Failed, (int)r);
  TEST_ASSERT_EQUAL((int)UsbOpenFail::NoMedia, (int)f);
  TEST_ASSERT_EQUAL_INT(1, h.portEnds);
  TEST_ASSERT_EQUAL_INT(live0, g_live);
}

void test_static_footprint() {
  TEST_ASSERT_TRUE(sizeof(UsbOpenSteps) <= 48);
  TEST_ASSERT_TRUE(sizeof(UsbBringUp) <= 40);
  TEST_ASSERT_TRUE(sizeof(UsbPresenceCore) <= 96);
}

// ======================================================== присутність
struct Rig {
  FakeChip c;
  UsbPresenceCore p;
  uint32_t t = 0;
  UsbPresEnv env;
  int changes = 0;
  bool lastFlag = false;
  explicit Rig(uint8_t mode = 1) : p(c, mode) {}
  void run(uint32_t ms) {
    for (uint32_t i = 0; i < ms; i++) {
      t++;
      env.now = t;
      p.tick(env);
      bool f;
      if (p.takeChange(f)) { changes++; lastFlag = f; }
    }
  }
  // до першого моменту, коли автомат стоїть і порт закрито
  void settleAll(uint32_t ms = 6000) { run(ms); }
  void boot() { p.begin(t); run(kUsbPresInitDelayMs + kUsbPresInitSettleMs + 600); }
};

void test_pres_init_no_flash_absent_parked() {
  Rig r;
  r.boot();
  TEST_ASSERT_EQUAL((int)UsbPres::Absent, (int)r.p.state());
  TEST_ASSERT_FALSE(r.c.open);          // запарковано: UART2 вимкнено
  TEST_ASSERT_TRUE(r.c.host);           // чіп лишився в host
  TEST_ASSERT_EQUAL_UINT32(kUsbBaudFast, r.c.baud);  // 2 Мбод збережено в чіпі
  TEST_ASSERT_EQUAL_INT(0, r.changes);  // прапорець 0 → 0: MEDIA не шлеться
  TEST_ASSERT_EQUAL_INT(0, r.c.probes);
}

void test_pres_init_not_before_delay() {
  Rig r;
  r.p.begin(0);
  r.run(kUsbPresInitDelayMs - 50);
  TEST_ASSERT_EQUAL_INT(0, r.c.touches);
}

void test_pres_init_with_flash_present() {
  Rig r;
  r.c.flash = true;
  r.boot();
  TEST_ASSERT_EQUAL((int)UsbPres::Present, (int)r.p.state());
  TEST_ASSERT_EQUAL_INT(1, r.changes);
  TEST_ASSERT_TRUE(r.lastFlag);
  TEST_ASSERT_FALSE(r.c.open);
}

void test_pres_insert_and_remove_reaction_time() {
  Rig r;
  r.boot();
  const uint32_t t0 = r.t;
  r.c.insert();
  while (r.changes == 0 && r.t - t0 < 5000) r.run(1);
  TEST_ASSERT_EQUAL((int)UsbPres::Present, (int)r.p.state());
  TEST_ASSERT_TRUE(r.t - t0 <= 400);  // на моделі без латентності чіпа: пауза дребезгу + кроки
  TEST_ASSERT_TRUE(r.lastFlag);
  r.run(100);
  TEST_ASSERT_FALSE(r.c.open);
  const uint32_t t1 = r.t;
  r.c.remove();
  while (r.changes == 1 && r.t - t1 < 5000) r.run(1);
  TEST_ASSERT_EQUAL((int)UsbPres::Absent, (int)r.p.state());
  TEST_ASSERT_FALSE(r.lastFlag);
  TEST_ASSERT_TRUE(r.t - t1 <= 300);
  TEST_ASSERT_EQUAL_INT(1, r.c.probes);  // виймання — без probe
  r.run(100);
  TEST_ASSERT_FALSE(r.c.open);
}

void test_pres_unsupported_fs_flag_zero() {
  Rig r;
  r.boot();
  r.c.probeRes = UsbOpenFail::UnsupportedFs;
  r.c.insert();
  r.run(1000);
  TEST_ASSERT_EQUAL((int)UsbPres::Unsupported, (int)r.p.state());
  TEST_ASSERT_EQUAL_INT(0, r.changes);  // 0 → 0
  r.c.probeRes = UsbOpenFail::None;
  r.c.remove();
  r.run(500);
  r.c.insert();  // знову, вже з придатною ФС
  r.run(1000);
  TEST_ASSERT_EQUAL((int)UsbPres::Present, (int)r.p.state());
  TEST_ASSERT_EQUAL_INT(1, r.changes);
}

void test_pres_own_exchange_edges_do_not_retrigger() {  // регресія: фронти INT# від власного обміну не є новою подією
  Rig r;
  r.boot();
  r.c.insert();
  r.run(8000);
  TEST_ASSERT_EQUAL((int)UsbPres::Present, (int)r.p.state());
  TEST_ASSERT_EQUAL_INT(1, r.c.probes);
  TEST_ASSERT_EQUAL_INT(1, r.c.connects);
  TEST_ASSERT_TRUE(r.c.edges > 2);
}

void test_pres_status_polled_only_when_int_low() {  // GET_STATUS без події коштує тайм-аут: опитуємо лише при INT# LOW
  Rig r;
  r.boot();
  r.c.evDelay = 0;
  const int st0 = r.c.statuses;
  r.run(3000);
  TEST_ASSERT_EQUAL_INT(st0, r.c.statuses);  // у спокої жодного GET_STATUS
}

void test_pres_empty_flash_without_mp3_is_present() {
  Rig r;  // probe не дивиться на mp3: ФС придатна → Present
  r.boot();
  r.c.insert();
  r.run(1000);
  TEST_ASSERT_TRUE(r.lastFlag);
}

void test_pres_defer_in_bt_does_not_touch_chip_and_keeps_event() {
  Rig r;
  r.boot();
  const int touches = r.c.touches;
  r.env.defer = true;
  r.c.insert();
  r.run(3000);
  TEST_ASSERT_EQUAL_INT(touches, r.c.touches);  // жодного звернення до чіпа
  TEST_ASSERT_FALSE(r.c.open);
  TEST_ASSERT_EQUAL((int)UsbPres::Absent, (int)r.p.state());
  TEST_ASSERT_TRUE(r.c.intLow());               // подія не загубилась
  r.c.remove();                                  // ще одна подія за час BT: лишається лише остання
  r.c.insert();
  r.env.defer = false;                           // вихід з BT
  r.run(1000);
  TEST_ASSERT_EQUAL((int)UsbPres::Present, (int)r.p.state());
  TEST_ASSERT_TRUE(r.lastFlag);
}

void test_pres_defer_midway_parks_and_redoes() {
  Rig r;
  r.boot();
  r.c.insert();
  r.run(kUsbPresSettleMs + 5);  // автомат у процесі, порт може бути відкритий
  r.env.defer = true;           // BT став активним
  r.run(50);
  TEST_ASSERT_FALSE(r.c.open);  // памʼять віддано
  r.run(2000);
  TEST_ASSERT_EQUAL((int)UsbPres::Absent, (int)r.p.state());  // поки BT — без змін
  r.env.defer = false;
  r.run(2000);
  TEST_ASSERT_EQUAL((int)UsbPres::Present, (int)r.p.state());  // повна перевірка після BT, подія не втрачена
}

void test_pres_event_during_probe_not_lost() {
  Rig r;
  r.boot();
  r.c.insert();
  r.run(kUsbPresSettleMs + 60);
  r.c.remove();  // виймання, поки автомат ще в процесі
  r.run(3000);
  TEST_ASSERT_EQUAL((int)UsbPres::Absent, (int)r.p.state());
  TEST_ASSERT_FALSE(r.c.intLow());
}

void test_pres_chip_latency_polls() {
  Rig r;
  r.boot();
  r.c.evDelay = 30;  // DISK_CONNECT/MOUNT відповідають не одразу
  r.c.insert();
  r.run(3000);
  TEST_ASSERT_EQUAL((int)UsbPres::Present, (int)r.p.state());
}

void test_pres_poll_mode_finds_event_without_int() {
  Rig r(2);
  r.boot();
  TEST_ASSERT_EQUAL((int)UsbPres::Absent, (int)r.p.state());
  const int w = r.c.wakes;
  r.run(kUsbPresPollMs * 2 + 100);
  TEST_ASSERT_TRUE(r.c.wakes >= w + 1);  // періодичні wake
  TEST_ASSERT_TRUE(r.c.wakes <= w + 3);  // не частіше ніж раз на kUsbPresPollMs
  TEST_ASSERT_FALSE(r.c.open);
  r.c.insert();
  r.run(kUsbPresPollMs + 1500);
  TEST_ASSERT_EQUAL((int)UsbPres::Present, (int)r.p.state());
  TEST_ASSERT_TRUE(r.lastFlag);
}

void test_pres_mode0_does_nothing() {
  Rig r(0);
  r.boot();
  r.c.insert();
  r.run(3000);
  TEST_ASSERT_EQUAL_INT(0, r.c.touches);
  TEST_ASSERT_EQUAL((int)UsbPres::Unknown, (int)r.p.state());
}

void test_pres_port_busy_is_respected() {
  Rig r;
  r.boot();
  r.env.portBusy = true;
  const int t = r.c.touches;
  r.c.insert();
  r.run(2000);
  TEST_ASSERT_EQUAL_INT(t, r.c.touches);
  r.env.portBusy = false;
  r.run(1500);
  TEST_ASSERT_EQUAL((int)UsbPres::Present, (int)r.p.state());
}

void test_pres_session_notifications() {
  Rig r;
  r.boot();
  r.p.sessionOpened();
  bool f = false;
  TEST_ASSERT_TRUE(r.p.takeChange(f));
  TEST_ASSERT_TRUE(f);
  TEST_ASSERT_FALSE(r.p.takeChange(f));  // повторно — без події
  r.p.volumeLost();
  TEST_ASSERT_TRUE(r.p.takeChange(f));
  TEST_ASSERT_FALSE(f);
  r.p.openFailed(UsbOpenFail::UnsupportedFs);
  TEST_ASSERT_EQUAL((int)UsbPres::Unsupported, (int)r.p.state());
  r.p.openFailed(UsbOpenFail::NoChip);  // збій чіпа стан не змінює
  TEST_ASSERT_EQUAL((int)UsbPres::Unsupported, (int)r.p.state());
  r.p.openFailed(UsbOpenFail::NoMedia);
  TEST_ASSERT_EQUAL((int)UsbPres::Absent, (int)r.p.state());
}

void test_pres_recheck_after_session_close() {
  Rig r;
  r.boot();
  r.c.insert();
  r.run(1000);
  TEST_ASSERT_TRUE(r.lastFlag);
  r.c.flash = false;  // вийняли непомітно (подію зʼїла сесія)
  r.p.sessionClosed(true);
  TEST_ASSERT_EQUAL((int)UsbPres::Unknown, (int)r.p.stateForOpen());
  r.run(4500);
  TEST_ASSERT_EQUAL((int)UsbPres::Absent, (int)r.p.state());
  TEST_ASSERT_FALSE(r.lastFlag);
}

void test_pres_state_for_open_unknown_while_pending() {
  Rig r;
  r.boot();
  TEST_ASSERT_EQUAL((int)UsbPres::Absent, (int)r.p.stateForOpen());
  r.c.insert();
  TEST_ASSERT_EQUAL((int)UsbPres::Unknown, (int)r.p.stateForOpen());  // подія ще не оброблена
  r.run(1000);
  TEST_ASSERT_EQUAL((int)UsbPres::Present, (int)r.p.stateForOpen());
}

void test_pres_chip_silent_retries_then_reinit() {
  Rig r;
  r.boot();
  r.c.pingOk = false;
  r.c.insert();
  r.run(6000);
  TEST_ASSERT_TRUE(r.p.needInit() || r.p.stats().retries >= kUsbPresRetryMax);
  TEST_ASSERT_FALSE(r.c.open);  // порт не лишається відкритим
  r.c.pingOk = true;
  r.run(20000);
  TEST_ASSERT_FALSE(r.p.needInit());
  TEST_ASSERT_EQUAL((int)UsbPres::Present, (int)r.p.state());
}

void test_pres_init_gives_up_without_chip() {
  Rig r;
  r.c.wakeOk = false;
  r.p.begin(0);
  r.run(120000);
  TEST_ASSERT_FALSE(r.p.needInit());
  TEST_ASSERT_EQUAL_UINT16(kUsbPresInitTries, r.p.stats().inits);
  r.p.restartInit(r.t);
  r.c.wakeOk = true;
  r.run(2000);
  TEST_ASSERT_EQUAL((int)UsbPres::Absent, (int)r.p.state());
}

void test_pres_external_reset_triggers_reinit() {
  Rig r;
  r.boot();
  r.c.host = false;  // DBG,usb,stop скинув чіп
  r.run(3000);
  TEST_ASSERT_TRUE(r.c.host);
  TEST_ASSERT_EQUAL_UINT32(kUsbBaudFast, r.c.baud);
}

void test_pres_recover_when_chip_left_on_fast_baud() {
  Rig r;
  r.c.pingOk = false;  // перший ping на 9600 мовчить (чіп на 2 Мбод після перезавантаження MCU)
  struct Hook {};
  r.p.begin(0);
  r.run(kUsbPresInitDelayMs + 5);
  r.c.pingOk = true;   // після recover() відповідає
  r.run(9000);
  TEST_ASSERT_TRUE(r.c.recovers >= 1);
  TEST_ASSERT_EQUAL((int)UsbPres::Absent, (int)r.p.state());
}

}  // namespace

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_bring_ok_with_flash);
  RUN_TEST(test_bring_no_flash_times_out_after_3s_without_blocking);
  RUN_TEST(test_bring_disconnect_and_nodisk);
  RUN_TEST(test_bring_check_fail_recovers_once);
  RUN_TEST(test_bring_abort_stops);
  RUN_TEST(test_open_steps_equal_blocking_core);
  RUN_TEST(test_session_steps_pending_then_done_and_no_leak);
  RUN_TEST(test_session_abort_at_every_step_frees_everything);
  RUN_TEST(test_session_failure_closes_port);
  RUN_TEST(test_static_footprint);
  RUN_TEST(test_pres_init_no_flash_absent_parked);
  RUN_TEST(test_pres_init_not_before_delay);
  RUN_TEST(test_pres_init_with_flash_present);
  RUN_TEST(test_pres_insert_and_remove_reaction_time);
  RUN_TEST(test_pres_unsupported_fs_flag_zero);
  RUN_TEST(test_pres_own_exchange_edges_do_not_retrigger);
  RUN_TEST(test_pres_status_polled_only_when_int_low);
  RUN_TEST(test_pres_empty_flash_without_mp3_is_present);
  RUN_TEST(test_pres_defer_in_bt_does_not_touch_chip_and_keeps_event);
  RUN_TEST(test_pres_defer_midway_parks_and_redoes);
  RUN_TEST(test_pres_event_during_probe_not_lost);
  RUN_TEST(test_pres_chip_latency_polls);
  RUN_TEST(test_pres_poll_mode_finds_event_without_int);
  RUN_TEST(test_pres_mode0_does_nothing);
  RUN_TEST(test_pres_port_busy_is_respected);
  RUN_TEST(test_pres_session_notifications);
  RUN_TEST(test_pres_recheck_after_session_close);
  RUN_TEST(test_pres_state_for_open_unknown_while_pending);
  RUN_TEST(test_pres_chip_silent_retries_then_reinit);
  RUN_TEST(test_pres_init_gives_up_without_chip);
  RUN_TEST(test_pres_external_reset_triggers_reinit);
  RUN_TEST(test_pres_recover_when_chip_left_on_fast_baud);
  return UNITY_END();
}

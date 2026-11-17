#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unity.h>

#include <new>
#include <vector>

#include "../test_ch376/mock_disk_link.h"
#include "config.h"
#include "usb/ch376_blockdev.h"
#include "usb/disk_probe.h"
#include "usb/usb_volume_core.h"

// Тести M4b3: ядро блочного пристрою, політика швидкості/самоперевірка, вибір розділу, життєвий цикл сеансу.
// Окремий набір (власний main): наявні тести test_ch376 не змінено. mock-диск — test/test_ch376/mock_disk_link.h.

// ---- лічильник виділень: усі new/delete цього набору (сеанс виділяє лише через new) ----
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
void operator delete(void* p) noexcept {
  if (p) {
    g_live--;
    free(p);
  }
}
void operator delete[](void* p) noexcept { operator delete(p); }
void operator delete(void* p, size_t) noexcept { operator delete(p); }
void operator delete[](void* p, size_t) noexcept { operator delete(p); }
void operator delete(void* p, const std::nothrow_t&) noexcept { operator delete(p); }
void operator delete[](void* p, const std::nothrow_t&) noexcept { operator delete(p); }

void setUp() {}
void tearDown() {}

namespace {

int E(DiskErr e) { return (int)e; }

bool patOk(const uint8_t* b, uint32_t lba, uint32_t n) {
  for (uint32_t k = 0; k < n * 512u; k++)
    if (b[k] != diskPattern(lba * 512u + k)) return false;
  return true;
}

void put16(uint8_t* p, uint32_t v) {
  p[0] = (uint8_t)v;
  p[1] = (uint8_t)(v >> 8);
}
void put32(uint8_t* p, uint32_t v) {
  for (int i = 0; i < 4; i++) p[i] = (uint8_t)(v >> (8 * i));
}
void put64(uint8_t* p, uint64_t v) {
  put32(p, (uint32_t)v);
  put32(p + 4, (uint32_t)(v >> 32));
}

void bootFat(uint8_t* s, uint8_t spc, uint16_t res, uint16_t rootEnt, uint16_t tot16, uint32_t tot32, uint16_t fat16, uint32_t fat32,
             const char* label) {
  memset(s, 0, 512);
  s[0] = 0xEB;
  s[1] = 0x58;
  s[2] = 0x90;
  put16(s + 11, 512);
  s[13] = spc;
  put16(s + 14, res);
  s[16] = 2;
  put16(s + 17, rootEnt);
  put16(s + 19, tot16);
  put16(s + 22, fat16);
  put32(s + 32, tot32);
  put32(s + 36, fat32);
  put32(s + 44, 2);
  if (label) memcpy(s + (fat32 ? 0x47 : 0x2B), label, 11);
  s[510] = 0x55;
  s[511] = 0xAA;
}

void mbrEntry(uint8_t* s, int i, uint8_t type, uint32_t start, uint32_t sectors) {
  uint8_t* e = s + 0x1BE + 16 * i;
  e[4] = type;
  put32(e + 8, start);
  put32(e + 12, sectors);
  s[510] = 0x55;
  s[511] = 0xAA;
}

// ---- образ диска в памʼяті з керованою підміною IUsbOpenHw ----
struct FakeHw : IUsbOpenHw {
  std::vector<uint8_t> img;
  uint32_t cap;
  bool portOk = true;
  Ch376Err r1M = Ch376Err::Ok, r2M = Ch376Err::Ok, rAuto = Ch376Err::BaudFail;
  uint32_t autoBaud = 460800;
  Ch376Err back1M = Ch376Err::Ok;  // результат повторного bringUp(1M) після 2M (для перевірки відкату)
  bool flip2M = false;             // на 2 Мбод сектор 0 спотворюється
  DiskErr rd2M = DiskErr::Ok, rd1M = DiskErr::Ok;
  DiskErr capErr = DiskErr::Ok;
  int bring = 0, portEnds = 0;
  int cnt1M = 0;
  uint32_t cur = 0;

  explicit FakeHw(uint32_t sectors) : img((size_t)sectors * 512u, 0), cap(sectors) {}
  uint8_t* at(uint32_t lba) { return &img[(size_t)lba * 512u]; }

  bool portBegin() override { return portOk; }
  Ch376Err bringUp(uint32_t b) override {
    bring++;
    Ch376Err r;
    if (b == kUsbBaudRef) {
      cnt1M++;
      r = cnt1M == 1 ? r1M : back1M;
      if (r == Ch376Err::Ok) cur = b;
    } else if (b == kUsbBaudFast) {
      r = r2M;
      if (r == Ch376Err::Ok) cur = b;
    } else {
      r = rAuto;
      if (r == Ch376Err::Ok) cur = autoBaud;
    }
    return r;
  }
  uint32_t baud() const override { return cur; }
  DiskErr capacity(uint32_t& s) override {
    s = cap;
    return capErr;
  }
  DiskErr readSector(uint32_t lba, uint8_t* dst) override {
    if (cur >= kUsbBaudFast && rd2M != DiskErr::Ok) return rd2M;
    if (cur == kUsbBaudRef && rd1M != DiskErr::Ok) return rd1M;
    if (lba >= cap) return DiskErr::ReadError;
    memcpy(dst, at(lba), 512);
    if (cur >= kUsbBaudFast && flip2M && lba == 0) dst[100] ^= 0x40;
    return DiskErr::Ok;
  }
  void portEnd() override { portEnds++; }
};

void fat32Boot(FakeHw& h, uint32_t lba, uint32_t tot, const char* label = "MEDIA      ") {
  bootFat(h.at(lba), 1, 32, 0, 0, tot, 0, 500, label);
}

// ======================= блочний пристрій =======================
struct BdFix {
  MockDiskLink L;
  Ch376Disk d;
  Ch376BlockDev bd;
  BdFix() : L(4096), d(L), bd(d) { bd.setPartition(100, 500); }
};

void test_bd_offset() {
  BdFix f;
  uint8_t b[512];
  TEST_ASSERT_TRUE(f.bd.readSector(0, b));
  TEST_ASSERT_TRUE(patOk(b, 100, 1));  // сектор 0 пристрою = LBA 100
  TEST_ASSERT_TRUE(f.bd.readSector(499, b));
  TEST_ASSERT_TRUE(patOk(b, 599, 1));
  TEST_ASSERT_EQUAL_UINT32(500, f.bd.sectorCount());
  TEST_ASSERT_EQUAL_UINT32(599, f.L.lastLba);
  TEST_ASSERT_TRUE(f.bd.present());
}

void test_bd_split_with_tail() {
  BdFix f;
  std::vector<uint8_t> b(20 * 512);
  TEST_ASSERT_TRUE(f.bd.readSectors(3, b.data(), 20));
  TEST_ASSERT_TRUE(patOk(b.data(), 103, 20));
  TEST_ASSERT_EQUAL_INT(3, f.L.cmdCount[0x54]);  // 8 + 8 + 4
  TEST_ASSERT_EQUAL_UINT32(3, f.bd.stats().cmds);
  TEST_ASSERT_EQUAL_UINT32(20, f.bd.stats().sectors);
  TEST_ASSERT_EQUAL_UINT32(1, f.bd.stats().calls);
  TEST_ASSERT_EQUAL_UINT32(4, f.L.lastCount);  // хвіст — окрема команда
  TEST_ASSERT_EQUAL_UINT32(103 + 16, f.L.lastLba);
}

void test_bd_exact_multiple_and_single() {
  BdFix f;
  std::vector<uint8_t> b(16 * 512);
  TEST_ASSERT_TRUE(f.bd.readSectors(0, b.data(), 16));
  TEST_ASSERT_EQUAL_INT(2, f.L.cmdCount[0x54]);
  TEST_ASSERT_TRUE(f.bd.readSector(7, b.data()));
  TEST_ASSERT_EQUAL_INT(3, f.L.cmdCount[0x54]);
  TEST_ASSERT_EQUAL_UINT32(1, f.L.lastCount);
}

void test_bd_edge_and_beyond() {
  BdFix f;
  std::vector<uint8_t> b(16 * 512);
  TEST_ASSERT_TRUE(f.bd.readSectors(490, b.data(), 10));  // рівно до кінця розділу
  TEST_ASSERT_TRUE(patOk(b.data(), 590, 10));
  const int cmds = f.L.cmdCount[0x54];
  TEST_ASSERT_FALSE(f.bd.readSectors(491, b.data(), 10));  // на 1 за межу
  TEST_ASSERT_FALSE(f.bd.readSector(500, b.data()));
  TEST_ASSERT_FALSE(f.bd.readSectors(0xFFFFFFF0u, b.data(), 2));
  TEST_ASSERT_EQUAL_INT(cmds, f.L.cmdCount[0x54]);  // чіпу нічого не надіслано
  TEST_ASSERT_EQUAL_UINT32(3, f.bd.stats().rangeErrors);
  TEST_ASSERT_TRUE(f.bd.present());  // помилка аргументів не робить пристрій «відсутнім»
  TEST_ASSERT_TRUE(f.bd.readSector(0, b.data()));
}

void test_bd_bad_args() {
  BdFix f;
  uint8_t b[512];
  TEST_ASSERT_FALSE(f.bd.readSectors(0, b, 0));
  TEST_ASSERT_FALSE(f.bd.readSectors(0, nullptr, 1));
  TEST_ASSERT_EQUAL_UINT32(2, f.bd.stats().rangeErrors);
  TEST_ASSERT_EQUAL_INT(0, f.L.cmdCount[0x54]);
}

void test_bd_partition_clamp() {
  BdFix f;
  f.bd.setPartition(0xFFFFFFF0u, 100);
  TEST_ASSERT_EQUAL_UINT32(15, f.bd.sectorCount());
}

void test_bd_eject_is_sticky_gone() {
  BdFix f;
  f.L.ejectAtBlock = 3;  // у першій команді
  std::vector<uint8_t> b(8 * 512);
  TEST_ASSERT_FALSE(f.bd.readSectors(0, b.data(), 8));
  TEST_ASSERT_FALSE(f.bd.present());
  TEST_ASSERT_EQUAL_INT(E(DiskErr::Disconnected), E(f.bd.lastError()));
  TEST_ASSERT_EQUAL_UINT32(1, f.bd.stats().errors);
  const int cmds = f.L.cmdCount[0x54];
  const int all = f.L.blockCnt;
  TEST_ASSERT_FALSE(f.bd.readSector(0, b.data()));  // чиста відмова без звернення до чіпа
  TEST_ASSERT_FALSE(f.bd.readSectors(0, b.data(), 4));
  TEST_ASSERT_EQUAL_INT(cmds, f.L.cmdCount[0x54]);
  TEST_ASSERT_EQUAL_INT(all, f.L.blockCnt);
  TEST_ASSERT_EQUAL_UINT32(2, f.bd.stats().rejected);
  TEST_ASSERT_EQUAL_UINT32(1, f.bd.stats().errors);  // не росте
}

void test_bd_read_error_and_timeout_map_to_gone() {
  {
    BdFix f;
    f.L.errAtBlock = 1;
    uint8_t b[512];
    TEST_ASSERT_FALSE(f.bd.readSector(0, b));
    TEST_ASSERT_FALSE(f.bd.present());
    TEST_ASSERT_EQUAL_INT(E(DiskErr::ReadError), E(f.bd.lastError()));
  }
  {
    BdFix f;
    f.L.truncAtBlock = 1;
    uint8_t b[512];
    TEST_ASSERT_FALSE(f.bd.readSector(0, b));
    TEST_ASSERT_FALSE(f.bd.present());
    TEST_ASSERT_EQUAL_INT(E(DiskErr::Timeout), E(f.bd.lastError()));
  }
}

void test_bd_error_mid_multicommand_stops() {
  BdFix f;
  f.L.errAtBlock = 65;  // 8 секторів × 8 блоків = 64 блоки першої команди; 65-й — перший блок другої
  std::vector<uint8_t> b(20 * 512);
  TEST_ASSERT_FALSE(f.bd.readSectors(0, b.data(), 20));
  TEST_ASSERT_EQUAL_INT(2, f.L.cmdCount[0x54]);  // 3-ту не надсилали
  TEST_ASSERT_EQUAL_UINT32(0, f.bd.stats().sectors);  // читання не завершено: успішні сектори не зараховано
  TEST_ASSERT_FALSE(f.bd.present());
}

void test_bd_setpartition_recovers() {
  BdFix f;
  f.L.errAtBlock = 1;
  uint8_t b[512];
  TEST_ASSERT_FALSE(f.bd.readSector(0, b));
  f.L.errAtBlock = -1;
  f.bd.setPartition(100, 500);  // новий open
  TEST_ASSERT_TRUE(f.bd.present());
  TEST_ASSERT_EQUAL_INT(E(DiskErr::Ok), E(f.bd.lastError()));
  TEST_ASSERT_EQUAL_UINT32(0, f.bd.stats().errors);
  TEST_ASSERT_TRUE(f.bd.readSector(0, b));
}

void test_bd_stats_and_secpercmd() {
  BdFix f;
  TEST_ASSERT_EQUAL_UINT32(kCh376SectorsPerCmd, f.d.secPerCmd());
  uint8_t b[512];
  f.bd.readSector(1, b);
  f.bd.readSector(2, b);
  TEST_ASSERT_EQUAL_UINT32(2, f.bd.stats().calls);
  TEST_ASSERT_EQUAL_UINT32(2, f.bd.stats().sectors);
  TEST_ASSERT_EQUAL_UINT32(2, f.bd.stats().cmds);
  TEST_ASSERT_TRUE(f.bd.isDeviceFailure(DiskErr::Timeout));
  TEST_ASSERT_FALSE(f.bd.isDeviceFailure(DiskErr::BadArg));
  TEST_ASSERT_FALSE(f.bd.isDeviceFailure(DiskErr::Ok));
}

// ======================= політика швидкості =======================
void test_neg_passes_and_stays_2m() {
  FakeHw h(64);
  UsbOpenInfo i;
  uint8_t sc[512];
  TEST_ASSERT_EQUAL_INT((int)UsbOpenFail::None, (int)usbNegotiate(h, sc, i));
  TEST_ASSERT_EQUAL_UINT32(2000000, i.baud);
  TEST_ASSERT_EQUAL_INT((int)SelfTest::Passed, (int)i.selfTest);
  TEST_ASSERT_EQUAL_INT(2, h.bring);
  TEST_ASSERT_EQUAL_UINT32(i.refHash, i.fastHash);
  TEST_ASSERT_EQUAL_UINT8(2, i.nSteps);
}

void test_neg_hash_mismatch_falls_back_to_1m() {
  FakeHw h(64);
  h.flip2M = true;
  UsbOpenInfo i;
  uint8_t sc[512];
  TEST_ASSERT_EQUAL_INT((int)UsbOpenFail::None, (int)usbNegotiate(h, sc, i));
  TEST_ASSERT_EQUAL_UINT32(1000000, i.baud);
  TEST_ASSERT_EQUAL_INT((int)SelfTest::Mismatch, (int)i.selfTest);
  TEST_ASSERT_EQUAL_INT(3, h.bring);  // 1М, 2М, назад 1М
  TEST_ASSERT_TRUE(i.refHash != i.fastHash);
}

void test_neg_fast_negotiation_fails() {
  FakeHw h(64);
  h.r2M = Ch376Err::BaudFail;
  UsbOpenInfo i;
  uint8_t sc[512];
  TEST_ASSERT_EQUAL_INT((int)UsbOpenFail::None, (int)usbNegotiate(h, sc, i));
  TEST_ASSERT_EQUAL_UINT32(1000000, i.baud);
  TEST_ASSERT_EQUAL_INT((int)SelfTest::FastFailed, (int)i.selfTest);
}

void test_neg_fast_read_fails() {
  FakeHw h(64);
  h.rd2M = DiskErr::Timeout;
  UsbOpenInfo i;
  uint8_t sc[512];
  TEST_ASSERT_EQUAL_INT((int)UsbOpenFail::None, (int)usbNegotiate(h, sc, i));
  TEST_ASSERT_EQUAL_UINT32(1000000, i.baud);
  TEST_ASSERT_EQUAL_INT((int)SelfTest::FastFailed, (int)i.selfTest);
  TEST_ASSERT_EQUAL_INT(E(DiskErr::Timeout), E(i.diskErr));
}

void test_neg_return_to_1m_fails_is_nochip() {
  FakeHw h(64);
  h.flip2M = true;
  h.back1M = Ch376Err::BaudFail;
  UsbOpenInfo i;
  uint8_t sc[512];
  TEST_ASSERT_EQUAL_INT((int)UsbOpenFail::NoChip, (int)usbNegotiate(h, sc, i));
}

void test_neg_ref_link_fail_fast_ok_unverified() {
  FakeHw h(64);
  h.r1M = Ch376Err::BaudFail;
  UsbOpenInfo i;
  uint8_t sc[512];
  TEST_ASSERT_EQUAL_INT((int)UsbOpenFail::None, (int)usbNegotiate(h, sc, i));
  TEST_ASSERT_EQUAL_UINT32(2000000, i.baud);
  TEST_ASSERT_EQUAL_INT((int)SelfTest::RefFailed, (int)i.selfTest);
}

void test_neg_no_media_stops_early() {
  FakeHw h(64);
  h.r1M = Ch376Err::Timeout;  // немає флешки
  UsbOpenInfo i;
  uint8_t sc[512];
  TEST_ASSERT_EQUAL_INT((int)UsbOpenFail::NoMedia, (int)usbNegotiate(h, sc, i));
  TEST_ASSERT_EQUAL_INT(1, h.bring);  // інші швидкості не пробуємо (кожна чекала б 3 с)
}

void test_neg_slow_link_refused() {
  FakeHw h(64);
  h.r1M = Ch376Err::BaudFail;
  h.r2M = Ch376Err::BaudFail;
  h.rAuto = Ch376Err::Ok;  // чіп живий на 460800
  UsbOpenInfo i;
  uint8_t sc[512];
  TEST_ASSERT_EQUAL_INT((int)UsbOpenFail::SlowLink, (int)usbNegotiate(h, sc, i));
  TEST_ASSERT_TRUE(i.baud < kCh376MinPlayBaud);
}

void test_neg_chip_absent() {
  FakeHw h(64);
  h.r1M = h.r2M = h.rAuto = Ch376Err::NoReply;
  UsbOpenInfo i;
  uint8_t sc[512];
  TEST_ASSERT_EQUAL_INT((int)UsbOpenFail::NoChip, (int)usbNegotiate(h, sc, i));
  TEST_ASSERT_EQUAL_INT(3, h.bring);
}

void test_neg_ref_read_disconnected_is_nomedia() {
  FakeHw h(64);
  h.rd1M = DiskErr::Disconnected;
  UsbOpenInfo i;
  uint8_t sc[512];
  TEST_ASSERT_EQUAL_INT((int)UsbOpenFail::NoMedia, (int)usbNegotiate(h, sc, i));
}

void test_core_port_fail() {
  FakeHw h(64);
  h.portOk = false;
  UsbOpenInfo i;
  uint8_t sc[512];
  ProbeReport pr;
  TEST_ASSERT_EQUAL_INT((int)UsbOpenFail::NoPort, (int)usbOpenCore(h, sc, pr, i));
  TEST_ASSERT_EQUAL_INT(0, h.bring);
}

// ======================= вибір розділу =======================
UsbOpenFail pick(FakeHw& h, UsbOpenInfo& i) {
  uint8_t sc[512];
  ProbeReport pr;
  i = UsbOpenInfo{};
  i.capacity = h.cap;
  return usbPickPartition(h, sc, pr, i);
}

void test_pick_mbr_first_fat32() {
  FakeHw h(70100);
  mbrEntry(h.at(0), 0, 0x0C, 2048, 68000);
  fat32Boot(h, 2048, 68000);
  UsbOpenInfo i;
  TEST_ASSERT_EQUAL_INT((int)UsbOpenFail::None, (int)pick(h, i));
  TEST_ASSERT_EQUAL_UINT32(2048, i.partStart);
  TEST_ASSERT_EQUAL_UINT32(68000, i.partSectors);
  TEST_ASSERT_EQUAL_UINT8(1, i.partIndex);
  TEST_ASSERT_EQUAL_INT((int)VolKind::Fat32, (int)i.kind);
  TEST_ASSERT_EQUAL_STRING("MEDIA", i.label);
}

void test_pick_big_reserved_and_reserved32_identical_path() {
  for (uint16_t res : {(uint16_t)2676, (uint16_t)32}) {
    FakeHw h(74100);
    mbrEntry(h.at(0), 0, 0x0C, 2048, 72000);
    bootFat(h.at(2048), 1, res, 0, 0, 72000, 0, 500, "USB        ");
    UsbOpenInfo i;
    TEST_ASSERT_EQUAL_INT((int)UsbOpenFail::None, (int)pick(h, i));
    TEST_ASSERT_EQUAL_UINT32(2048, i.partStart);
  }
}

void test_pick_skips_unusable_first_partition() {
  FakeHw h(80000);
  mbrEntry(h.at(0), 0, 0x07, 100, 1000);  // NTFS/чужа ФС: сектор 100 порожній
  mbrEntry(h.at(0), 1, 0x0C, 4096, 70000);
  fat32Boot(h, 4096, 70000);
  UsbOpenInfo i;
  TEST_ASSERT_EQUAL_INT((int)UsbOpenFail::None, (int)pick(h, i));
  TEST_ASSERT_EQUAL_UINT32(4096, i.partStart);
  TEST_ASSERT_EQUAL_UINT8(2, i.partIndex);
}

void test_pick_only_foreign_fs_is_unsupported() {
  FakeHw h(4000);
  mbrEntry(h.at(0), 0, 0x07, 100, 1000);
  memcpy(h.at(100) + 3, "NTFS    ", 8);
  UsbOpenInfo i;
  TEST_ASSERT_EQUAL_INT((int)UsbOpenFail::UnsupportedFs, (int)pick(h, i));
}

void test_pick_gpt_first_entry_size_from_bpb() {
  FakeHw h(70100);
  mbrEntry(h.at(0), 0, 0xEE, 1, 70099);
  memcpy(h.at(1), "EFI PART", 8);
  put64(h.at(1) + 0x48, 2);
  put32(h.at(1) + 0x50, 128);
  put32(h.at(1) + 0x54, 128);
  h.at(2)[0] = 0x01;
  put64(h.at(2) + 0x20, 3084);
  put64(h.at(2) + 0x28, 3084 + 60000 - 1);
  fat32Boot(h, 3084, 60000);
  UsbOpenInfo i;
  TEST_ASSERT_EQUAL_INT((int)UsbOpenFail::None, (int)pick(h, i));
  TEST_ASSERT_EQUAL_UINT32(3084, i.partStart);
  TEST_ASSERT_EQUAL_UINT32(60000, i.partSectors);
  TEST_ASSERT_EQUAL_UINT8(0, i.partIndex);
}

void test_pick_exfat_superfloppy() {
  FakeHw h(100);
  uint8_t* s = h.at(0);
  memcpy(s + 3, "EXFAT   ", 8);
  put64(s + 0x48, 100);
  put32(s + 0x50, 24);
  put32(s + 0x54, 8);
  put32(s + 0x58, 32);
  put32(s + 0x5C, 50);
  put32(s + 0x60, 5);
  s[0x6C] = 9;
  s[0x6D] = 3;
  s[0x6E] = 1;
  s[510] = 0x55;
  s[511] = 0xAA;
  UsbOpenInfo i;
  TEST_ASSERT_EQUAL_INT((int)UsbOpenFail::None, (int)pick(h, i));
  TEST_ASSERT_EQUAL_INT((int)VolKind::ExFat, (int)i.kind);
  TEST_ASSERT_EQUAL_UINT32(0, i.partStart);
  TEST_ASSERT_EQUAL_UINT32(100, i.partSectors);
  TEST_ASSERT_EQUAL_STRING("", i.label);
}

void test_pick_exfat_4k_sector_unsupported() {
  FakeHw h(100);
  uint8_t* s = h.at(0);
  memcpy(s + 3, "EXFAT   ", 8);
  put64(s + 0x48, 100);
  put32(s + 0x5C, 50);
  put32(s + 0x60, 5);
  s[0x6C] = 12;
  s[0x6D] = 3;
  s[0x6E] = 1;
  s[510] = 0x55;
  s[511] = 0xAA;
  UsbOpenInfo i;
  TEST_ASSERT_EQUAL_INT((int)UsbOpenFail::UnsupportedFs, (int)pick(h, i));
}

void test_pick_fat12_unsupported_and_empty_is_nopartition() {
  {
    FakeHw h(2880);
    bootFat(h.at(0), 1, 1, 224, 2880, 0, 9, 0, nullptr);
    UsbOpenInfo i;
    TEST_ASSERT_EQUAL_INT((int)UsbOpenFail::UnsupportedFs, (int)pick(h, i));
  }
  {
    FakeHw h(64);
    UsbOpenInfo i;
    TEST_ASSERT_EQUAL_INT((int)UsbOpenFail::NoPartition, (int)pick(h, i));
  }
  {
    FakeHw h(64);
    mbrEntry(h.at(0), 0, 0, 0, 0);  // таблиця без записів
    UsbOpenInfo i;
    TEST_ASSERT_EQUAL_INT((int)UsbOpenFail::NoPartition, (int)pick(h, i));
  }
}

void test_pick_read_failures() {
  {
    FakeHw h(64);
    h.rd1M = DiskErr::ReadError;
    h.cur = 1000000;
    UsbOpenInfo i;
    TEST_ASSERT_EQUAL_INT((int)UsbOpenFail::ReadError, (int)pick(h, i));
  }
  {
    FakeHw h(64);
    h.rd1M = DiskErr::Disconnected;
    h.cur = 1000000;
    UsbOpenInfo i;
    TEST_ASSERT_EQUAL_INT((int)UsbOpenFail::NoMedia, (int)pick(h, i));
  }
  {  // захисний MBR є, а сектор тому за межею диска → читання тому не вдалось
    FakeHw h(100);
    mbrEntry(h.at(0), 0, 0x0C, 5000, 1000);
    UsbOpenInfo i;
    TEST_ASSERT_EQUAL_INT((int)UsbOpenFail::ReadError, (int)pick(h, i));
  }
}

void test_pick_size_clamped_to_capacity() {
  FakeHw h(10000);
  mbrEntry(h.at(0), 0, 0x0C, 2048, 90000);  // таблиця бреше: більше за диск
  fat32Boot(h, 2048, 90000);
  UsbOpenInfo i;
  TEST_ASSERT_EQUAL_INT((int)UsbOpenFail::None, (int)pick(h, i));
  TEST_ASSERT_EQUAL_UINT32(10000 - 2048, i.partSectors);
}

void test_core_full_open_ok_and_capacity_error() {
  {
    FakeHw h(70100);
    mbrEntry(h.at(0), 0, 0x0C, 2048, 68000);
    fat32Boot(h, 2048, 68000);
    uint8_t sc[512];
    ProbeReport pr;
    UsbOpenInfo i;
    TEST_ASSERT_EQUAL_INT((int)UsbOpenFail::None, (int)usbOpenCore(h, sc, pr, i));
    TEST_ASSERT_EQUAL_UINT32(70100, i.capacity);
    TEST_ASSERT_EQUAL_UINT32(2000000, i.baud);
  }
  {
    FakeHw h(64);
    h.capErr = DiskErr::Timeout;
    uint8_t sc[512];
    ProbeReport pr;
    UsbOpenInfo i;
    TEST_ASSERT_EQUAL_INT((int)UsbOpenFail::ReadError, (int)usbOpenCore(h, sc, pr, i));
  }
}

// ======================= сеанс на справжньому Ch376Disk + mock-чіп =======================
struct MockHw : IUsbOpenHw {
  Ch376Disk* d = nullptr;
  uint32_t cur = 0;
  int portEnds = 0;
  Ch376Err r1M = Ch376Err::Ok;
  void attach(Ch376Disk& x) override { d = &x; }
  bool portBegin() override { return true; }
  Ch376Err bringUp(uint32_t b) override {
    if (b == kUsbBaudRef && r1M != Ch376Err::Ok) return r1M;
    cur = b;
    return Ch376Err::Ok;
  }
  uint32_t baud() const override { return cur; }
  DiskErr capacity(uint32_t& s) override { return d->capacity(s); }
  DiskErr readSector(uint32_t lba, uint8_t* dst) override { return d->readSectors(lba, 1, dst); }
  void portEnd() override { portEnds++; }
};

void buildImage(MockDiskLink& L) {
  std::vector<uint8_t>& im = L.image();
  memset(&im[0], 0, 512);
  mbrEntry(&im[0], 0, 0x0C, 2048, 70000);
  bootFat(&im[(size_t)2048 * 512], 1, 32, 0, 0, 70000, 0, 500, "SESSION    ");
}

// Прогрів: mock-чіп росте власні вектори під час перших читань; рахуємо виділення вже після нього.
void warmUp(MockDiskLink& L) {
  MockHw hw;
  UsbSession s;
  s.open(L, hw);
  s.close();
}

void test_session_open_close_reads_partition() {
  MockDiskLink L(74000);
  buildImage(L);
  warmUp(L);
  MockHw hw;
  UsbSession s;
  const long base = g_live;
  TEST_ASSERT_EQUAL_INT((int)UsbOpenFail::None, (int)s.open(L, hw));
  TEST_ASSERT_TRUE(s.isOpen());
  TEST_ASSERT_EQUAL_UINT32(2048, s.info().partStart);
  TEST_ASSERT_EQUAL_UINT32(70000, s.dev()->sectorCount());
  TEST_ASSERT_EQUAL_STRING("SESSION", s.info().label);
  TEST_ASSERT_EQUAL_UINT32(2000000, s.info().baud);
  uint8_t b[512];
  TEST_ASSERT_TRUE(s.dev()->readSector(1, b));  // сектор 1 розділу = LBA 2049
  TEST_ASSERT_TRUE(patOk(b, 2049, 1));
  TEST_ASSERT_EQUAL_INT((int)UsbOpenFail::AlreadyOpen, (int)s.open(L, hw));
  s.close();
  TEST_ASSERT_FALSE(s.isOpen());
  s.close();  // ідемпотентно
  TEST_ASSERT_EQUAL_INT(base, g_live);
}

void test_session_20_cycles_no_leak() {
  MockDiskLink L(74000);
  buildImage(L);
  warmUp(L);
  const long base = g_live;
  for (int k = 0; k < 20; k++) {
    MockHw hw;
    UsbSession s;
    TEST_ASSERT_EQUAL_INT((int)UsbOpenFail::None, (int)s.open(L, hw));
    TEST_ASSERT_TRUE(s.isOpen());
    uint8_t b[512];
    TEST_ASSERT_TRUE(s.dev()->readSector(0, b));
    s.close();
    TEST_ASSERT_EQUAL_INT(base, g_live);
  }
  TEST_ASSERT_EQUAL_INT(base, g_live);
}

void test_session_failure_frees_everything_and_closes_port() {
  MockDiskLink L(24000);  // без таблиці й ФС (шаблонне сміття) → розділу немає
  warmUp(L);
  const long base = g_live;
  for (int k = 0; k < 20; k++) {
    MockHw hw;
    UsbSession s;
    const UsbOpenFail f = s.open(L, hw);
    TEST_ASSERT_TRUE(f == UsbOpenFail::NoPartition || f == UsbOpenFail::UnsupportedFs);
    TEST_ASSERT_FALSE(s.isOpen());
    TEST_ASSERT_EQUAL_INT(1, hw.portEnds);
    TEST_ASSERT_EQUAL_INT(base, g_live);
  }
  {
    MockHw hw;
    hw.r1M = Ch376Err::Timeout;
    UsbSession s;
    TEST_ASSERT_EQUAL_INT((int)UsbOpenFail::NoMedia, (int)s.open(L, hw));
    TEST_ASSERT_EQUAL_INT(1, hw.portEnds);
    TEST_ASSERT_EQUAL_INT(base, g_live);
  }
}

void test_session_eject_after_open_is_clean() {
  MockDiskLink L(74000);
  buildImage(L);
  MockHw hw;
  UsbSession s;
  TEST_ASSERT_EQUAL_INT((int)UsbOpenFail::None, (int)s.open(L, hw));
  L.ejectAtBlock = L.blockCnt + 1;
  uint8_t b[512];
  TEST_ASSERT_FALSE(s.dev()->readSector(5, b));
  TEST_ASSERT_FALSE(s.dev()->present());
  TEST_ASSERT_FALSE(s.dev()->readSector(6, b));  // без зависань і звернень до чіпа
  s.close();
}

void test_static_footprint() {
  TEST_ASSERT_TRUE(sizeof(UsbSession) + sizeof(UsbOpenInfo) <= 300);
  TEST_ASSERT_TRUE(sizeof(UsbOpenInfo) <= 160);
  TEST_ASSERT_EQUAL_STRING("nomedia", usbOpenFailName(UsbOpenFail::NoMedia));
  TEST_ASSERT_TRUE(strlen(usbOpenFailText(UsbOpenFail::SlowLink)) > 10);
}

}  // namespace

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_bd_offset);
  RUN_TEST(test_bd_split_with_tail);
  RUN_TEST(test_bd_exact_multiple_and_single);
  RUN_TEST(test_bd_edge_and_beyond);
  RUN_TEST(test_bd_bad_args);
  RUN_TEST(test_bd_partition_clamp);
  RUN_TEST(test_bd_eject_is_sticky_gone);
  RUN_TEST(test_bd_read_error_and_timeout_map_to_gone);
  RUN_TEST(test_bd_error_mid_multicommand_stops);
  RUN_TEST(test_bd_setpartition_recovers);
  RUN_TEST(test_bd_stats_and_secpercmd);
  RUN_TEST(test_neg_passes_and_stays_2m);
  RUN_TEST(test_neg_hash_mismatch_falls_back_to_1m);
  RUN_TEST(test_neg_fast_negotiation_fails);
  RUN_TEST(test_neg_fast_read_fails);
  RUN_TEST(test_neg_return_to_1m_fails_is_nochip);
  RUN_TEST(test_neg_ref_link_fail_fast_ok_unverified);
  RUN_TEST(test_neg_no_media_stops_early);
  RUN_TEST(test_neg_slow_link_refused);
  RUN_TEST(test_neg_chip_absent);
  RUN_TEST(test_neg_ref_read_disconnected_is_nomedia);
  RUN_TEST(test_core_port_fail);
  RUN_TEST(test_pick_mbr_first_fat32);
  RUN_TEST(test_pick_big_reserved_and_reserved32_identical_path);
  RUN_TEST(test_pick_skips_unusable_first_partition);
  RUN_TEST(test_pick_only_foreign_fs_is_unsupported);
  RUN_TEST(test_pick_gpt_first_entry_size_from_bpb);
  RUN_TEST(test_pick_exfat_superfloppy);
  RUN_TEST(test_pick_exfat_4k_sector_unsupported);
  RUN_TEST(test_pick_fat12_unsupported_and_empty_is_nopartition);
  RUN_TEST(test_pick_read_failures);
  RUN_TEST(test_pick_size_clamped_to_capacity);
  RUN_TEST(test_core_full_open_ok_and_capacity_error);
  RUN_TEST(test_session_open_close_reads_partition);
  RUN_TEST(test_session_20_cycles_no_leak);
  RUN_TEST(test_session_failure_frees_everything_and_closes_port);
  RUN_TEST(test_session_eject_after_open_is_clean);
  RUN_TEST(test_static_footprint);
  return UNITY_END();
}

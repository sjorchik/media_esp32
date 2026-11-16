#include <stdio.h>
#include <string.h>
#include <unity.h>

#include <vector>

#include "config.h"
#include "mock_disk_link.h"
#include "usb/ch376_disk.h"
#include "usb/disk_probe.h"

// Тести шару секторів CH376S (M4b2) на mock-у з test/test_ch376/mock_disk_link.h.

namespace {

bool patOk(const uint8_t* b, uint32_t lba, uint32_t n) {
  for (uint32_t k = 0; k < n * 512u; k++)
    if (b[k] != diskPattern(lba * 512u + k)) return false;
  return true;
}

struct Fix {
  MockDiskLink L;
  Ch376Disk d;
  explicit Fix(uint32_t sectors = 2048) : L(sectors), d(L) {}
};

int E(DiskErr e) { return (int)e; }

void test_capacity_and_cache() {
  Fix f;
  uint32_t c = 0;
  TEST_ASSERT_EQUAL_INT(E(DiskErr::Ok), E(f.d.capacity(c)));
  TEST_ASSERT_EQUAL_UINT32(2048, c);
  TEST_ASSERT_TRUE(f.d.capacityKnown());
  TEST_ASSERT_EQUAL_UINT32(2048, f.d.cachedCapacity());
  TEST_ASSERT_EQUAL_INT(1, f.L.cmdCount[0x3E]);
  std::vector<uint8_t> b(512);
  TEST_ASSERT_EQUAL_INT(E(DiskErr::Ok), E(f.d.readSectors(2047, 1, b.data())));
  TEST_ASSERT_TRUE(patOk(b.data(), 2047, 1));
  const int before = f.L.cmdCount[0x54];
  TEST_ASSERT_EQUAL_INT(E(DiskErr::BadArg), E(f.d.readSectors(2048, 1, b.data())));
  TEST_ASSERT_EQUAL_INT(before, f.L.cmdCount[0x54]);  // за межами — жодної команди чіпу
}

void test_read_one_sector_data_and_stats() {
  Fix f;
  std::vector<uint8_t> b(512);
  TEST_ASSERT_EQUAL_INT(E(DiskErr::Ok), E(f.d.readSectors(5, 1, b.data())));
  TEST_ASSERT_TRUE(patOk(b.data(), 5, 1));
  TEST_ASSERT_EQUAL_UINT32(1, f.d.stats().cmds);
  TEST_ASSERT_EQUAL_UINT32(1, f.d.stats().sectors);
  TEST_ASSERT_EQUAL_UINT32(8, f.d.stats().blocks);
  TEST_ASSERT_EQUAL_UINT32(0, f.d.stats().errors);
  TEST_ASSERT_EQUAL_UINT32(5, f.L.lastLba);
  TEST_ASSERT_EQUAL_UINT32(1, f.L.lastCount);
}

void test_split_by_secpercmd() {
  Fix f;
  f.d.setSecPerCmd(2);
  std::vector<uint8_t> b(5 * 512);
  TEST_ASSERT_EQUAL_INT(E(DiskErr::Ok), E(f.d.readSectors(100, 5, b.data())));
  TEST_ASSERT_TRUE(patOk(b.data(), 100, 5));
  TEST_ASSERT_EQUAL_INT(3, f.L.cmdCount[0x54]);  // 2 + 2 + 1
  TEST_ASSERT_EQUAL_UINT32(3, f.d.stats().cmds);
  TEST_ASSERT_EQUAL_UINT32(5, f.d.stats().sectors);
}

void test_max_255_in_one_command() {
  Fix f(300);
  f.d.setSecPerCmd(255);
  std::vector<uint8_t> b(255 * 512);
  TEST_ASSERT_EQUAL_INT(E(DiskErr::Ok), E(f.d.readSectors(0, 255, b.data())));
  TEST_ASSERT_TRUE(patOk(b.data(), 0, 255));
  TEST_ASSERT_EQUAL_INT(1, f.L.cmdCount[0x54]);
  TEST_ASSERT_EQUAL_UINT32(255, f.L.lastCount);
  TEST_ASSERT_EQUAL_UINT32(255 * 8, f.d.stats().blocks);
}

void test_discard_mode_null_dst() {
  Fix f;
  TEST_ASSERT_EQUAL_INT(E(DiskErr::Ok), E(f.d.readSectors(10, 4, nullptr)));
  TEST_ASSERT_EQUAL_UINT32(4, f.d.stats().sectors);
  TEST_ASSERT_EQUAL_UINT32(32, f.d.stats().blocks);
}

void test_bad_args() {
  Fix f;
  std::vector<uint8_t> b(512);
  TEST_ASSERT_EQUAL_INT(E(DiskErr::BadArg), E(f.d.readSectors(0, 0, b.data())));
  f.d.setSecPerCmd(0);
  TEST_ASSERT_EQUAL_UINT32(8, f.d.secPerCmd());  // 0 — ігнорується
  f.d.setSecPerCmd(256);
  TEST_ASSERT_EQUAL_UINT32(8, f.d.secPerCmd());  // 256 — ігнорується (лічильник 1 Б)
  TEST_ASSERT_EQUAL_INT(0, f.L.cmdCount[0x54]);
}

void test_beyond_capacity_unknown_gives_read_error() {
  Fix f;  // ємність не запитано: перевірки немає, чіп відповідає помилкою
  std::vector<uint8_t> b(2 * 512);
  TEST_ASSERT_EQUAL_INT(E(DiskErr::ReadError), E(f.d.readSectors(2047, 2, b.data())));
  TEST_ASSERT_EQUAL_UINT32(1, f.d.stats().errors);
}

void test_eject_mid_read_then_nodisk() {
  Fix f;
  f.L.ejectAtBlock = 5;
  std::vector<uint8_t> b(2 * 512);
  TEST_ASSERT_EQUAL_INT(E(DiskErr::Disconnected), E(f.d.readSectors(0, 2, b.data())));
  TEST_ASSERT_EQUAL_INT(E(DiskErr::NoDisk), E(f.d.readSectors(0, 1, b.data())));
}

void test_block_error_readerror() {
  Fix f;
  f.L.errAtBlock = 3;
  std::vector<uint8_t> b(512);
  TEST_ASSERT_EQUAL_INT(E(DiskErr::ReadError), E(f.d.readSectors(0, 1, b.data())));
  TEST_ASSERT_EQUAL_UINT32(1, f.d.stats().errors);
  // після збою команда знову працює
  f.L.errAtBlock = -1;
  TEST_ASSERT_EQUAL_INT(E(DiskErr::Ok), E(f.d.readSectors(0, 1, b.data())));
  TEST_ASSERT_TRUE(patOk(b.data(), 0, 1));
}

void test_truncated_block_is_timeout() {
  Fix f;
  f.L.truncAtBlock = 2;
  std::vector<uint8_t> b(512);
  TEST_ASSERT_EQUAL_INT(E(DiskErr::Timeout), E(f.d.readSectors(0, 1, b.data())));
}

void test_garbage_length_is_badreply() {
  Fix f;
  f.L.badLenAtBlock = 1;
  std::vector<uint8_t> b(512);
  TEST_ASSERT_EQUAL_INT(E(DiskErr::BadReply), E(f.d.readSectors(0, 1, b.data())));
}

void test_silent_status_noreply() {
  Fix f;
  f.L.silentStatus = true;
  std::vector<uint8_t> b(512);
  TEST_ASSERT_EQUAL_INT(E(DiskErr::NoReply), E(f.d.readSectors(0, 1, b.data())));
}

void test_abort_requested() {
  Fix f;
  f.L.abortFlag = true;
  std::vector<uint8_t> b(512);
  TEST_ASSERT_EQUAL_INT(E(DiskErr::Aborted), E(f.d.readSectors(0, 1, b.data())));
  TEST_ASSERT_EQUAL_INT(0, f.L.cmdCount[0x54]);
}

void test_int_mode_counts_waits() {
  Fix f;
  std::vector<uint8_t> b(512);
  TEST_ASSERT_EQUAL_INT(E(DiskErr::Ok), E(f.d.readSectors(0, 1, b.data())));
  TEST_ASSERT_TRUE(patOk(b.data(), 0, 1));
  TEST_ASSERT_TRUE(f.d.stats().intWaits > 0);
  TEST_ASSERT_EQUAL_UINT32(0, f.d.stats().intMisses);
}

void test_poll_mode_same_data() {
  Fix f;
  f.d.setUseInt(false);
  std::vector<uint8_t> b(3 * 512);
  TEST_ASSERT_EQUAL_INT(E(DiskErr::Ok), E(f.d.readSectors(40, 3, b.data())));
  TEST_ASSERT_TRUE(patOk(b.data(), 40, 3));
  TEST_ASSERT_EQUAL_UINT32(0, f.d.stats().intWaits);
}

void test_no_intpin_polls() {
  Fix f;
  f.L.intPin = false;
  std::vector<uint8_t> b(512);
  TEST_ASSERT_EQUAL_INT(E(DiskErr::Ok), E(f.d.readSectors(7, 1, b.data())));
  TEST_ASSERT_TRUE(patOk(b.data(), 7, 1));
  TEST_ASSERT_EQUAL_UINT32(0, f.d.stats().intWaits);
}

void test_status_mapping() {
  TEST_ASSERT_EQUAL_INT(E(DiskErr::Ok), E(Ch376Disk::mapStatus(0x14)));
  TEST_ASSERT_EQUAL_INT(E(DiskErr::Disconnected), E(Ch376Disk::mapStatus(0x16)));
  TEST_ASSERT_EQUAL_INT(E(DiskErr::NoDisk), E(Ch376Disk::mapStatus(0x82)));
  TEST_ASSERT_EQUAL_INT(E(DiskErr::ReadError), E(Ch376Disk::mapStatus(0x1F)));
  TEST_ASSERT_EQUAL_INT(E(DiskErr::ReadError), E(Ch376Disk::mapStatus(0x17)));
  TEST_ASSERT_EQUAL_INT(E(DiskErr::ChipStatus), E(Ch376Disk::mapStatus(0x42)));
}

void test_settle_counts_in_status_time() {
  Fix f;
  f.L.byteUs = 100;
  f.d.setSettleBytes(2);
  std::vector<uint8_t> b(512);
  TEST_ASSERT_EQUAL_INT(E(DiskErr::Ok), E(f.d.readSectors(0, 1, b.data())));
  // 8 очікувань блоків + 1 кінцевий = 9 статусів, кожен з паузою 2 × 100 мкс
  TEST_ASSERT_TRUE(f.d.stats().statUs >= 9u * 200u);
}

void test_skip_status_experiment_fewer_polls() {
  Fix f;
  f.d.setSkipStatus(true);
  std::vector<uint8_t> b(512);
  TEST_ASSERT_EQUAL_INT(E(DiskErr::Ok), E(f.d.readSectors(0, 1, b.data())));
  TEST_ASSERT_TRUE(patOk(b.data(), 0, 1));  // у mock дані готові одразу
  TEST_ASSERT_EQUAL_INT(2, f.L.cmdCount[0x22]);  // перший блок + кінець команди
}

void test_stats_and_reset() {
  Fix f;
  std::vector<uint8_t> b(512);
  f.d.readSectors(0, 1, b.data());
  TEST_ASSERT_EQUAL_UINT32(1, f.d.stats().cmds);
  f.d.resetStats();
  TEST_ASSERT_EQUAL_UINT32(0, f.d.stats().cmds);
  f.d.reset();
  TEST_ASSERT_FALSE(f.d.capacityKnown());
  TEST_ASSERT_EQUAL_UINT32(8, f.d.secPerCmd());
}


// ---- Розбір структур (M4b2): образи в памʼяті ----
struct Img : SectorReader {
  std::vector<uint8_t> s;
  uint32_t n;
  explicit Img(uint32_t sectors) : s((size_t)sectors * 512u, 0), n(sectors) {}
  bool readSector(uint32_t lba, uint8_t* dst) override {
    if (lba >= n) return false;
    memcpy(dst, &s[(size_t)lba * 512u], 512);
    return true;
  }
  uint8_t* at(uint32_t lba) { return &s[(size_t)lba * 512u]; }
};

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

// BPB FAT (звичайний 55AA + перехід). fat32 != 0 — FAT32-поля.
void bootFat(uint8_t* s, uint16_t bps, uint8_t spc, uint16_t res, uint8_t nf, uint16_t rootEnt, uint16_t tot16,
             uint32_t tot32, uint16_t fat16, uint32_t fat32) {
  memset(s, 0, 512);
  s[0] = 0xEB;
  s[1] = 0x58;
  s[2] = 0x90;
  put16(s + 11, bps);
  s[13] = spc;
  put16(s + 14, res);
  s[16] = nf;
  put16(s + 17, rootEnt);
  put16(s + 19, tot16);
  s[21] = 0xF8;
  put16(s + 22, fat16);
  put32(s + 32, tot32);
  put32(s + 36, fat32);
  s[510] = 0x55;
  s[511] = 0xAA;
}

// MBR з одним розділом типу t на LBA start (sectors).
void mbrOne(uint8_t* s, uint8_t t, uint32_t start, uint32_t sectors) {
  memset(s, 0, 512);
  uint8_t* e = s + 0x1BE;
  e[4] = t;
  put32(e + 8, start);
  put32(e + 12, sectors);
  s[510] = 0x55;
  s[511] = 0xAA;
}

// Образ: MBR → FAT32 на 2048 (spc 1, res, tot 72000 або 68000, fat 500).
void buildMbrFat32(Img& im, uint16_t res, uint32_t tot) {
  mbrOne(im.at(0), 0x0C, 2048, tot);
  bootFat(im.at(2048), 512, 1, res, 2, 0, 0, tot, 0, 500);
  put32(im.at(2048) + 44, 2);  // BPB_RootClus
  memcpy(im.at(2048) + 0x47, "MEDIA      ", 11);
}

void test_probe_mbr_fat32_reserved32() {
  Img im(70100);
  buildMbrFat32(im, 32, 68000);
  ProbeReport r;
  uint8_t sc[512];
  TEST_ASSERT_TRUE(probeDisk(im, sc, r));
  TEST_ASSERT_TRUE(r.mbr);
  TEST_ASSERT_EQUAL_UINT8(1, r.partCount);
  TEST_ASSERT_EQUAL_UINT8(0x0C, r.parts[0].type);
  TEST_ASSERT_EQUAL_UINT32(2048, r.parts[0].startLba);
  TEST_ASSERT_EQUAL_UINT32(2048, r.volStart);
  TEST_ASSERT_EQUAL_INT((int)VolKind::Fat32, (int)r.kind);
  TEST_ASSERT_TRUE(r.readable);
  TEST_ASSERT_EQUAL_UINT32(32, r.bpb.reserved);
  TEST_ASSERT_EQUAL_UINT32(2, r.bpb.rootClus);
  TEST_ASSERT_EQUAL_STRING("MEDIA", r.bpb.label);
}

void test_probe_fat32_reserved_4598_is_full_field() {
  Img im(74100);
  buildMbrFat32(im, 4598, 72000);  // reserved > 255: чіп бачив би лише молодший байт
  ProbeReport r;
  uint8_t sc[512];
  TEST_ASSERT_TRUE(probeDisk(im, sc, r));
  TEST_ASSERT_EQUAL_INT((int)VolKind::Fat32, (int)r.kind);
  TEST_ASSERT_TRUE(r.readable);
  TEST_ASSERT_EQUAL_UINT32(4598, r.bpb.reserved);
  TEST_ASSERT_EQUAL_UINT32(4598 + 1000, r.bpb.dataStart);
}

void test_probe_protective_gpt_first_partition() {
  Img im(70100);
  mbrOne(im.at(0), 0xEE, 1, 70099);
  memcpy(im.at(1), "EFI PART", 8);
  put64(im.at(1) + 0x48, 2);    // PartitionEntryLBA
  put32(im.at(1) + 0x50, 128);  // NumberOfPartitionEntries
  put32(im.at(1) + 0x54, 128);  // SizeOfPartitionEntry
  im.at(2)[0] = 0x01;           // непорожній тип (GUID)
  put64(im.at(2) + 0x20, 2048);
  put64(im.at(2) + 0x28, 2048 + 68000 - 1);
  bootFat(im.at(2048), 512, 1, 32, 2, 0, 0, 68000, 0, 500);
  ProbeReport r;
  uint8_t sc[512];
  TEST_ASSERT_TRUE(probeDisk(im, sc, r));
  TEST_ASSERT_TRUE(r.protectiveGpt);
  TEST_ASSERT_TRUE(r.gptHeader);
  TEST_ASSERT_TRUE(r.gptFirstFound);
  TEST_ASSERT_EQUAL_UINT32(2048, r.volStart);
  TEST_ASSERT_EQUAL_INT((int)VolKind::Fat32, (int)r.kind);
  TEST_ASSERT_TRUE(r.readable);
}

void test_probe_gpt_without_header_is_none() {
  Img im(16);
  mbrOne(im.at(0), 0xEE, 1, 15);  // захисний MBR, але LBA1 порожній
  ProbeReport r;
  uint8_t sc[512];
  TEST_ASSERT_TRUE(probeDisk(im, sc, r));
  TEST_ASSERT_TRUE(r.protectiveGpt);
  TEST_ASSERT_FALSE(r.gptHeader);
  TEST_ASSERT_EQUAL_INT((int)VolKind::None, (int)r.kind);
  TEST_ASSERT_FALSE(r.readable);
}

void test_probe_exfat_superfloppy() {
  Img im(100);
  uint8_t* s = im.at(0);
  memcpy(s + 3, "EXFAT   ", 8);
  put64(s + 0x40, 0);
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
  ProbeReport r;
  uint8_t sc[512];
  TEST_ASSERT_TRUE(probeDisk(im, sc, r));
  TEST_ASSERT_EQUAL_INT((int)VolKind::ExFat, (int)r.kind);
  TEST_ASSERT_TRUE(r.ex.valid);
  TEST_ASSERT_EQUAL_UINT32(50, r.ex.clusterCount);
  TEST_ASSERT_EQUAL_UINT32(5, r.ex.rootCluster);
  TEST_ASSERT_FALSE(r.readable);
}

void test_probe_fat16_superfloppy_and_label() {
  Img im(20000);
  bootFat(im.at(0), 512, 4, 1, 2, 512, 20000, 0, 40, 0);
  memcpy(im.at(0) + 0x2B, "MY DISK    ", 11);
  ProbeReport r;
  uint8_t sc[512];
  TEST_ASSERT_TRUE(probeDisk(im, sc, r));
  TEST_ASSERT_FALSE(r.mbr);
  TEST_ASSERT_EQUAL_UINT32(0, r.volStart);
  TEST_ASSERT_EQUAL_INT((int)VolKind::Fat16, (int)r.kind);
  TEST_ASSERT_TRUE(r.readable);
  TEST_ASSERT_EQUAL_STRING("MY DISK", r.bpb.label);
}

void test_probe_fat12_floppy() {
  Img im(2880);
  bootFat(im.at(0), 512, 1, 1, 2, 224, 2880, 0, 9, 0);
  ProbeReport r;
  uint8_t sc[512];
  TEST_ASSERT_TRUE(probeDisk(im, sc, r));
  TEST_ASSERT_EQUAL_INT((int)VolKind::Fat12, (int)r.kind);
  TEST_ASSERT_EQUAL_UINT32(2847, r.bpb.clusters);
}

void test_probe_empty_sector_is_none() {
  Img im(16);
  ProbeReport r;
  uint8_t sc[512];
  TEST_ASSERT_TRUE(probeDisk(im, sc, r));
  TEST_ASSERT_FALSE(r.mbr);
  TEST_ASSERT_EQUAL_INT((int)VolKind::None, (int)r.kind);
  TEST_ASSERT_FALSE(r.readable);
}

void test_probe_read_failure_returns_false() {
  Img im(0);
  ProbeReport r;
  uint8_t sc[512];
  TEST_ASSERT_FALSE(probeDisk(im, sc, r));
}

void test_probe_volume_read_failure_is_unknown() {
  Img im(100);
  mbrOne(im.at(0), 0x0C, 5000, 1000);  // розділ за межами образа
  ProbeReport r;
  uint8_t sc[512];
  TEST_ASSERT_TRUE(probeDisk(im, sc, r));
  TEST_ASSERT_EQUAL_UINT32(5000, r.volStart);
  TEST_ASSERT_EQUAL_INT((int)VolKind::Unknown, (int)r.kind);
  TEST_ASSERT_FALSE(r.readable);
}

void test_probe_bad_bytes_per_sector_rejected() {
  Img im(16);
  bootFat(im.at(0), 300, 1, 1, 2, 0, 0, 1000, 0, 10);  // 300 Б/сектор неможливо
  ProbeReport r;
  uint8_t sc[512];
  TEST_ASSERT_TRUE(probeDisk(im, sc, r));
  TEST_ASSERT_FALSE(r.bpb.valid);
  TEST_ASSERT_EQUAL_INT((int)VolKind::None, (int)r.kind);
}

void test_parse_units_and_thresholds() {
  TEST_ASSERT_EQUAL_INT((int)VolKind::Fat12, (int)fatKindFromClusters(4084));
  TEST_ASSERT_EQUAL_INT((int)VolKind::Fat16, (int)fatKindFromClusters(4085));
  TEST_ASSERT_EQUAL_INT((int)VolKind::Fat16, (int)fatKindFromClusters(65524));
  TEST_ASSERT_EQUAL_INT((int)VolKind::Fat32, (int)fatKindFromClusters(65525));
  TEST_ASSERT_TRUE(volKindName(VolKind::ExFat) != nullptr);

  uint8_t s[512];
  memset(s, 0, sizeof s);
  s[510] = 0x55;
  s[511] = 0xAA;
  s[0] = 0xEB;
  s[2] = 0x90;
  BpbInfo b;
  TEST_ASSERT_FALSE(parseBpb(s, b));  // fatSz = 0

  memset(s, 0, sizeof s);
  memcpy(s + 3, "EXFAT   ", 8);
  s[510] = 0x55;
  s[511] = 0xAA;
  s[0x0B] = 1;  // MustBeZero порушено
  ExFatInfo e;
  TEST_ASSERT_FALSE(parseExFat(s, e));

  memset(s, 0, sizeof s);
  uint32_t el = 0, c = 0, z = 0;
  TEST_ASSERT_FALSE(parseGptHeader(s, el, c, z));  // немає «EFI PART»
  uint32_t st = 0, sec = 0;
  TEST_ASSERT_FALSE(parseGptFirst(s, 128, st, sec));  // усі записи порожні
}

void test_probe_label_non_printable_becomes_question() {
  Img im(20000);
  bootFat(im.at(0), 512, 4, 1, 2, 512, 20000, 0, 40, 0);
  memcpy(im.at(0) + 0x2B, "A\x01" "B        ", 11);
  ProbeReport r;
  uint8_t sc[512];
  TEST_ASSERT_TRUE(probeDisk(im, sc, r));
  TEST_ASSERT_EQUAL_STRING("A?B", r.bpb.label);
}

}  // namespace

void run_disk_tests() {
  RUN_TEST(test_capacity_and_cache);
  RUN_TEST(test_read_one_sector_data_and_stats);
  RUN_TEST(test_split_by_secpercmd);
  RUN_TEST(test_max_255_in_one_command);
  RUN_TEST(test_discard_mode_null_dst);
  RUN_TEST(test_bad_args);
  RUN_TEST(test_beyond_capacity_unknown_gives_read_error);
  RUN_TEST(test_eject_mid_read_then_nodisk);
  RUN_TEST(test_block_error_readerror);
  RUN_TEST(test_truncated_block_is_timeout);
  RUN_TEST(test_garbage_length_is_badreply);
  RUN_TEST(test_silent_status_noreply);
  RUN_TEST(test_abort_requested);
  RUN_TEST(test_int_mode_counts_waits);
  RUN_TEST(test_poll_mode_same_data);
  RUN_TEST(test_no_intpin_polls);
  RUN_TEST(test_status_mapping);
  RUN_TEST(test_settle_counts_in_status_time);
  RUN_TEST(test_skip_status_experiment_fewer_polls);
  RUN_TEST(test_stats_and_reset);
  RUN_TEST(test_probe_mbr_fat32_reserved32);
  RUN_TEST(test_probe_fat32_reserved_4598_is_full_field);
  RUN_TEST(test_probe_protective_gpt_first_partition);
  RUN_TEST(test_probe_gpt_without_header_is_none);
  RUN_TEST(test_probe_exfat_superfloppy);
  RUN_TEST(test_probe_fat16_superfloppy_and_label);
  RUN_TEST(test_probe_fat12_floppy);
  RUN_TEST(test_probe_empty_sector_is_none);
  RUN_TEST(test_probe_read_failure_returns_false);
  RUN_TEST(test_probe_volume_read_failure_is_unknown);
  RUN_TEST(test_probe_bad_bytes_per_sector_rejected);
  RUN_TEST(test_parse_units_and_thresholds);
  RUN_TEST(test_probe_label_non_printable_becomes_question);
}

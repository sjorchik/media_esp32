#include <stdlib.h>
#include <string.h>
#include <unity.h>

#include <new>
#include <vector>

#include "../test_ch376/mock_disk_link.h"
#include "config.h"
#include "usb/ch376_blockdev.h"
#include "usb/sector_cache.h"

// M4c1: LRU-кеш секторів, читання наперед і блочний пристрій з кешем. Окремий набір; наявні тести не змінено.

static long g_live = 0;
void* operator new(size_t n) { void* p = malloc(n ? n : 1); if (!p) abort(); g_live++; return p; }
void* operator new[](size_t n) { return operator new(n); }
void* operator new(size_t n, const std::nothrow_t&) noexcept { void* p = malloc(n ? n : 1); if (p) g_live++; return p; }
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
bool patOk(const uint8_t* b, uint32_t lba) {
  for (uint32_t k = 0; k < 512; k++) if (b[k] != diskPattern(lba * 512u + k)) return false;
  return true;
}

struct Fix {
  MockDiskLink L;
  Ch376Disk d;
  Ch376BlockDev bd;
  Fix() : L(4096), d(L), bd(d) { bd.setPartition(100, 500); }
  int cmds() { return L.cmdCount[0x54]; }
};
}  // namespace

// ---- SectorCache: логіка
void test_cache_open_close_frees() {
  const long base = g_live;
  SectorCache c;
  TEST_ASSERT_TRUE(c.open(8, 4));
  TEST_ASSERT_TRUE(c.isOpen());
  c.close();
  c.close();  // ідемпотентно
  TEST_ASSERT_FALSE(c.isOpen());
  TEST_ASSERT_EQUAL(base, g_live);
  TEST_ASSERT_TRUE(c.open(0, 1));  // «тінь»
  TEST_ASSERT_EQUAL_UINT8(0, c.slots());
}

void test_cache_lru_evicts_oldest() {
  SectorCache c;
  c.open(4, 1);
  for (uint32_t s = 10; s < 14; s++) {
    c.note(s);
    c.miss();
    int f = c.victimRun(1);
    c.fill(f, s, 1);
  }
  TEST_ASSERT_TRUE(c.find(10) >= 0);
  c.hit(c.find(10));  // 10 — найновіший; найстарший тепер 11
  c.note(20);
  c.fill(c.victimRun(1), 20, 1);
  TEST_ASSERT_TRUE(c.find(11) < 0);
  TEST_ASSERT_TRUE(c.find(10) >= 0);
  TEST_ASSERT_TRUE(c.find(20) >= 0);
  TEST_ASSERT_EQUAL_UINT32(1, c.stats().evictions);
  TEST_ASSERT_EQUAL_UINT32(1, c.stats().hits);
}

void test_cache_invalidate() {
  SectorCache c;
  c.open(4, 1);
  c.fill(c.victimRun(1), 5, 1);
  TEST_ASSERT_TRUE(c.find(5) >= 0);
  c.invalidate();
  TEST_ASSERT_TRUE(c.find(5) < 0);
}

void test_cache_counters_unique_repeat_seq() {
  SectorCache c;
  c.open(0, 1);  // тінь
  const uint32_t seqv[] = {5, 6, 7, 6, 5, 7, 100, 100, 7};
  for (uint32_t s : seqv) c.note(s);
  const SectorCacheStats& st = c.stats();
  TEST_ASSERT_EQUAL_UINT32(9, st.reqs);
  TEST_ASSERT_EQUAL_UINT32(4, st.unique);  // 5,6,7,100
  TEST_ASSERT_EQUAL_UINT32(2, st.seq);     // 5→6, 6→7
  TEST_ASSERT_EQUAL_UINT32(1, st.same);    // 100→100
  TEST_ASSERT_EQUAL_UINT32(5, st.jump);
  TEST_ASSERT_EQUAL_UINT32(5, st.wouldHit);  // повтори: 6, 5, 7, другий 100, 7 — усі в вікні 8
}

void test_cache_plan_and_victim_run() {
  SectorCache c;
  c.open(8, 4);
  c.note(50);
  TEST_ASSERT_EQUAL_UINT32(1, c.planFetch(50, 1000));  // перший запит: не послідовний
  c.fill(c.victimRun(1), 50, 1);
  c.note(51);
  TEST_ASSERT_EQUAL_UINT32(4, c.planFetch(51, 1000));  // послідовний → наперед
  TEST_ASSERT_EQUAL_UINT32(2, c.planFetch(51, 53));    // кінець розділу: 51,52
  int f = c.victimRun(4);
  TEST_ASSERT_TRUE(f >= 1 && f <= 4);  // не накриває слот 0 з сектором 50 (найновіший), якщо є вільні
  TEST_ASSERT_TRUE(c.find(50) >= 0);
}

// ---- блочний пристрій з кешем
void test_bd_cache_hits_avoid_chip() {
  Fix f;
  TEST_ASSERT_TRUE(f.bd.cacheOpen(8, 1));
  uint8_t b[512];
  for (int r = 0; r < 5; r++) {
    TEST_ASSERT_TRUE(f.bd.readSector(7, b));
    TEST_ASSERT_TRUE(patOk(b, 107));
  }
  TEST_ASSERT_EQUAL(1, f.cmds());  // одна команда замість п'яти
  TEST_ASSERT_EQUAL_UINT32(4, f.bd.cacheStats()->hits);
  TEST_ASSERT_EQUAL_UINT32(1, f.bd.cacheStats()->misses);
}

void test_bd_readahead_sequential() {
  Fix f;
  f.bd.cacheOpen(8, 4);
  uint8_t b[512];
  for (uint32_t s = 0; s < 12; s++) {
    TEST_ASSERT_TRUE(f.bd.readSector(s, b));
    TEST_ASSERT_TRUE(patOk(b, 100 + s));
  }
  // 0 (1 сектор), 1..4, 5..8, 9..12 → 4 команди замість 12
  TEST_ASSERT_EQUAL(4, f.cmds());
  TEST_ASSERT_TRUE(f.bd.cacheStats()->raSectors >= 6);
}

void test_bd_readahead_clamped_at_partition_end() {
  Fix f;
  f.bd.cacheOpen(8, 4);
  uint8_t b[512];
  for (uint32_t s = 496; s < 500; s++) TEST_ASSERT_TRUE(f.bd.readSector(s, b));
  TEST_ASSERT_TRUE(patOk(b, 599));
  TEST_ASSERT_TRUE(f.L.lastLba + f.L.lastCount <= 600);  // не читали за розділом
  TEST_ASSERT_EQUAL_UINT32(0, f.bd.stats().rangeErrors);
  TEST_ASSERT_FALSE(f.bd.readSector(500, b));  // за межею — помилка аргументів
  TEST_ASSERT_TRUE(f.bd.present());
}

void test_bd_random_reads_single_sector() {
  Fix f;
  f.bd.cacheOpen(8, 4);
  uint8_t b[512];
  const uint32_t s[] = {10, 200, 30, 400};
  for (uint32_t x : s) TEST_ASSERT_TRUE(f.bd.readSector(x, b));
  TEST_ASSERT_EQUAL(4, f.cmds());
  TEST_ASSERT_EQUAL_UINT32(0, f.bd.cacheStats()->raSectors);
}

void test_bd_multi_sector_bypasses_cache() {
  Fix f;
  f.bd.cacheOpen(8, 4);
  std::vector<uint8_t> big(16 * 512);
  TEST_ASSERT_TRUE(f.bd.readSectors(0, big.data(), 16));
  TEST_ASSERT_EQUAL(2, f.cmds());
  TEST_ASSERT_EQUAL_UINT32(0, f.bd.cacheStats()->reqs);
}

void test_bd_remount_and_gone_invalidate() {
  Fix f;
  f.bd.cacheOpen(8, 1);
  uint8_t b[512];
  f.bd.readSector(3, b);
  f.bd.setPartition(100, 500);  // перемонтування
  f.bd.readSector(3, b);
  TEST_ASSERT_EQUAL(2, f.cmds());  // кеш недійсний → знову до чіпа
  f.bd.markGone(DiskErr::Disconnected);
  TEST_ASSERT_FALSE(f.bd.readSector(3, b));  // Gone: навіть закешований не віддається
  TEST_ASSERT_EQUAL_UINT32(1, f.bd.stats().rejected);
}

void test_bd_cache_freed_after_scan_and_dtor() {
  const long base = g_live;
  {
    Fix f;
    const long b0 = g_live;
    f.bd.cacheOpen(8, 4);
    TEST_ASSERT_TRUE(g_live > b0);
    f.bd.cacheClose();
    TEST_ASSERT_EQUAL(b0, g_live);
    TEST_ASSERT_FALSE(f.bd.cacheOn());
    f.bd.cacheOpen(8, 4);  // лишаємо відкритим: деструктор має звільнити
  }
  TEST_ASSERT_EQUAL(base, g_live);
}

void test_bd_shadow_mode_counts_only() {
  Fix f;
  f.bd.cacheOpen(0, 1);
  uint8_t b[512];
  f.bd.readSector(5, b);
  f.bd.readSector(5, b);
  f.bd.readSector(6, b);
  TEST_ASSERT_EQUAL(3, f.cmds());  // кеша даних нема
  TEST_ASSERT_EQUAL_UINT32(2, f.bd.cacheStats()->unique);
  TEST_ASSERT_EQUAL_UINT32(1, f.bd.cacheStats()->wouldHit);
}

void test_bd_resize_keeps_stats_and_grows() {
  Fix f;
  uint8_t b[512];
  f.bd.readSector(0, b);  // прогрів: мок може виділяти власні буфери при першому читанні
  const long base = g_live;
  f.bd.cacheOpen(4, 1);
  f.bd.readSector(3, b);
  f.bd.readSector(3, b);
  TEST_ASSERT_TRUE(f.bd.cacheResize(8, 4));
  TEST_ASSERT_EQUAL(8, f.bd.cacheSlots());
  TEST_ASSERT_EQUAL_UINT32(2, f.bd.cacheStats()->reqs);  // лічильники перенесено
  TEST_ASSERT_EQUAL_UINT32(1, f.bd.cacheStats()->hits);
  TEST_ASSERT_TRUE(f.bd.readSector(3, b));  // новий кеш порожній: читання з чіпа, дані правильні
  TEST_ASSERT_TRUE(patOk(b, 103));
  f.bd.cacheClose();
  TEST_ASSERT_EQUAL(base, g_live);  // старий кеш звільнено
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_cache_open_close_frees);
  RUN_TEST(test_cache_lru_evicts_oldest);
  RUN_TEST(test_cache_invalidate);
  RUN_TEST(test_cache_counters_unique_repeat_seq);
  RUN_TEST(test_cache_plan_and_victim_run);
  RUN_TEST(test_bd_cache_hits_avoid_chip);
  RUN_TEST(test_bd_readahead_sequential);
  RUN_TEST(test_bd_readahead_clamped_at_partition_end);
  RUN_TEST(test_bd_random_reads_single_sector);
  RUN_TEST(test_bd_multi_sector_bypasses_cache);
  RUN_TEST(test_bd_remount_and_gone_invalidate);
  RUN_TEST(test_bd_cache_freed_after_scan_and_dtor);
  RUN_TEST(test_bd_shadow_mode_counts_only);
  RUN_TEST(test_bd_resize_keeps_stats_and_grows);
  return UNITY_END();
}

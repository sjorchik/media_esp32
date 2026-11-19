#include <unity.h>

#include "util/heap_phases.h"

void setUp() { HeapPhase::reset(); }
void tearDown() {}

void test_min_per_phase() {
  HeapPhase::record(HeapPhase::ScanPeak, 15000, 9000);
  HeapPhase::record(HeapPhase::ScanPeak, 12000, 9500);
  HeapPhase::record(HeapPhase::ScanPeak, 13000, 8000);
  const HeapPhase::Rec& r = HeapPhase::get(HeapPhase::ScanPeak);
  TEST_ASSERT_EQUAL_UINT32(3, r.n);
  TEST_ASSERT_EQUAL_UINT32(12000, r.minFree);
  TEST_ASSERT_EQUAL_UINT32(8000, r.minLargest);
  TEST_ASSERT_EQUAL_UINT32(0, HeapPhase::get(HeapPhase::Play).n);
}

void test_reset_clears() {
  HeapPhase::record(HeapPhase::Deact, 1, 1);
  HeapPhase::reset();
  TEST_ASSERT_EQUAL_UINT32(0, HeapPhase::get(HeapPhase::Deact).n);
  TEST_ASSERT_EQUAL_UINT32(0xFFFFFFFFu, HeapPhase::get(HeapPhase::Deact).minFree);
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_min_per_phase);
  RUN_TEST(test_reset_clears);
  return UNITY_END();
}

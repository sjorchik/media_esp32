#include <string.h>
#include <unity.h>

#include "audio/level.h"
#include "config.h"

// M4c1: на 0 дБ без рампи обробка повністю обходиться (бітова ідентичність, жодного множення).
void setUp() {
  for (int i = 0; i < level::kSlots; i++) level::setDb((level::Slot)i, 0);
  level::resetCounters();
}
void tearDown() {}

static void fillPat(int16_t* b, int n) {
  for (int i = 0; i < n; i++) b[i] = (int16_t)(i * 37 - 20000);
}

void test_unity_bypass_all_slots() {
  for (int s = 0; s < level::kSlots; s++) {
    int16_t a[2304], ref[2304];
    fillPat(a, 2304);
    memcpy(ref, a, sizeof a);
    level::resetCounters();
    for (int blk = 0; blk < 10; blk++) level::process(a, 1152, 2, (level::Slot)s);
    TEST_ASSERT_EQUAL_MEMORY(ref, a, sizeof a);
    const level::Counters c = level::counters();
    TEST_ASSERT_EQUAL_UINT32(0, c.mul);
    TEST_ASSERT_EQUAL_UINT32(0, c.mulFrames);
    TEST_ASSERT_EQUAL_UINT32(10, c.unity);
  }
}

void test_advance_returns_unity_gain() {
  const level::Gain g = level::advance(level::Slot::Sd, 1152);
  TEST_ASSERT_TRUE(g.unity);
  TEST_ASSERT_EQUAL_INT32(0, g.nRamp);
}

void test_minus20_multiplies_then_back_to_unity() {
  int16_t a[2304];
  level::setDb(level::Slot::Usb, -20);
  level::process(a, 1, 2, level::Slot::Usb);  // перший виклик слота: одразу на ціль
  level::resetCounters();
  fillPat(a, 2304);
  level::process(a, 1152, 2, level::Slot::Usb);
  TEST_ASSERT_EQUAL_UINT32(1, level::counters().mul);
  TEST_ASSERT_EQUAL_UINT32(1152, level::counters().mulFrames);
  level::setDb(level::Slot::Usb, 0);  // рампа ≈ 441 кадр, потім усталений 0 дБ
  fillPat(a, 2304);
  level::process(a, 1152, 2, level::Slot::Usb);
  level::resetCounters();
  int16_t ref[2304];
  fillPat(a, 2304);
  memcpy(ref, a, sizeof a);
  level::process(a, 1152, 2, level::Slot::Usb);
  TEST_ASSERT_EQUAL_MEMORY(ref, a, sizeof a);
  TEST_ASSERT_EQUAL_UINT32(0, level::counters().mul);
  TEST_ASSERT_EQUAL_UINT32(1, level::counters().unity);
}

void test_slots_independent() {
  level::setDb(level::Slot::Bt, -10);
  int16_t a[200], ref[200];
  fillPat(a, 200);
  memcpy(ref, a, sizeof a);
  level::process(a, 100, 2, level::Slot::Sd);  // SD на 0 дБ не чіпається, хоч BT приглушено
  TEST_ASSERT_EQUAL_MEMORY(ref, a, sizeof a);
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_unity_bypass_all_slots);
  RUN_TEST(test_advance_returns_unity_gain);
  RUN_TEST(test_minus20_multiplies_then_back_to_unity);
  RUN_TEST(test_slots_independent);
  return UNITY_END();
}

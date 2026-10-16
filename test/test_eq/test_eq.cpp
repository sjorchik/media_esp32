#include <math.h>
#include <stdint.h>
#include <string.h>
#include <unity.h>

#include "audio/eq.h"
#include "audio/level.h"
#include "config.h"

static const double kPi = 3.14159265358979323846;
static int16_t s_buf[2 * 48000];  // до 1 с стерео
static int16_t s_ref[2 * 48000];

void setUp(void) {
  const int8_t z[eq_cfg::kBandCount] = {0, 0, 0, 0, 0};
  eq::setAllDb(z);
  eq::setSampleRate(eq_cfg::kDefaultSampleRateHz);
  eq::reset();
  for (uint8_t s = 0; s < level::kSlots; s++) level::setDb((level::Slot)s, 0);
}
void tearDown(void) {}

// Синус амплітуди amp частоти f на fs, n кадрів, стерео (L=R)
static void makeSine(int16_t* b, int n, double f, double fs, double amp, uint8_t ch = 2) {
  for (int i = 0; i < n; i++) {
    int16_t v = (int16_t)lrint(amp * sin(2.0 * kPi * f * i / fs));
    if (ch == 2) {
      b[2 * i] = v;
      b[2 * i + 1] = v;
    } else {
      b[i] = v;
    }
  }
}

static double rmsCh(const int16_t* b, int from, int to, uint8_t ch, uint8_t c) {
  double s = 0;
  for (int i = from; i < to; i++) {
    double v = ch == 2 ? b[2 * i + c] : b[i];
    s += v * v;
  }
  return sqrt(s / (to - from));
}

// Пропускає синус через EQ блоками по blk кадрів; повертає підсилення в дБ за другою половиною сигналу.
static double measureDb(double f, double fs, double amp, int blk, uint8_t chk = 0) {
  const int n = (int)fs;  // 1 с; вимірюємо останні 0,5 с (ціле число періодів для всіх смуг)
  makeSine(s_buf, n, f, fs, amp);
  memcpy(s_ref, s_buf, sizeof(int16_t) * 2 * n);
  for (int i = 0; i < n; i += blk) eq::process(s_buf + 2 * i, (n - i < blk ? n - i : blk), 2);
  double a = rmsCh(s_buf, n / 2, n, 2, chk);
  double b = rmsCh(s_ref, n / 2, n, 2, chk);
  return 20.0 * log10(a / b);
}

static void bandResponse(double fs) {
  const int gains[] = {12, 6, -9, -12};
  for (uint8_t b = 0; b < eq_cfg::kBandCount; b++) {
    for (int g : gains) {
      int8_t p[eq_cfg::kBandCount] = {0, 0, 0, 0, 0};
      p[b] = (int8_t)g;
      eq::setAllDb(p);
      eq::reset();
      double dl = measureDb(eq_cfg::kBandFreqHz[b], fs, 4000.0, 100, 0);
      TEST_ASSERT_FLOAT_WITHIN(0.2f, (float)g, (float)dl);
      eq::reset();
      double dr = measureDb(eq_cfg::kBandFreqHz[b], fs, 4000.0, 100, 1);
      TEST_ASSERT_FLOAT_WITHIN(0.2f, (float)g, (float)dr);
    }
  }
}

static void test_band_response_44100() {
  eq::setSampleRate(44100);
  bandResponse(44100.0);
}

static void test_band_response_48000() {
  eq::setSampleRate(48000);
  bandResponse(48000.0);
}

static void test_all_zero_bit_identity() {
  uint32_t x = 2463534242u;
  for (int i = 0; i < 2 * 1152; i++) {
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    s_buf[i] = (int16_t)(x >> 8);
  }
  memcpy(s_ref, s_buf, sizeof(int16_t) * 2 * 1152);
  eq::process(s_buf, 1152, 2);
  TEST_ASSERT_EQUAL_INT(0, memcmp(s_buf, s_ref, sizeof(int16_t) * 2 * 1152));
  eq::process(s_buf, 2 * 1152, 1);  // моно-режим теж не чіпає
  TEST_ASSERT_EQUAL_INT(0, memcmp(s_buf, s_ref, sizeof(int16_t) * 2 * 1152));
  // із рівнем 0 дБ через level::process
  level::process(s_buf, 1152, 2, level::Slot::Sd);
  TEST_ASSERT_EQUAL_INT(0, memcmp(s_buf, s_ref, sizeof(int16_t) * 2 * 1152));
  TEST_ASSERT_TRUE(level::kUnityGain.unity);
}

static void test_saturation_no_wrap() {
  const int8_t p[eq_cfg::kBandCount] = {12, 12, 12, 12, 12};
  eq::setAllDb(p);
  const double fs = 44100.0;
  const int n = 44100;
  const double fr[] = {60.0, 1000.0, 12000.0};
  for (double f : fr) {
    eq::reset();
    makeSine(s_buf, n, f, fs, 32767.0);
    memcpy(s_ref, s_buf, sizeof(int16_t) * 2 * n);
    for (int i = 0; i < n; i += 256) eq::process(s_buf + 2 * i, (n - i < 256 ? n - i : 256), 2);
    int mx = -40000, mn = 40000, flips = 0;
    for (int i = n / 2; i < n; i++) {
      int o = s_buf[2 * i];
      int in = s_ref[2 * i];
      if (o > mx) mx = o;
      if (o < mn) mn = o;
      if (abs(in) > 20000 && ((in > 0) != (o > 0))) flips++;  // обертання дало б протилежний знак
    }
    TEST_ASSERT_EQUAL_INT(32767, mx);
    TEST_ASSERT_EQUAL_INT(-32768, mn);
    TEST_ASSERT_EQUAL_INT(0, flips);
  }
}

static void test_band12k_off_at_22050() {
  int8_t p[eq_cfg::kBandCount] = {0, 0, 0, 0, 12};
  eq::setAllDb(p);
  eq::setSampleRate(22050);
  TEST_ASSERT_FALSE(eq::bandActive(4));
  eq::setSampleRate(24000);  // 0,45·24000 = 10800 < 12000
  TEST_ASSERT_FALSE(eq::bandActive(4));
  eq::setSampleRate(44100);
  TEST_ASSERT_TRUE(eq::bandActive(4));
  eq::setSampleRate(22050);
  // смуга мовчить: блок бітово незмінний
  makeSine(s_buf, 2205, 3000.0, 22050.0, 10000.0);
  memcpy(s_ref, s_buf, sizeof(int16_t) * 2 * 2205);
  eq::process(s_buf, 2205, 2);
  TEST_ASSERT_EQUAL_INT(0, memcmp(s_buf, s_ref, sizeof(int16_t) * 2 * 2205));
  // нижчі смуги на 22,05 кГц працюють (4 кГц ≤ 0,45·22050)
  p[3] = 6;
  p[4] = 0;
  eq::setAllDb(p);
  TEST_ASSERT_TRUE(eq::bandActive(3));
  eq::reset();
  double d = measureDb(4000.0, 22050.0, 4000.0, 128);
  TEST_ASSERT_FLOAT_WITHIN(0.2f, 6.0f, (float)d);
}

static void test_setget_clamp_and_rate() {
  eq::setBandDb(0, 50);
  eq::setBandDb(1, -50);
  eq::setBandDb(9, 5);  // неіснуюча смуга ігнорується
  TEST_ASSERT_EQUAL_INT(eq_cfg::kGainMaxDb, eq::getBandDb(0));
  TEST_ASSERT_EQUAL_INT(eq_cfg::kGainMinDb, eq::getBandDb(1));
  TEST_ASSERT_EQUAL_INT(0, eq::getBandDb(9));
  int8_t out[eq_cfg::kBandCount];
  eq::getAllDb(out);
  TEST_ASSERT_EQUAL_INT(12, out[0]);
  TEST_ASSERT_EQUAL_INT(-12, out[1]);
  eq::setSampleRate(48000);
  eq::setSampleRate(100);     // нижче мінімуму — ігнорується
  eq::setSampleRate(400000);  // вище максимуму — ігнорується
  TEST_ASSERT_EQUAL_UINT32(48000, eq::sampleRate());
}

static void test_mono_equals_left() {
  const int8_t p[eq_cfg::kBandCount] = {5, -4, 7, 3, -6};
  eq::setAllDb(p);
  const int n = 4410;
  makeSine(s_buf, n, 700.0, 44100.0, 9000.0, 2);
  static int16_t mono[4410];
  for (int i = 0; i < n; i++) mono[i] = s_buf[2 * i];
  eq::reset();
  for (int i = 0; i < n; i += 300) eq::process(s_buf + 2 * i, (n - i < 300 ? n - i : 300), 2);
  eq::reset();
  for (int i = 0; i < n; i += 300) eq::process(mono + i, (n - i < 300 ? n - i : 300), 1);
  for (int i = 0; i < n; i++) TEST_ASSERT_EQUAL_INT16(s_buf[2 * i], mono[i]);
}

// ---------------------------------------------------------------- level

static void test_level_db_to_linear() {
  TEST_ASSERT_FLOAT_WITHIN(1e-6f, 1.0f, level::dbToLinear(0));
  TEST_ASSERT_FLOAT_WITHIN(1e-4f, 0.501187f, level::dbToLinear(-6));
  TEST_ASSERT_FLOAT_WITHIN(1e-5f, 0.1f, level::dbToLinear(-20));
  TEST_ASSERT_FLOAT_WITHIN(1e-5f, 0.01f, level::dbToLinear(-40));
}

static void test_level_slots_and_clamp() {
  level::setDb(level::Slot::Bt, -10);
  level::setDb(level::Slot::Sd, -100);  // обрізається до -40
  level::setDb(level::Slot::Usb, 7);    // вище 0 обрізається до 0
  TEST_ASSERT_EQUAL_INT(-10, level::getDb(level::Slot::Bt));
  TEST_ASSERT_EQUAL_INT(kLevelMinDb, level::getDb(level::Slot::Sd));
  TEST_ASSERT_EQUAL_INT(kLevelMaxDb, level::getDb(level::Slot::Usb));
}

static void test_level_ramp_reaches_target() {
  level::Gain g = level::advance(level::Slot::Sd, 128);  // слот змінився: стрибок на ціль (0 дБ)
  TEST_ASSERT_TRUE(g.unity);
  level::setDb(level::Slot::Sd, -20);
  const float tgt = level::dbToLinear(-20);
  int total = 0;
  bool done = false;
  float prevEnd = 1.0f;
  for (int i = 0; i < 20 && !done; i++) {
    g = level::advance(level::Slot::Sd, 128);
    TEST_ASSERT_FALSE(g.unity);
    TEST_ASSERT_TRUE(g.gEnd <= prevEnd);  // монотонно вниз
    prevEnd = g.gEnd;
    total += (g.nRamp < 128 ? g.nRamp : 128);
    if (g.gEnd == tgt && g.nRamp < 128) done = true;
  }
  TEST_ASSERT_TRUE(done);
  // повний хід 0,9 коефіцієнта при кроці 1/441 ≈ 397 кадрів (≈9 мс)
  TEST_ASSERT_TRUE(total >= 390 && total <= 400);
  g = level::advance(level::Slot::Sd, 128);  // усталено
  TEST_ASSERT_EQUAL_INT(0, g.nRamp);
  TEST_ASSERT_FALSE(g.unity);
  TEST_ASSERT_FLOAT_WITHIN(1e-7f, tgt, g.gEnd);
}

static void test_level_apply_ramp_monotonic() {
  level::process(s_buf, 16, 2, level::Slot::Bt);  // новий слот: ціль 0 дБ
  level::setDb(level::Slot::Bt, -20);
  const int n = 1000;
  for (int i = 0; i < 2 * n; i++) s_buf[i] = 10000;
  for (int i = 0; i < n; i += 128) level::process(s_buf + 2 * i, (n - i < 128 ? n - i : 128), 2, level::Slot::Bt);
  for (int i = 1; i < n; i++) {
    TEST_ASSERT_TRUE(s_buf[2 * i] <= s_buf[2 * (i - 1)]);  // плавне спадання без стрибків угору
    TEST_ASSERT_EQUAL_INT16(s_buf[2 * i], s_buf[2 * i + 1]);
  }
  TEST_ASSERT_EQUAL_INT16(1000, s_buf[2 * (n - 1)]);  // 10000 · 0,1
  TEST_ASSERT_TRUE(s_buf[0] > 9900);                  // першим кадром крок лише 1/441
  // розмір кроку: найбільша різниця сусідніх кадрів ≈ 10000/441·0,9 ≈ 20
  int maxStep = 0;
  for (int i = 1; i < n; i++) {
    int d = s_buf[2 * (i - 1)] - s_buf[2 * i];
    if (d > maxStep) maxStep = d;
  }
  TEST_ASSERT_TRUE(maxStep <= 25);
}

static void test_level_unity_identity() {
  for (int i = 0; i < 2 * 500; i++) s_buf[i] = (int16_t)(i * 37 - 9000);
  memcpy(s_ref, s_buf, sizeof(int16_t) * 2 * 500);
  level::process(s_buf, 500, 2, level::Slot::Usb);
  level::process(s_buf, 500, 2, level::Slot::Usb);
  TEST_ASSERT_EQUAL_INT(0, memcmp(s_buf, s_ref, sizeof(int16_t) * 2 * 500));
}

static void test_level_slot_switch_snaps() {
  level::setDb(level::Slot::Bt, -30);
  level::setDb(level::Slot::Sd, 0);
  level::advance(level::Slot::Bt, 128);                  // BT: стрибок на −30 дБ
  level::Gain g = level::advance(level::Slot::Sd, 128);  // інше джерело: одразу 0 дБ, без рампи
  TEST_ASSERT_TRUE(g.unity);
  TEST_ASSERT_EQUAL_INT(1, level::lastSlot());
}

// EQ +12 дБ і рівень −12 дБ: насичення лише в кінці. Якби EQ насичував до рівня,
// піку 20000·3,98 = 79600 → 32767 ·0,251 ≈ 8200, а не ≈ 20000.
static void test_single_saturation_at_end() {
  const int8_t p[eq_cfg::kBandCount] = {0, 0, 12, 0, 0};
  eq::setAllDb(p);
  level::setDb(level::Slot::Sd, -12);
  const double fs = 44100.0;
  const int n = 44100;
  makeSine(s_buf, n, 1000.0, fs, 20000.0);
  eq::reset();
  for (int i = 0; i < n; i += 256) {
    int m = n - i < 256 ? n - i : 256;
    eq::process(s_buf + 2 * i, m, 2, level::advance(level::Slot::Sd, m));
  }
  int mx = 0;
  for (int i = n / 2; i < n; i++)
    if (abs(s_buf[2 * i]) > mx) mx = abs(s_buf[2 * i]);
  const double expect = 20000.0 * pow(10.0, 12.0 / 20.0) * pow(10.0, -12.0 / 20.0);  // = 20000
  TEST_ASSERT_FLOAT_WITHIN((float)(expect * 0.02), (float)expect, (float)mx);
}

// Лише рівень (смуг нема): блок масштабується на коефіцієнт, насичення не потрібне.
static void test_level_only_through_eq_path() {
  level::setDb(level::Slot::Bt, -6);
  level::advance(level::Slot::Bt, 1);
  level::advance(level::Slot::Bt, 1000);  // дорампувати
  for (int i = 0; i < 2 * 200; i++) s_buf[i] = 20000;
  eq::process(s_buf, 200, 2, level::advance(level::Slot::Bt, 200));
  TEST_ASSERT_FLOAT_WITHIN(1.0f, 20000.0f * 0.501187f, (float)s_buf[0]);
  TEST_ASSERT_FLOAT_WITHIN(1.0f, 20000.0f * 0.501187f, (float)s_buf[2 * 199 + 1]);
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_band_response_44100);
  RUN_TEST(test_band_response_48000);
  RUN_TEST(test_all_zero_bit_identity);
  RUN_TEST(test_saturation_no_wrap);
  RUN_TEST(test_band12k_off_at_22050);
  RUN_TEST(test_setget_clamp_and_rate);
  RUN_TEST(test_mono_equals_left);
  RUN_TEST(test_level_db_to_linear);
  RUN_TEST(test_level_slots_and_clamp);
  RUN_TEST(test_level_ramp_reaches_target);
  RUN_TEST(test_level_apply_ramp_monotonic);
  RUN_TEST(test_level_unity_identity);
  RUN_TEST(test_level_slot_switch_snaps);
  RUN_TEST(test_single_saturation_at_end);
  RUN_TEST(test_level_only_through_eq_path);
  return UNITY_END();
}

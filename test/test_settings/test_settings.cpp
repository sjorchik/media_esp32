// Unit-тести M5b2 на ПК: settings_record і boot_guard (чисті модулі)
#include <string.h>
#include <unity.h>

#include "storage/boot_guard.h"
#include "storage/resume_record.h"
#include "storage/settings_record.h"

using BootLogic::Decision;
using BootLogic::Reason;

void setUp() {}
void tearDown() {}

// Збирає блоб із довільними байтами полів і ВІРНИМ CRC (для перевірки діапазонів/версій)
static void makeBlob(uint8_t* b, uint8_t ver, uint8_t src, uint8_t shuf, uint8_t rep) {
  b[0] = 'S'; b[1] = 'T'; b[2] = ver; b[3] = src; b[4] = shuf; b[5] = rep;
  uint32_t c = Resume::crc32(b, 6);
  b[6] = (uint8_t)c; b[7] = (uint8_t)(c >> 8); b[8] = (uint8_t)(c >> 16); b[9] = (uint8_t)(c >> 24);
}

void test_roundtrip_all_values() {
  for (uint8_t s = 0; s <= Settings::kSrcMax; s++)
    for (uint8_t sh = 0; sh <= Settings::kShufMax; sh++)
      for (uint8_t r = 0; r <= Settings::kRepMax; r++) {
        uint8_t b[Settings::kBlobSize];
        TEST_ASSERT_EQUAL(Settings::kBlobSize, Settings::serialize(s, sh, r, b, sizeof b));
        Settings::Record rec = {};
        TEST_ASSERT_EQUAL((int)Settings::ParseErr::Ok, (int)Settings::parse(b, sizeof b, rec));
        TEST_ASSERT_EQUAL(Settings::kVersion, rec.version);
        TEST_ASSERT_EQUAL(s, rec.lastSrc);
        TEST_ASSERT_EQUAL(sh, rec.shuf);
        TEST_ASSERT_EQUAL(r, rec.rep);
      }
}

void test_serialize_rejects_bad() {
  uint8_t b[Settings::kBlobSize + 4];
  TEST_ASSERT_EQUAL(0, Settings::serialize(4, 0, 0, b, sizeof b));
  TEST_ASSERT_EQUAL(0, Settings::serialize(0, 2, 0, b, sizeof b));
  TEST_ASSERT_EQUAL(0, Settings::serialize(0, 0, 3, b, sizeof b));
  TEST_ASSERT_EQUAL(0, Settings::serialize(1, 1, 1, b, Settings::kBlobSize - 1));
  TEST_ASSERT_EQUAL(0, Settings::serialize(1, 1, 1, nullptr, 16));
}

void test_blob_layout_and_crc() {
  uint8_t b[Settings::kBlobSize], e[Settings::kBlobSize];
  TEST_ASSERT_EQUAL(Settings::kBlobSize, Settings::serialize(2, 1, 2, b, sizeof b));
  TEST_ASSERT_EQUAL('S', b[0]);
  TEST_ASSERT_EQUAL('T', b[1]);
  TEST_ASSERT_EQUAL(1, b[2]);
  makeBlob(e, 1, 2, 1, 2);
  TEST_ASSERT_EQUAL_UINT8_ARRAY(e, b, sizeof b);
}

void test_out_of_range_with_valid_crc() {
  uint8_t b[Settings::kBlobSize];
  Settings::Record r = {};
  makeBlob(b, 1, 4, 0, 0);
  TEST_ASSERT_EQUAL((int)Settings::ParseErr::Range, (int)Settings::parse(b, sizeof b, r));
  makeBlob(b, 1, 0, 2, 0);
  TEST_ASSERT_EQUAL((int)Settings::ParseErr::Range, (int)Settings::parse(b, sizeof b, r));
  makeBlob(b, 1, 0, 0, 3);
  TEST_ASSERT_EQUAL((int)Settings::ParseErr::Range, (int)Settings::parse(b, sizeof b, r));
  makeBlob(b, 1, 255, 255, 255);
  TEST_ASSERT_EQUAL((int)Settings::ParseErr::Range, (int)Settings::parse(b, sizeof b, r));
}

void test_bad_crc_truncated_foreign() {
  uint8_t b[Settings::kBlobSize + 1];
  Settings::Record r = {};
  TEST_ASSERT_EQUAL(Settings::kBlobSize, Settings::serialize(1, 1, 2, b, sizeof b));
  // поганий CRC: будь-який зіпсований байт
  for (size_t i = 3; i < Settings::kBlobSize; i++) {
    uint8_t c[Settings::kBlobSize];
    memcpy(c, b, sizeof c);
    c[i] ^= 0x01;
    Settings::ParseErr e = Settings::parse(c, sizeof c, r);
    TEST_ASSERT_TRUE(e == Settings::ParseErr::Crc || e == Settings::ParseErr::Range);
    TEST_ASSERT_NOT_EQUAL((int)Settings::ParseErr::Ok, (int)e);
  }
  // обрізаний
  for (size_t n = 0; n < Settings::kBlobSize; n++)
    TEST_ASSERT_EQUAL((int)Settings::ParseErr::Short, (int)Settings::parse(b, n, r));
  // задовгий
  b[Settings::kBlobSize] = 0;
  TEST_ASSERT_EQUAL((int)Settings::ParseErr::Len, (int)Settings::parse(b, Settings::kBlobSize + 1, r));
  // чужий маркер (маркер резюме 'R','S')
  uint8_t f[Settings::kBlobSize];
  memcpy(f, b, sizeof f);
  f[0] = 'R';
  TEST_ASSERT_EQUAL((int)Settings::ParseErr::Magic, (int)Settings::parse(f, sizeof f, r));
  // невідома версія
  makeBlob(f, 2, 1, 1, 1);
  TEST_ASSERT_EQUAL((int)Settings::ParseErr::Version, (int)Settings::parse(f, sizeof f, r));
  makeBlob(f, 0, 1, 1, 1);
  TEST_ASSERT_EQUAL((int)Settings::ParseErr::Version, (int)Settings::parse(f, sizeof f, r));
  TEST_ASSERT_EQUAL((int)Settings::ParseErr::Short, (int)Settings::parse(nullptr, 0, r));
}

void test_guard_normal_resets() {
  Decision d = BootLogic::onBoot(Reason::Normal, 2, 3);
  TEST_ASSERT_EQUAL(0, d.count);
  TEST_ASSERT_FALSE(d.protect);
  TEST_ASSERT_TRUE(d.write);
  d = BootLogic::onBoot(Reason::Normal, 0, 3);  // нічого не змінилось → не писати
  TEST_ASSERT_EQUAL(0, d.count);
  TEST_ASSERT_FALSE(d.protect);
  TEST_ASSERT_FALSE(d.write);
  d = BootLogic::onBoot(Reason::Normal, 5, 3);  // навіть при старому лічильнику ≥ порогу норма знімає захист
  TEST_ASSERT_EQUAL(0, d.count);
  TEST_ASSERT_FALSE(d.protect);
  TEST_ASSERT_TRUE(d.write);
}

void test_guard_crash_increments_and_threshold() {
  Decision d = BootLogic::onBoot(Reason::Crash, 0, 3);
  TEST_ASSERT_EQUAL(1, d.count);
  TEST_ASSERT_FALSE(d.protect);
  TEST_ASSERT_TRUE(d.write);
  d = BootLogic::onBoot(Reason::Crash, d.count, 3);
  TEST_ASSERT_EQUAL(2, d.count);
  TEST_ASSERT_FALSE(d.protect);
  d = BootLogic::onBoot(Reason::Crash, d.count, 3);  // третя аварія підряд
  TEST_ASSERT_EQUAL(3, d.count);
  TEST_ASSERT_TRUE(d.protect);
  TEST_ASSERT_TRUE(d.write);
  d = BootLogic::onBoot(Reason::Crash, d.count, 3);  // далі лишається захист
  TEST_ASSERT_EQUAL(4, d.count);
  TEST_ASSERT_TRUE(d.protect);
}

void test_guard_saturation_and_zero_threshold() {
  Decision d = BootLogic::onBoot(Reason::Crash, 255, 3);
  TEST_ASSERT_EQUAL(255, d.count);
  TEST_ASSERT_TRUE(d.protect);
  TEST_ASSERT_FALSE(d.write);
  d = BootLogic::onBoot(Reason::Crash, 7, 0);  // поріг 0 — захист вимкнено
  TEST_ASSERT_FALSE(d.protect);
}

void test_guard_stable_resets() {
  Decision d = BootLogic::onStable(2);
  TEST_ASSERT_EQUAL(0, d.count);
  TEST_ASSERT_TRUE(d.write);
  d = BootLogic::onStable(0);
  TEST_ASSERT_EQUAL(0, d.count);
  TEST_ASSERT_FALSE(d.write);
}

void test_guard_crash_normal_crash() {
  uint8_t cnt = 0;
  Decision d = BootLogic::onBoot(Reason::Crash, cnt, 3);
  cnt = d.count;
  TEST_ASSERT_EQUAL(1, cnt);
  d = BootLogic::onBoot(Reason::Crash, cnt, 3);
  cnt = d.count;
  TEST_ASSERT_EQUAL(2, cnt);
  d = BootLogic::onBoot(Reason::Normal, cnt, 3);  // звичайний старт перериває серію
  cnt = d.count;
  TEST_ASSERT_EQUAL(0, cnt);
  d = BootLogic::onBoot(Reason::Crash, cnt, 3);
  cnt = d.count;
  TEST_ASSERT_EQUAL(1, cnt);
  TEST_ASSERT_FALSE(d.protect);
  // стабільна робота після аварії теж перериває серію
  d = BootLogic::onBoot(Reason::Crash, cnt, 3);
  cnt = d.count;
  d = BootLogic::onStable(cnt);
  cnt = d.count;
  TEST_ASSERT_EQUAL(0, cnt);
  d = BootLogic::onBoot(Reason::Crash, cnt, 3);
  TEST_ASSERT_EQUAL(1, d.count);
  TEST_ASSERT_FALSE(d.protect);
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_roundtrip_all_values);
  RUN_TEST(test_serialize_rejects_bad);
  RUN_TEST(test_blob_layout_and_crc);
  RUN_TEST(test_out_of_range_with_valid_crc);
  RUN_TEST(test_bad_crc_truncated_foreign);
  RUN_TEST(test_guard_normal_resets);
  RUN_TEST(test_guard_crash_increments_and_threshold);
  RUN_TEST(test_guard_saturation_and_zero_threshold);
  RUN_TEST(test_guard_stable_resets);
  RUN_TEST(test_guard_crash_normal_crash);
  return UNITY_END();
}

// Unit-тести резюме (M5b1): формат блоба storage/resume_record. Запуск: pio test -e native
#include <string.h>
#include <unity.h>

#include "storage/resume_record.h"

using namespace Resume;

void setUp() {}
void tearDown() {}

namespace {

// Збирає блоб вручну (для «підроблених» випадків: невідома версія, довжина 256, NUL у шляху тощо) з чинним CRC.
size_t craft(uint8_t* out, uint8_t ver, size_t len, const uint8_t* path, uint32_t off, uint32_t ms) {
  out[0] = 'R';
  out[1] = 'S';
  out[2] = ver;
  out[3] = (uint8_t)len;
  out[4] = (uint8_t)(len >> 8);
  if (len) memcpy(out + 5, path, len);
  for (int i = 0; i < 4; i++) out[5 + len + i] = (uint8_t)(off >> (8 * i));
  for (int i = 0; i < 4; i++) out[9 + len + i] = (uint8_t)(ms >> (8 * i));
  uint32_t c = crc32(out, 13 + len);
  for (int i = 0; i < 4; i++) out[13 + len + i] = (uint8_t)(c >> (8 * i));
  return 17 + len;
}

void recrc(uint8_t* b, size_t total) {
  uint32_t c = crc32(b, total - 4);
  for (int i = 0; i < 4; i++) b[total - 4 + i] = (uint8_t)(c >> (8 * i));
}

}  // namespace

void test_crc32_vector() {
  const char* s = "123456789";
  TEST_ASSERT_EQUAL_HEX32(0xCBF43926u, crc32((const uint8_t*)s, 9));
  TEST_ASSERT_EQUAL_HEX32(0u, crc32((const uint8_t*)s, 0));
}

void test_roundtrip() {
  uint8_t b[kBlobMax];
  const char* p = "/Rock/Band/track 01.mp3";
  size_t n = serialize(p, strlen(p), 123456u, 98765u, b, sizeof b);
  TEST_ASSERT_EQUAL_UINT32(kOverhead + strlen(p), n);
  Record r;
  memset(&r, 0xEE, sizeof r);
  TEST_ASSERT_EQUAL((int)ParseErr::Ok, (int)parse(b, n, r));
  TEST_ASSERT_EQUAL_UINT8(kVersion, r.version);
  TEST_ASSERT_EQUAL_STRING(p, r.path);
  TEST_ASSERT_EQUAL_UINT32(123456u, r.off);
  TEST_ASSERT_EQUAL_UINT32(98765u, r.ms);
}

void test_roundtrip_extremes_and_record_overload() {
  Record in{};
  in.version = kVersion;
  strcpy(in.path, "/a.mp3");
  in.off = 0xFFFFFFFFu;
  in.ms = 0u;
  uint8_t b[kBlobMax];
  size_t n = serialize(in, b, sizeof b);
  TEST_ASSERT_TRUE(n > 0);
  Record out;
  TEST_ASSERT_EQUAL((int)ParseErr::Ok, (int)parse(b, n, out));
  TEST_ASSERT_EQUAL_UINT32(0xFFFFFFFFu, out.off);
  TEST_ASSERT_EQUAL_UINT32(0u, out.ms);
  TEST_ASSERT_EQUAL_STRING("/a.mp3", out.path);
}

void test_cyrillic_path() {
  const char* p = "/Музика/Київ/Пісня №1 — Їжак.mp3";
  uint8_t b[kBlobMax];
  size_t n = serialize(p, strlen(p), 4242u, 31337u, b, sizeof b);
  TEST_ASSERT_TRUE(n > 0);
  Record r;
  TEST_ASSERT_EQUAL((int)ParseErr::Ok, (int)parse(b, n, r));
  TEST_ASSERT_EQUAL_STRING(p, r.path);
  TEST_ASSERT_EQUAL_UINT32(strlen(p), strlen(r.path));  // байти збережено точно
}

void test_path_255_and_256() {
  char p[300];
  memset(p, 'x', sizeof p);
  p[0] = '/';
  uint8_t b[kBlobMax + 8];
  size_t n = serialize(p, 255, 1u, 2u, b, sizeof b);
  TEST_ASSERT_EQUAL_UINT32(kBlobMax, n);
  Record r;
  TEST_ASSERT_EQUAL((int)ParseErr::Ok, (int)parse(b, n, r));
  TEST_ASSERT_EQUAL_UINT32(255, strlen(r.path));
  TEST_ASSERT_EQUAL_INT(0, memcmp(r.path, p, 255));
  TEST_ASSERT_EQUAL_UINT32(0, serialize(p, 256, 1u, 2u, b, sizeof b));  // 256 Б — не зберігається
}

void test_oversize_len_in_blob_rejected() {
  // «Чужий» блоб із довжиною 256 і чинним CRC: parse не має його приймати
  uint8_t path[256];
  memset(path, 'y', sizeof path);
  uint8_t b[kOverhead + 256 + 4];
  size_t n = craft(b, kVersion, 256, path, 1, 2);
  Record r;
  TEST_ASSERT_EQUAL((int)ParseErr::Path, (int)parse(b, n, r));
}

void test_bad_crc() {
  const char* p = "/A/B/c.mp3";
  uint8_t b[kBlobMax];
  size_t n = serialize(p, strlen(p), 10u, 20u, b, sizeof b);
  Record r;
  for (size_t i = 0; i < n; i++) {  // будь-який зіпсований байт блоба не проходить
    uint8_t save = b[i];
    b[i] ^= 0x01;
    TEST_ASSERT_TRUE(parse(b, n, r) != ParseErr::Ok);
    b[i] = save;
  }
  b[6] ^= 0x40;  // байт шляху
  TEST_ASSERT_EQUAL((int)ParseErr::Crc, (int)parse(b, n, r));
}

void test_truncated() {
  const char* p = "/Папка/файл.mp3";
  uint8_t b[kBlobMax];
  size_t n = serialize(p, strlen(p), 777u, 888u, b, sizeof b);
  Record r;
  for (size_t cut = 0; cut < n; cut++) TEST_ASSERT_TRUE(parse(b, cut, r) != ParseErr::Ok);
  b[n] = 0;  // зайвий байт у кінці
  TEST_ASSERT_EQUAL((int)ParseErr::Len, (int)parse(b, n + 1, r));
}

void test_unknown_version() {
  const char* p = "/v.mp3";
  uint8_t b[kBlobMax];
  size_t n = serialize(p, strlen(p), 5u, 6u, b, sizeof b);
  b[2] = kVersion + 1;
  Record r;
  TEST_ASSERT_EQUAL((int)ParseErr::Version, (int)parse(b, n, r));  // без перерахунку CRC
  recrc(b, n);
  TEST_ASSERT_EQUAL((int)ParseErr::Version, (int)parse(b, n, r));  // і з чинним CRC
  b[2] = 0;
  recrc(b, n);
  TEST_ASSERT_EQUAL((int)ParseErr::Version, (int)parse(b, n, r));
}

void test_bad_magic() {
  const char* p = "/m.mp3";
  uint8_t b[kBlobMax];
  size_t n = serialize(p, strlen(p), 1u, 2u, b, sizeof b);
  b[0] = 'X';
  Record r;
  TEST_ASSERT_EQUAL((int)ParseErr::Magic, (int)parse(b, n, r));
}

void test_empty_path() {
  uint8_t b[kBlobMax];
  TEST_ASSERT_EQUAL_UINT32(0, serialize("", 0, 1u, 2u, b, sizeof b));
  Record empty{};
  TEST_ASSERT_EQUAL_UINT32(0, serialize(empty, b, sizeof b));
  size_t n = craft(b, kVersion, 0, nullptr, 1, 2);  // блоб із довжиною 0 і чинним CRC
  Record r;
  TEST_ASSERT_EQUAL((int)ParseErr::Path, (int)parse(b, n, r));
}

void test_embedded_nul_rejected() {
  const uint8_t path[5] = {'/', 'a', 0, 'b', 'c'};
  uint8_t b[kBlobMax];
  size_t n = craft(b, kVersion, sizeof path, path, 1, 2);
  Record r;
  TEST_ASSERT_EQUAL((int)ParseErr::Path, (int)parse(b, n, r));
}

void test_small_cap() {
  const char* p = "/x.mp3";
  uint8_t b[kBlobMax];
  size_t need = serialize(p, strlen(p), 1u, 2u, b, sizeof b);
  TEST_ASSERT_EQUAL_UINT32(0, serialize(p, strlen(p), 1u, 2u, b, need - 1));
  TEST_ASSERT_EQUAL_UINT32(need, serialize(p, strlen(p), 1u, 2u, b, need));
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_crc32_vector);
  RUN_TEST(test_roundtrip);
  RUN_TEST(test_roundtrip_extremes_and_record_overload);
  RUN_TEST(test_cyrillic_path);
  RUN_TEST(test_path_255_and_256);
  RUN_TEST(test_oversize_len_in_blob_rejected);
  RUN_TEST(test_bad_crc);
  RUN_TEST(test_truncated);
  RUN_TEST(test_unknown_version);
  RUN_TEST(test_bad_magic);
  RUN_TEST(test_empty_path);
  RUN_TEST(test_embedded_nul_rejected);
  RUN_TEST(test_small_cap);
  return UNITY_END();
}

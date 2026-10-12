#include <stdio.h>
#include <string.h>
#include <unity.h>

#include "proto/frame.h"

using namespace proto;

struct Sink {
  int n = 0;
  char last[400] = {0};
};

static void cb(char* b, size_t len, void* c) {
  Sink* s = (Sink*)c;
  s->n++;
  memcpy(s->last, b, len);
  s->last[len] = 0;
}

static void feedStr(FrameParser& p, const char* s) { p.feed((const uint8_t*)s, strlen(s)); }

static void makeFrame(char* out, size_t bodyLen) {
  out[0] = '$';
  memset(out + 1, 'A', bodyLen);
  uint8_t x = xorSum(out + 1, bodyLen);
  sprintf(out + 1 + bodyLen, "*%02X\n", x);
}

void setUp(void) {}
void tearDown(void) {}

static void test_examples_parse() {
  const char* frames[] = {"$HB,1*17\n", "$PLAY*04\n", "$SRC,sd*79\n", "$STATE,sd,play,0,0*44\n"};
  const char* bodies[] = {"HB,1", "PLAY", "SRC,sd", "STATE,sd,play,0,0"};
  for (int i = 0; i < 4; i++) {
    Sink s;
    FrameParser p(cb, &s);
    feedStr(p, frames[i]);
    TEST_ASSERT_EQUAL_INT(1, s.n);
    TEST_ASSERT_EQUAL_STRING(bodies[i], s.last);
  }
}

static void test_examples_encode() {
  char buf[256];
  Arg hb[] = {Arg::N(1)};
  TEST_ASSERT_GREATER_THAN(0, (int)encodeFrame(buf, sizeof buf, "HB", hb, 1));
  TEST_ASSERT_EQUAL_STRING("$HB,1*17\n", buf);
  encodeFrame(buf, sizeof buf, "PLAY", nullptr, 0);
  TEST_ASSERT_EQUAL_STRING("$PLAY*04\n", buf);
  Arg sr[] = {Arg::S("sd")};
  encodeFrame(buf, sizeof buf, "SRC", sr, 1);
  TEST_ASSERT_EQUAL_STRING("$SRC,sd*79\n", buf);
  Arg st[] = {Arg::S("sd"), Arg::S("play"), Arg::N(0), Arg::N(0)};
  encodeFrame(buf, sizeof buf, "STATE", st, 4);
  TEST_ASSERT_EQUAL_STRING("$STATE,sd,play,0,0*44\n", buf);
}

static void test_escape_roundtrip() {
  const char* txt = "a%b,c*d$e\nf";
  char buf[256];
  Arg a[] = {Arg::T(txt)};
  size_t len = encodeFrame(buf, sizeof buf, "META", a, 1);
  TEST_ASSERT_GREATER_THAN(0, (int)len);
  TEST_ASSERT_NOT_NULL(strstr(buf, "a%25b%2Cc%2Ad%24e%0Af"));
  Sink s;
  FrameParser p(cb, &s);
  p.feed((const uint8_t*)buf, len);
  TEST_ASSERT_EQUAL_INT(1, s.n);
  Msg m;
  TEST_ASSERT_TRUE(splitBody(s.last, m));
  TEST_ASSERT_EQUAL_STRING("META", m.name);
  TEST_ASSERT_EQUAL_UINT8(1, m.argc);
  TEST_ASSERT_EQUAL_STRING(txt, m.args[0]);
}

static void test_cyrillic_passthrough() {
  char buf[256];
  Arg a[] = {Arg::T("Привіт")};
  encodeFrame(buf, sizeof buf, "META", a, 1);
  TEST_ASSERT_NOT_NULL(strstr(buf, "Привіт"));  // кирилиця без екранування
}

static void test_bad_xor() {
  Sink s;
  FrameParser p(cb, &s);
  feedStr(p, "$PLAY*05\n");
  TEST_ASSERT_EQUAL_INT(0, s.n);
  TEST_ASSERT_EQUAL_UINT32(1, p.bad());
  feedStr(p, "$PLAY\n");  // без '*XX'
  TEST_ASSERT_EQUAL_INT(0, s.n);
  feedStr(p, "$PLAY*04\n");  // парсер живий
  TEST_ASSERT_EQUAL_INT(1, s.n);
}

static void test_oversize() {
  char f[300];
  Sink s;
  FrameParser p(cb, &s);
  makeFrame(f, 250);  // рівно 255 байтів з '$' і LF
  TEST_ASSERT_EQUAL_size_t(255, strlen(f));
  feedStr(p, f);
  TEST_ASSERT_EQUAL_INT(1, s.n);
  makeFrame(f, 251);  // 256 — завеликий
  feedStr(p, f);
  TEST_ASSERT_EQUAL_INT(1, s.n);
  feedStr(p, "$PLAY*04\n");  // після завеликого парсер відновлюється
  TEST_ASSERT_EQUAL_INT(2, s.n);
}

static void test_dollar_resync() {
  Sink s;
  FrameParser p(cb, &s);
  feedStr(p, "$HB,$PLAY*04\n");
  TEST_ASSERT_EQUAL_INT(1, s.n);
  TEST_ASSERT_EQUAL_STRING("PLAY", s.last);
  TEST_ASSERT_EQUAL_UINT32(1, p.bad());
}

static void test_cr_and_garbage() {
  Sink s;
  FrameParser p(cb, &s);
  feedStr(p, "xyz\n$PLAY*04\r\n");
  TEST_ASSERT_EQUAL_INT(1, s.n);
  TEST_ASSERT_EQUAL_STRING("PLAY", s.last);
}

static void test_truncate_utf8() {
  char big[420];
  big[0] = 0;
  for (int i = 0; i < 200; i++) strcat(big, "Я");  // 400 байтів
  char buf[256];
  Arg a[] = {Arg::T(big), Arg::T("Title"), Arg::T("Album")};
  size_t len = encodeFrame(buf, sizeof buf, "META", a, 3);
  TEST_ASSERT_GREATER_THAN(0, (int)len);
  TEST_ASSERT_TRUE(len <= kMaxFrame);
  Sink s;
  FrameParser p(cb, &s);
  p.feed((const uint8_t*)buf, len);
  TEST_ASSERT_EQUAL_INT(1, s.n);
  Msg m;
  splitBody(s.last, m);
  TEST_ASSERT_EQUAL_UINT8(3, m.argc);
  TEST_ASSERT_EQUAL_STRING("Title", m.args[1]);
  TEST_ASSERT_EQUAL_STRING("Album", m.args[2]);
  size_t l0 = strlen(m.args[0]);
  TEST_ASSERT_TRUE(l0 > 0 && l0 % 2 == 0);  // лише цілі двобайтові символи
  for (size_t i = 0; i < l0; i += 2) {
    TEST_ASSERT_EQUAL_HEX8(0xD0, (uint8_t)m.args[0][i]);
    TEST_ASSERT_EQUAL_HEX8(0xAF, (uint8_t)m.args[0][i + 1]);
  }
}

static void test_split_empty_fields() {
  char b[] = "META,a,,c";
  Msg m;
  TEST_ASSERT_TRUE(splitBody(b, m));
  TEST_ASSERT_EQUAL_UINT8(3, m.argc);
  TEST_ASSERT_EQUAL_STRING("a", m.args[0]);
  TEST_ASSERT_EQUAL_STRING("", m.args[1]);
  TEST_ASSERT_EQUAL_STRING("c", m.args[2]);
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_examples_parse);
  RUN_TEST(test_examples_encode);
  RUN_TEST(test_escape_roundtrip);
  RUN_TEST(test_cyrillic_passthrough);
  RUN_TEST(test_bad_xor);
  RUN_TEST(test_oversize);
  RUN_TEST(test_dollar_resync);
  RUN_TEST(test_cr_and_garbage);
  RUN_TEST(test_truncate_utf8);
  RUN_TEST(test_split_empty_fields);
  return UNITY_END();
}
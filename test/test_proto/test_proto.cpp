#include <stdio.h>
#include <string.h>
#include <unity.h>

#include "audio/eq.h"
#include "audio/level.h"
#include "config.h"
#include "proto/cmd_args.h"
#include "proto/frame.h"
#include "proto/proto_table.h"

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

// ---------------- M5c4: додаткові тести відповідності контракту ----------------

static void test_escape_ctrl_chars() {
  const char* txt = "a\r\nb\tc";
  char buf[256];
  Arg a[] = {Arg::T(txt)};
  size_t len = encodeFrame(buf, sizeof buf, "META", a, 1);
  TEST_ASSERT_GREATER_THAN(0, (int)len);
  TEST_ASSERT_NOT_NULL(strstr(buf, "a%0D%0Ab%09c"));
  for (size_t i = 1; i + 1 < len; i++) TEST_ASSERT_TRUE((uint8_t)buf[i] >= 0x20);  // у кадрі лише фінальний LF
  Sink s;
  FrameParser p(cb, &s);
  p.feed((const uint8_t*)buf, len);
  TEST_ASSERT_EQUAL_INT(1, s.n);
  Msg m;
  TEST_ASSERT_TRUE(splitBody(s.last, m));
  TEST_ASSERT_EQUAL_STRING(txt, m.args[0]);
}

static void test_escape_no_raw_specials() {
  char buf[256];
  Arg a[] = {Arg::T("a,b*c$d%e")};
  size_t len = encodeFrame(buf, sizeof buf, "META", a, 1);
  int commas = 0, stars = 0, dollars = 0;
  for (size_t i = 0; i < len; i++) {
    if (buf[i] == ',') commas++;
    if (buf[i] == '*') stars++;
    if (buf[i] == '$') dollars++;
  }
  TEST_ASSERT_EQUAL_INT(1, commas);   // лише роздільник імені й поля
  TEST_ASSERT_EQUAL_INT(1, stars);    // лише перед сумою
  TEST_ASSERT_EQUAL_INT(1, dollars);  // лише початок кадру
  TEST_ASSERT_NOT_NULL(strstr(buf, "a%2Cb%2Ac%24d%25e"));
}

static void test_empty_fields_encode() {
  char buf[256];
  Arg a[] = {Arg::T(""), Arg::T(""), Arg::T("")};
  size_t len = encodeFrame(buf, sizeof buf, "META", a, 3);
  TEST_ASSERT_GREATER_THAN(0, (int)len);
  TEST_ASSERT_EQUAL_INT(0, strncmp(buf, "$META,,,*", 9));
  Sink s;
  FrameParser p(cb, &s);
  p.feed((const uint8_t*)buf, len);
  TEST_ASSERT_EQUAL_INT(1, s.n);
  Msg m;
  TEST_ASSERT_TRUE(splitBody(s.last, m));
  TEST_ASSERT_EQUAL_UINT8(3, m.argc);
  for (int i = 0; i < 3; i++) TEST_ASSERT_EQUAL_STRING("", m.args[i]);
}

static void test_hb_seq_wrap_text() {
  char buf[256];
  Arg a[] = {Arg::N(65535)};
  encodeFrame(buf, sizeof buf, "HB", a, 1);
  TEST_ASSERT_EQUAL_INT(0, strncmp(buf, "$HB,65535*", 10));
  Arg z[] = {Arg::N(0)};
  encodeFrame(buf, sizeof buf, "HB", z, 1);
  TEST_ASSERT_EQUAL_INT(0, strncmp(buf, "$HB,0*", 6));
}

// Обрізання не розрізає escape-послідовність: поле з одних ',' (кожна → %2C, 3 Б).
static void test_truncate_escape_boundary() {
  char big[301];
  memset(big, ',', 300);
  big[300] = 0;
  char buf[256];
  Arg a[] = {Arg::T(big)};
  size_t len = encodeFrame(buf, sizeof buf, "META", a, 1);
  TEST_ASSERT_TRUE(len > 0 && len <= kMaxFrame);
  Sink s;
  FrameParser p(cb, &s);
  p.feed((const uint8_t*)buf, len);
  TEST_ASSERT_EQUAL_INT(1, s.n);
  Msg m;
  splitBody(s.last, m);
  TEST_ASSERT_EQUAL_UINT8(1, m.argc);
  size_t fl = strlen(m.args[0]);
  TEST_ASSERT_EQUAL_size_t(81, fl);  // бюджет поля 245 Б → 81 × 3 = 243
  for (size_t i = 0; i < fl; i++) TEST_ASSERT_EQUAL_INT(',', m.args[0][i]);  // жодного обрубка «%2»
}

// Довге ASCII-поле використовує кадр до межі: рівно 255 Б.
static void test_truncate_ascii_fills_frame() {
  char big[401];
  memset(big, 'a', 400);
  big[400] = 0;
  char buf[256];
  Arg a[] = {Arg::T(big)};
  size_t len = encodeFrame(buf, sizeof buf, "META", a, 1);
  TEST_ASSERT_EQUAL_size_t(255, len);
  Sink s;
  FrameParser p(cb, &s);
  p.feed((const uint8_t*)buf, len);
  TEST_ASSERT_EQUAL_INT(1, s.n);
  Msg m;
  splitBody(s.last, m);
  TEST_ASSERT_EQUAL_size_t(245, strlen(m.args[0]));
}

// 3- і 4-байтові символи ріжуться лише цілими.
static void test_truncate_multibyte() {
  char big[700];
  big[0] = 0;
  for (int i = 0; i < 100; i++) strcat(big, "\xF0\x9F\x98\x80");  // U+1F600, 4 Б
  char buf[256];
  Arg a[] = {Arg::T(big)};
  size_t len = encodeFrame(buf, sizeof buf, "META", a, 1);
  TEST_ASSERT_TRUE(len > 0 && len <= kMaxFrame);
  Sink s;
  FrameParser p(cb, &s);
  p.feed((const uint8_t*)buf, len);
  Msg m;
  splitBody(s.last, m);
  size_t fl = strlen(m.args[0]);
  TEST_ASSERT_TRUE(fl > 0 && fl % 4 == 0);
  for (size_t i = 0; i < fl; i += 4) {
    TEST_ASSERT_EQUAL_HEX8(0xF0, (uint8_t)m.args[0][i]);
    TEST_ASSERT_EQUAL_HEX8(0x9F, (uint8_t)m.args[0][i + 1]);
    TEST_ASSERT_EQUAL_HEX8(0x98, (uint8_t)m.args[0][i + 2]);
    TEST_ASSERT_EQUAL_HEX8(0x80, (uint8_t)m.args[0][i + 3]);
  }
  big[0] = 0;
  for (int i = 0; i < 200; i++) strcat(big, "\xE2\x82\xAC");  // €, 3 Б
  Arg b[] = {Arg::T(big)};
  len = encodeFrame(buf, sizeof buf, "META", b, 1);
  TEST_ASSERT_TRUE(len > 0 && len <= kMaxFrame);
  Sink s2;
  FrameParser p2(cb, &s2);
  p2.feed((const uint8_t*)buf, len);
  splitBody(s2.last, m);
  fl = strlen(m.args[0]);
  TEST_ASSERT_TRUE(fl > 0 && fl % 3 == 0);
}

// Трьох довгих полів із екрануванням разом: кадр ≤ 255, кожне поле цілісне.
static void test_truncate_three_fields_escaped() {
  char a1[301], a2[301], a3[301];
  memset(a1, ',', 300);
  a1[300] = 0;
  memset(a2, '%', 300);
  a2[300] = 0;
  a3[0] = 0;
  for (int i = 0; i < 100; i++) strcat(a3, "Ї");
  char buf[256];
  Arg a[] = {Arg::T(a1), Arg::T(a2), Arg::T(a3)};
  size_t len = encodeFrame(buf, sizeof buf, "META", a, 3);
  TEST_ASSERT_TRUE(len > 0 && len <= kMaxFrame);
  Sink s;
  FrameParser p(cb, &s);
  p.feed((const uint8_t*)buf, len);
  TEST_ASSERT_EQUAL_INT(1, s.n);
  Msg m;
  splitBody(s.last, m);
  TEST_ASSERT_EQUAL_UINT8(3, m.argc);
  for (size_t i = 0; i < strlen(m.args[0]); i++) TEST_ASSERT_EQUAL_INT(',', m.args[0][i]);
  for (size_t i = 0; i < strlen(m.args[1]); i++) TEST_ASSERT_EQUAL_INT('%', m.args[1][i]);
  size_t l3 = strlen(m.args[2]);
  TEST_ASSERT_TRUE(l3 > 0 && l3 % 2 == 0);
}

static void test_dollar_resync_variants() {
  {
    Sink s;
    FrameParser p(cb, &s);
    feedStr(p, "$SRC,s$PLAY*04\n");  // обірваний кадр + новий
    TEST_ASSERT_EQUAL_INT(1, s.n);
    TEST_ASSERT_EQUAL_STRING("PLAY", s.last);
  }
  {
    Sink s;
    FrameParser p(cb, &s);
    feedStr(p, "$$$PLAY*04\n");  // кілька '$' підряд
    TEST_ASSERT_EQUAL_INT(1, s.n);
    TEST_ASSERT_EQUAL_STRING("PLAY", s.last);
  }
  {
    Sink s;
    FrameParser p(cb, &s);
    feedStr(p, "$PLAY*04$HB,1*17\n");  // перший без LF втрачено, другий прийнято
    TEST_ASSERT_EQUAL_INT(1, s.n);
    TEST_ASSERT_EQUAL_STRING("HB,1", s.last);
  }
  {
    Sink s;
    FrameParser p(cb, &s);
    feedStr(p, "$PLAY*04\nxx$PLAY*04\n");  // сміття між валідними кадрами
    TEST_ASSERT_EQUAL_INT(2, s.n);
  }
}

static void test_no_dollar_ignored() {
  Sink s;
  FrameParser p(cb, &s);
  feedStr(p, "PLAY*04\n");
  feedStr(p, "GET\n");
  TEST_ASSERT_EQUAL_INT(0, s.n);
  TEST_ASSERT_EQUAL_UINT32(0, p.bad());  // без '$' — не кадр, навіть не помилка
  feedStr(p, "$PLAY*04\n");
  TEST_ASSERT_EQUAL_INT(1, s.n);
}

static void test_cr_ignored_inside() {
  Sink s;
  FrameParser p(cb, &s);
  feedStr(p, "$PLAY\r*04\r\n");  // CR ігнорується будь-де
  TEST_ASSERT_EQUAL_INT(1, s.n);
  TEST_ASSERT_EQUAL_STRING("PLAY", s.last);
}

static void test_oversize_far_and_recover() {
  Sink s;
  FrameParser p(cb, &s);
  p.feed((const uint8_t*)"$", 1);
  for (int i = 0; i < 700; i++) p.feed((uint8_t)'A');  // значно більше ліміту
  feedStr(p, "*00\n");
  TEST_ASSERT_EQUAL_INT(0, s.n);
  feedStr(p, "$GET*XX\n");  // погана сума — теж мовчки
  TEST_ASSERT_EQUAL_INT(0, s.n);
  feedStr(p, "$PLAY*04\n");
  TEST_ASSERT_EQUAL_INT(1, s.n);
}

static void test_err_frame_encode() {
  char buf[256];
  Arg a[] = {Arg::S("unknown_cmd"), Arg::T("A,B")};
  size_t len = encodeFrame(buf, sizeof buf, "ERR", a, 2);
  TEST_ASSERT_GREATER_THAN(0, (int)len);
  TEST_ASSERT_NOT_NULL(strstr(buf, "$ERR,unknown_cmd,A%2CB*"));
  Sink s;
  FrameParser p(cb, &s);
  p.feed((const uint8_t*)buf, len);
  TEST_ASSERT_EQUAL_INT(1, s.n);
  Arg b[] = {Arg::S("bad_arg")};
  len = encodeFrame(buf, sizeof buf, "ERR", b, 1);
  TEST_ASSERT_EQUAL_INT(0, strncmp(buf, "$ERR,bad_arg*", 13));  // без деталі — без зайвої коми
}

// Таблиця команд відповідає розд. 4 контракту (імена, кількість аргументів).
static const proto::CmdDef* findCmd(const char* n) {
  for (size_t i = 0; i < kCmdCount; i++)
    if (!strcmp(kCmds[i].name, n)) return &kCmds[i];
  return nullptr;
}

static void test_cmd_table_matches_contract() {
  struct E { const char* n; int mn, mx; };
  const E want[] = {{"HELLO", 1, 1}, {"GET", 0, 0},   {"SRC", 1, 1},   {"PLAY", 0, 0}, {"PAUSE", 0, 0},
                    {"TOGGLE", 0, 0}, {"STOP", 0, 0}, {"NEXT", 0, 0},  {"PREV", 0, 0}, {"FNEXT", 0, 0},
                    {"FPREV", 0, 0},  {"SHUF", 1, 1}, {"REP", 1, 1},   {"HB", 1, 1},
                    {"EQ", 5, 5},    {"LVL", 2, 2}};
  for (const E& e : want) {
    const CmdDef* d = findCmd(e.n);
    TEST_ASSERT_NOT_NULL(d);
    TEST_ASSERT_EQUAL_INT(e.mn, d->minArgs);
    TEST_ASSERT_EQUAL_INT(e.mx, d->maxArgs);
  }
  // зарезервовані: є в таблиці (роутер відповідає not_implemented), інших команд зверх контракту нема
  TEST_ASSERT_NOT_NULL(findCmd("LIST"));
  TEST_ASSERT_NOT_NULL(findCmd("PLAYFILE"));
  TEST_ASSERT_EQUAL_size_t(sizeof(want) / sizeof(want[0]) + 2, kCmdCount);
  TEST_ASSERT_NULL(findCmd("play"));  // регістр значущий → unknown_cmd
  TEST_ASSERT_NULL(findCmd("DBG"));   // DBG — лише UART0, не частина контракту
}

// Модель перевірки арності з роутера: ці випадки мають давати bad_arg.
static bool arityOk(const char* body) {
  char tmp[64];
  strncpy(tmp, body, sizeof tmp - 1);
  tmp[sizeof tmp - 1] = 0;
  Msg m;
  if (!splitBody(tmp, m)) return false;
  const CmdDef* d = findCmd(m.name);
  if (!d) return false;
  return !m.tooMany && m.argc >= d->minArgs && m.argc <= d->maxArgs;
}

static void test_arity_model() {
  TEST_ASSERT_TRUE(arityOk("SRC,sd"));
  TEST_ASSERT_TRUE(arityOk("HELLO,1"));
  TEST_ASSERT_TRUE(arityOk("PLAY"));
  TEST_ASSERT_FALSE(arityOk("SRC"));       // немає аргументу
  TEST_ASSERT_FALSE(arityOk("SRC,sd,x"));  // зайвий аргумент
  TEST_ASSERT_FALSE(arityOk("PLAY,1"));
  TEST_ASSERT_FALSE(arityOk("PLAY,"));     // порожній аргумент теж аргумент
  TEST_ASSERT_FALSE(arityOk("SHUF"));
  TEST_ASSERT_FALSE(arityOk("REP"));
  TEST_ASSERT_FALSE(arityOk("FOO"));       // невідома команда
}

static void test_errc_codes_match_contract() {
  TEST_ASSERT_EQUAL_STRING("unknown_cmd", errc::kUnknownCmd);
  TEST_ASSERT_EQUAL_STRING("bad_arg", errc::kBadArg);
  TEST_ASSERT_EQUAL_STRING("bad_state", errc::kBadState);
  TEST_ASSERT_EQUAL_STRING("no_media", errc::kNoMedia);
  TEST_ASSERT_EQUAL_STRING("read_fail", errc::kReadFail);
  TEST_ASSERT_EQUAL_STRING("decode_fail", errc::kDecodeFail);
  TEST_ASSERT_EQUAL_STRING("too_many_files", errc::kTooManyFiles);
  TEST_ASSERT_EQUAL_STRING("not_implemented", errc::kNotImplemented);
  TEST_ASSERT_EQUAL_STRING("bt_fail", errc::kBtFail);
}

static void test_split_edge_cases() {
  char b1[] = "SHUF,1,2,3,4,5,6,7,8,9";  // 9 аргументів → tooMany
  Msg m;
  TEST_ASSERT_TRUE(splitBody(b1, m));
  TEST_ASSERT_EQUAL_UINT8(kMaxArgs, m.argc);
  TEST_ASSERT_TRUE(m.tooMany);
  char b2[] = ",x";  // порожнє імʼя
  TEST_ASSERT_FALSE(splitBody(b2, m));
  char b3[] = "ERR,bad%2Carg";
  TEST_ASSERT_TRUE(splitBody(b3, m));
  TEST_ASSERT_EQUAL_STRING("bad,arg", m.args[0]);
}


// ---------------- M6b: EQ, LVL, proto_ver = 2 ----------------

// Модель роутера до виклику apply*: пошук у таблиці, перевірка арності, далі applyEq/applyLvl.
// true — команда прийнята й застосована; false — роутер відповів би ERR,bad_arg,<NAME> (або unknown_cmd).
static bool route(const char* body) {
  char tmp[160];
  strncpy(tmp, body, sizeof tmp - 1);
  tmp[sizeof tmp - 1] = 0;
  Msg m;
  if (!splitBody(tmp, m)) return false;
  const CmdDef* d = findCmd(m.name);
  if (!d) return false;
  if (m.tooMany || m.argc < d->minArgs || m.argc > d->maxArgs) return false;
  if (d->id == CmdId::Eq) return applyEq(m);
  if (d->id == CmdId::Lvl) return applyLvl(m);
  return false;
}

static void audioReset() {
  const int8_t z[eq_cfg::kBandCount] = {0, 0, 0, 0, 0};
  eq::setAllDb(z);
  for (uint8_t i = 0; i < level::kSlots; i++) level::setDb((level::Slot)i, 0);
}

static void expectEq(int a, int b, int c, int d, int e) {
  int8_t g[eq_cfg::kBandCount];
  eq::getAllDb(g);
  TEST_ASSERT_EQUAL_INT(a, g[0]);
  TEST_ASSERT_EQUAL_INT(b, g[1]);
  TEST_ASSERT_EQUAL_INT(c, g[2]);
  TEST_ASSERT_EQUAL_INT(d, g[3]);
  TEST_ASSERT_EQUAL_INT(e, g[4]);
}

static void expectLvl(int bt, int sd, int usb) {
  TEST_ASSERT_EQUAL_INT(bt, level::getDb(level::Slot::Bt));
  TEST_ASSERT_EQUAL_INT(sd, level::getDb(level::Slot::Sd));
  TEST_ASSERT_EQUAL_INT(usb, level::getDb(level::Slot::Usb));
}

static void test_proto_ver_is_2() {
  TEST_ASSERT_EQUAL_INT(2, kProtoVer);
  char buf[256];
  Arg a[] = {Arg::T(FW_VERSION), Arg::N(kProtoVer)};  // як Events::ready()
  size_t len = encodeFrame(buf, sizeof buf, "READY", a, 2);
  TEST_ASSERT_GREATER_THAN(0, (int)len);
  Sink s;
  FrameParser p(cb, &s);
  p.feed((const uint8_t*)buf, len);
  TEST_ASSERT_EQUAL_INT(1, s.n);
  Msg m;
  TEST_ASSERT_TRUE(splitBody(s.last, m));
  TEST_ASSERT_EQUAL_STRING("READY", m.name);
  TEST_ASSERT_EQUAL_UINT8(2, m.argc);
  TEST_ASSERT_EQUAL_STRING("2", m.args[1]);
}

static void test_parse_ints() {
  long v = 99;
  TEST_ASSERT_TRUE(parseUInt("0", v));
  TEST_ASSERT_EQUAL_INT(0, (int)v);
  TEST_ASSERT_TRUE(parseUInt("100000", v));
  TEST_ASSERT_FALSE(parseUInt("100001", v));
  TEST_ASSERT_FALSE(parseUInt("", v));
  TEST_ASSERT_FALSE(parseUInt(nullptr, v));
  TEST_ASSERT_FALSE(parseUInt("-1", v));  // беззнакова версія знак не приймає
  TEST_ASSERT_TRUE(parseSInt("-12", v));
  TEST_ASSERT_EQUAL_INT(-12, (int)v);
  TEST_ASSERT_TRUE(parseSInt("12", v));
  TEST_ASSERT_EQUAL_INT(12, (int)v);
  TEST_ASSERT_TRUE(parseSInt("-100000", v));
  TEST_ASSERT_FALSE(parseSInt("-100001", v));
  const char* bad[] = {"", "-", "--1", "+1", " 1", "1 ", "1.5", "-1.5", "0x5", "1e1", "abc", "- 1", "-+1"};
  for (const char* b : bad) TEST_ASSERT_FALSE(parseSInt(b, v));
  TEST_ASSERT_FALSE(parseSInt(nullptr, v));
}

// HELLO: чужа версія (зокрема стара 1) → лише READY; наша 2 → READY + повний стан (його шле app).
static void test_hello_version_policy() {
  long v = 0;
  TEST_ASSERT_TRUE(helloVerdict("1", kProtoVer, v) == HelloVerdict::ReadyOnly);
  TEST_ASSERT_EQUAL_INT(1, (int)v);
  TEST_ASSERT_TRUE(helloVerdict("2", kProtoVer, v) == HelloVerdict::Full);
  TEST_ASSERT_EQUAL_INT(2, (int)v);
  TEST_ASSERT_TRUE(helloVerdict("3", kProtoVer, v) == HelloVerdict::ReadyOnly);
  TEST_ASSERT_TRUE(helloVerdict("0", kProtoVer, v) == HelloVerdict::ReadyOnly);
  TEST_ASSERT_TRUE(helloVerdict("abc", kProtoVer, v) == HelloVerdict::BadArg);
  TEST_ASSERT_TRUE(helloVerdict("", kProtoVer, v) == HelloVerdict::BadArg);
  TEST_ASSERT_TRUE(helloVerdict("-2", kProtoVer, v) == HelloVerdict::BadArg);
  TEST_ASSERT_TRUE(helloVerdict(nullptr, kProtoVer, v) == HelloVerdict::BadArg);
}

static void test_eq_valid() {
  audioReset();
  TEST_ASSERT_TRUE(route("EQ,3,0,0,0,3"));
  expectEq(3, 0, 0, 0, 3);
  TEST_ASSERT_TRUE(route("EQ,-1,-2,-3,-4,-5"));  // від'ємні числа
  expectEq(-1, -2, -3, -4, -5);
  TEST_ASSERT_TRUE(route("EQ,0,0,0,0,0"));
  expectEq(0, 0, 0, 0, 0);
  TEST_ASSERT_TRUE(route("EQ,12,-12,6,-6,0"));
  expectEq(12, -12, 6, -6, 0);
  expectLvl(0, 0, 0);  // EQ рівнів не чіпає
}

static void test_eq_bounds() {
  audioReset();
  TEST_ASSERT_TRUE(route("EQ,-12,-12,-12,-12,-12"));
  expectEq(-12, -12, -12, -12, -12);
  TEST_ASSERT_TRUE(route("EQ,12,12,12,12,12"));
  expectEq(12, 12, 12, 12, 12);
  TEST_ASSERT_FALSE(route("EQ,13,0,0,0,0"));
  TEST_ASSERT_FALSE(route("EQ,0,0,0,0,-13"));
  TEST_ASSERT_FALSE(route("EQ,-100,0,0,0,0"));
  TEST_ASSERT_FALSE(route("EQ,0,0,0,0,100000"));
  expectEq(12, 12, 12, 12, 12);  // жодна спроба поза діапазоном нічого не змінила
}

static void test_eq_arity() {
  audioReset();
  TEST_ASSERT_TRUE(route("EQ,1,2,3,4,5"));
  TEST_ASSERT_FALSE(route("EQ"));
  TEST_ASSERT_FALSE(route("EQ,1"));
  TEST_ASSERT_FALSE(route("EQ,9,9,9,9"));          // 4 аргументи
  TEST_ASSERT_FALSE(route("EQ,9,9,9,9,9,9"));      // 6 аргументів
  TEST_ASSERT_FALSE(route("EQ,9,9,9,9,9,9,9,9,9")); // 9 → tooMany
  expectEq(1, 2, 3, 4, 5);
}

static void test_eq_not_number() {
  audioReset();
  TEST_ASSERT_TRUE(route("EQ,1,2,3,4,5"));
  const char* bad[] = {"EQ,x,2,3,4,5",   "EQ,1,2,3,4,x",   "EQ,1,2,3,4,",     "EQ,,2,3,4,5",    "EQ,1,2,3,4, 5",
                       "EQ,1,2,3,4,+5",  "EQ,1,2,3,4,5.5", "EQ,1,2,3,4,--5",  "EQ,1,2,3,4,-",   "EQ,1,2,3,4,0x5",
                       "EQ,1,2,3,4,1e1", "EQ,1,2,3,4,5 ",  "EQ,1,2,3,4,abc"};
  for (const char* b : bad) {
    TEST_ASSERT_FALSE(route(b));
    expectEq(1, 2, 3, 4, 5);
  }
}

// Атомарність: помилка в будь-якому аргументі (зокрема в 5-му) не змінює ні першого, ні інших.
static void test_eq_atomic() {
  audioReset();
  TEST_ASSERT_TRUE(route("EQ,1,2,3,4,5"));
  TEST_ASSERT_FALSE(route("EQ,9,9,9,9,99"));  // 5-й поза діапазоном
  expectEq(1, 2, 3, 4, 5);
  TEST_ASSERT_FALSE(route("EQ,9,9,9,9,x"));    // 5-й не число
  expectEq(1, 2, 3, 4, 5);
  TEST_ASSERT_FALSE(route("EQ,9,9,9,99,9"));   // 4-й
  expectEq(1, 2, 3, 4, 5);
  TEST_ASSERT_FALSE(route("EQ,99,9,9,9,9"));   // 1-й
  expectEq(1, 2, 3, 4, 5);
  TEST_ASSERT_FALSE(route("EQ,9,9,9,9"));      // 4 аргументи
  expectEq(1, 2, 3, 4, 5);
  TEST_ASSERT_TRUE(route("EQ,5,4,3,2,1"));     // після помилок нормальна команда працює
  expectEq(5, 4, 3, 2, 1);
}

static void test_lvl_valid() {
  audioReset();
  TEST_ASSERT_TRUE(route("LVL,sd,-20"));
  expectLvl(0, -20, 0);
  TEST_ASSERT_TRUE(route("LVL,bt,-40"));
  expectLvl(-40, -20, 0);
  TEST_ASSERT_TRUE(route("LVL,usb,-1"));
  expectLvl(-40, -20, -1);
  TEST_ASSERT_TRUE(route("LVL,sd,0"));
  expectLvl(-40, 0, -1);
  expectEq(0, 0, 0, 0, 0);  // LVL EQ не чіпає
}

static void test_lvl_bounds() {
  audioReset();
  TEST_ASSERT_TRUE(route("LVL,sd,-40"));
  TEST_ASSERT_TRUE(route("LVL,bt,0"));
  expectLvl(0, -40, 0);
  TEST_ASSERT_FALSE(route("LVL,sd,-41"));
  TEST_ASSERT_FALSE(route("LVL,sd,1"));
  TEST_ASSERT_FALSE(route("LVL,sd,100"));
  TEST_ASSERT_FALSE(route("LVL,sd,-100000"));
  expectLvl(0, -40, 0);
}

static void test_lvl_arity() {
  audioReset();
  TEST_ASSERT_TRUE(route("LVL,sd,-5"));
  TEST_ASSERT_FALSE(route("LVL"));
  TEST_ASSERT_FALSE(route("LVL,sd"));            // 1 аргумент
  TEST_ASSERT_FALSE(route("LVL,sd,-20,1"));      // 3 аргументи
  TEST_ASSERT_FALSE(route("LVL,sd,-20,1,2,3,4,5,6"));  // tooMany
  expectLvl(0, -5, 0);
}

static void test_lvl_bad_slot() {
  audioReset();
  const char* bad[] = {"LVL,off,-10", "LVL,SD,-10", "LVL,Bt,-10", "LVL,,-10", "LVL,sdd,-10", "LVL,s,-10",
                       "LVL,usb ,-10", "LVL, sd,-10", "LVL,0,-10", "LVL,all,-10"};
  for (const char* b : bad) {
    TEST_ASSERT_FALSE(route(b));
    expectLvl(0, 0, 0);
  }
}

static void test_lvl_not_number() {
  audioReset();
  TEST_ASSERT_TRUE(route("LVL,sd,-7"));
  const char* bad[] = {"LVL,sd,x", "LVL,sd,", "LVL,sd,-", "LVL,sd,+0", "LVL,sd,-2.5", "LVL,sd, -20", "LVL,sd,-20 ",
                       "LVL,sd,--5", "LVL,sd,0x1", "LVL,sd,1e1"};
  for (const char* b : bad) {
    TEST_ASSERT_FALSE(route(b));
    expectLvl(0, -7, 0);
  }
}

// Повний шлях: кадр із XOR-сумою → парсер → тіло → route. Контрольні суми з прикладів контракту.
static void test_audio_cmd_frames() {
  audioReset();
  const char* frames[] = {"$EQ,3,0,0,0,3*08\n", "$LVL,sd,-20*6E\n"};
  const char* bodies[] = {"EQ,3,0,0,0,3", "LVL,sd,-20"};
  for (int i = 0; i < 2; i++) {
    Sink s;
    FrameParser p(cb, &s);
    feedStr(p, frames[i]);
    TEST_ASSERT_EQUAL_INT(1, s.n);
    TEST_ASSERT_EQUAL_STRING(bodies[i], s.last);
    TEST_ASSERT_TRUE(route(s.last));
  }
  expectEq(3, 0, 0, 0, 3);
  expectLvl(0, -20, 0);
  char buf[96];
  snprintf(buf, sizeof buf, "$%s*%02X\n", "EQ,1,2,3,4,x", xorSum("EQ,1,2,3,4,x", 12));  // кадр валідний, команда ні
  Sink s2;
  FrameParser p2(cb, &s2);
  feedStr(p2, buf);
  TEST_ASSERT_EQUAL_INT(1, s2.n);
  TEST_ASSERT_FALSE(route(s2.last));
  expectEq(3, 0, 0, 0, 3);
  audioReset();
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
  RUN_TEST(test_escape_ctrl_chars);
  RUN_TEST(test_escape_no_raw_specials);
  RUN_TEST(test_empty_fields_encode);
  RUN_TEST(test_hb_seq_wrap_text);
  RUN_TEST(test_truncate_escape_boundary);
  RUN_TEST(test_truncate_ascii_fills_frame);
  RUN_TEST(test_truncate_multibyte);
  RUN_TEST(test_truncate_three_fields_escaped);
  RUN_TEST(test_dollar_resync_variants);
  RUN_TEST(test_no_dollar_ignored);
  RUN_TEST(test_cr_ignored_inside);
  RUN_TEST(test_oversize_far_and_recover);
  RUN_TEST(test_err_frame_encode);
  RUN_TEST(test_cmd_table_matches_contract);
  RUN_TEST(test_arity_model);
  RUN_TEST(test_errc_codes_match_contract);
  RUN_TEST(test_split_edge_cases);
  RUN_TEST(test_proto_ver_is_2);
  RUN_TEST(test_parse_ints);
  RUN_TEST(test_hello_version_policy);
  RUN_TEST(test_eq_valid);
  RUN_TEST(test_eq_bounds);
  RUN_TEST(test_eq_arity);
  RUN_TEST(test_eq_not_number);
  RUN_TEST(test_eq_atomic);
  RUN_TEST(test_lvl_valid);
  RUN_TEST(test_lvl_bounds);
  RUN_TEST(test_lvl_arity);
  RUN_TEST(test_lvl_bad_slot);
  RUN_TEST(test_lvl_not_number);
  RUN_TEST(test_audio_cmd_frames);
  return UNITY_END();
}


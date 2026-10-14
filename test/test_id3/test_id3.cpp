#include <stdio.h>
#include <string.h>
#include <unity.h>

#include <string>
#include <vector>

#include "config.h"
#include "media/id3.h"

using Bytes = std::vector<uint8_t>;

// ---- читач у памʼяті з лічильником прочитаних байтів ----
class MemReader : public IFileReader {
 public:
  explicit MemReader(const Bytes& d) : d_(d) {}
  bool open(const char*) override { p_ = 0; return true; }
  size_t read(void* dst, size_t n) override {
    size_t l = d_.size() - p_ < n ? d_.size() - p_ : n;
    if (l) memcpy(dst, d_.data() + p_, l);
    p_ += l;
    total += l;
    return l;
  }
  bool seek(uint32_t pos) override { if (pos > d_.size()) return false; p_ = pos; return true; }
  uint32_t size() const override { return (uint32_t)d_.size(); }
  uint32_t pos() const override { return (uint32_t)p_; }
  bool isOpen() const override { return true; }
  bool failed() const override { return false; }
  void close() override {}
  size_t total = 0;

 private:
  const Bytes& d_;
  size_t p_ = 0;
};

// ---- будівники тегів ----
static void put(Bytes& b, const char* s) { while (*s) b.push_back((uint8_t)*s++); }
static void putv(Bytes& b, const Bytes& v) { b.insert(b.end(), v.begin(), v.end()); }
static Bytes bytes(std::initializer_list<int> l) { Bytes r; for (int x : l) r.push_back((uint8_t)x); return r; }
static void ss(Bytes& b, uint32_t v) { b.push_back((v >> 21) & 0x7F); b.push_back((v >> 14) & 0x7F); b.push_back((v >> 7) & 0x7F); b.push_back(v & 0x7F); }
static void be32(Bytes& b, uint32_t v) { b.push_back(v >> 24); b.push_back(v >> 16); b.push_back(v >> 8); b.push_back(v); }

// Кадр: payload = байт кодування + текст
static Bytes frame(int ver, const char* id, const Bytes& payload, int flags = 0) {
  Bytes f;
  put(f, id);
  uint32_t n = (uint32_t)payload.size();
  if (ver == 2) { f.push_back(n >> 16); f.push_back(n >> 8); f.push_back(n); }
  else {
    if (ver == 3) be32(f, n); else ss(f, n);
    f.push_back(flags >> 8);
    f.push_back(flags & 0xFF);
  }
  putv(f, payload);
  return f;
}
static Bytes txt(int enc, const Bytes& t) { Bytes p; p.push_back(enc); putv(p, t); return p; }
static Bytes txt(int enc, const char* s) { Bytes t; put(t, s); return txt(enc, t); }

static Bytes tag(int ver, const Bytes& frames, int flags = 0, size_t pad = 0) {
  Bytes t;
  put(t, "ID3");
  t.push_back(ver);
  t.push_back(0);
  t.push_back(flags);
  ss(t, (uint32_t)(frames.size() + pad));
  putv(t, frames);
  t.insert(t.end(), pad, 0);
  return t;
}
static void audio(Bytes& b) { for (int i = 0; i < 64; i++) b.push_back(i & 1 ? 0xFB : 0x90); }

// ---- загальні перевірки ----
static uint8_t g_scr[kId3FrameBuf + kId3WinBytes + 64];  // +64: канарка за межею скретча
static const size_t kScrLen = kId3FrameBuf + kId3WinBytes;

struct Guarded {  // Tags із канарками з обох боків
  uint8_t pre[16];
  Id3::Tags t;
  uint8_t post[16];
  Guarded() { memset(pre, 0xA5, sizeof pre); memset(post, 0xA5, sizeof post); memset(&t, 0x7E, sizeof t); }
  bool intact() const {
    for (int i = 0; i < 16; i++) if (pre[i] != 0xA5 || post[i] != 0xA5) return false;
    return true;
  }
};

static uint8_t run(const Bytes& file, Guarded& g, Id3::Info* inf = nullptr, size_t* rd = nullptr) {
  memset(g_scr, 0xC3, sizeof g_scr);
  MemReader r(file);
  uint8_t m = Id3::parse(r, g_scr, kScrLen, g.t, inf);
  for (size_t i = kScrLen; i < sizeof g_scr; i++) TEST_ASSERT_EQUAL_UINT8(0xC3, g_scr[i]);  // за межі скретча не пишемо
  TEST_ASSERT_TRUE(g.intact());
  if (rd) *rd = r.total;
  return m;
}

static void dec0(const Bytes& in, char* out, size_t cap) { Id3::decodeText(0, in.data(), in.size(), out, cap); }

void setUp(void) {}
void tearDown(void) {}

// ---------------- декодери ----------------
static void test_latin1_cafe() {
  char o[32];
  dec0(bytes({'C', 'a', 'f', 0xE9}), o, sizeof o);
  TEST_ASSERT_EQUAL_STRING("Caf\xC3\xA9", o);  // «Café» лишається Latin1: 1 з 4 байтів ≥0x80 (25% < 50%), 0x80..0x9F нема
}

static void test_cp1251_privit() {
  char o[32];
  dec0(bytes({0xCF, 0xF0, 0xE8, 0xE2, 0xB3, 0xF2}), o, sizeof o);  // 6 з 6 ≥0x80 → cp1251
  TEST_ASSERT_EQUAL_STRING("Привіт", o);
}

static void test_cp1251_ukrainian_letters() {
  char o[64];
  // Ґ ґ Є є І і Ї ї Ё ё
  dec0(bytes({0xA5, 0xB4, 0xAA, 0xBA, 0xB2, 0xB3, 0xAF, 0xBF, 0xA8, 0xB8}), o, sizeof o);
  TEST_ASSERT_EQUAL_STRING("ҐґЄєІіЇїЁё", o);
  dec0(bytes({0xC0, 0xDF, 0xE0, 0xFF}), o, sizeof o);
  TEST_ASSERT_EQUAL_STRING("АЯая", o);
}

static void test_c1_forces_cp1251() {
  char o[32];
  dec0(bytes({'D', 'o', 'n', 0x92, 't'}), o, sizeof o);  // 0x92 у cp1251/cp1252 = ’
  TEST_ASSERT_EQUAL_STRING("Don\xE2\x80\x99t", o);
}

static void test_utf8_in_latin1_field() {
  char o[32];
  Bytes u;
  put(u, "Привіт");
  dec0(u, o, sizeof o);
  TEST_ASSERT_EQUAL_STRING("Привіт", o);
  Bytes u2;
  put(u2, "Caf\xC3\xA9");
  dec0(u2, o, sizeof o);
  TEST_ASSERT_EQUAL_STRING("Caf\xC3\xA9", o);
}

static void test_utf16le_bom() {
  char o[32];
  Bytes t = bytes({0xFF, 0xFE, 0x1F, 0x04, 0x40, 0x04, 0x38, 0x04, 0x32, 0x04, 0x56, 0x04, 0x42, 0x04});  // «Привіт»
  Id3::decodeText(1, t.data(), t.size(), o, sizeof o);
  TEST_ASSERT_EQUAL_STRING("Привіт", o);
}

static void test_utf16be_nobom_and_bombe() {
  char o[32];
  Bytes t = bytes({0x04, 0x1F, 0x04, 0x40, 0x00, 'x'});
  Id3::decodeText(2, t.data(), t.size(), o, sizeof o);
  TEST_ASSERT_EQUAL_STRING("Прx", o);
  Bytes t2 = bytes({0xFE, 0xFF, 0x00, 'A', 0x00, 'B'});  // enc 1 з BOM BE
  Id3::decodeText(1, t2.data(), t2.size(), o, sizeof o);
  TEST_ASSERT_EQUAL_STRING("AB", o);
}

static void test_utf8_enc3_and_surrogate() {
  char o[32];
  Bytes u;
  put(u, "Тест");
  Id3::decodeText(3, u.data(), u.size(), o, sizeof o);
  TEST_ASSERT_EQUAL_STRING("Тест", o);
  // U+1F600 = D83D DE00 (LE)
  Bytes s = bytes({0xFF, 0xFE, 'a', 0, 0x3D, 0xD8, 0x00, 0xDE, 'b', 0});
  Id3::decodeText(1, s.data(), s.size(), o, sizeof o);
  TEST_ASSERT_EQUAL_STRING("a\xF0\x9F\x98\x80" "b", o);
  Bytes lone = bytes({0xFF, 0xFE, 0x3D, 0xD8, 'z', 0});  // одинокий старший сурогат → '?'
  Id3::decodeText(1, lone.data(), lone.size(), o, sizeof o);
  TEST_ASSERT_EQUAL_STRING("?z", o);
}

static void test_enc3_invalid_falls_back() {
  char o[32];
  Bytes t = bytes({0xCF, 0xF0, 0xE8, 0xE2, 0xB3, 0xF2});  // cp1251 із позначкою UTF-8
  Id3::decodeText(3, t.data(), t.size(), o, sizeof o);
  TEST_ASSERT_EQUAL_STRING("Привіт", o);
}

static void test_multivalue_ctrl_trim() {
  char o[32];
  Bytes t;
  put(t, "A\tB  ");
  t.push_back(0);
  put(t, "second");
  Id3::decodeText(3, t.data(), t.size(), o, sizeof o);
  TEST_ASSERT_EQUAL_STRING("A B", o);  // перше значення, \t → пробіл, хвостові пробіли знято
  Bytes e;
  Id3::decodeText(0, e.data(), 0, o, sizeof o);
  TEST_ASSERT_EQUAL_STRING("", o);
}

static void test_truncate_utf8_boundary() {
  Bytes t;
  for (int i = 0; i < 200; i++) put(t, "Я");
  char o[kEvMetaFieldMax + 1];
  size_t n = Id3::decodeText(3, t.data(), t.size(), o, sizeof o);
  TEST_ASSERT_TRUE(n <= kEvMetaFieldMax && n > 0 && n % 2 == 0);
  TEST_ASSERT_EQUAL_size_t(n, strlen(o));
  for (size_t i = 0; i < n; i += 2) { TEST_ASSERT_EQUAL_UINT8(0xD0, (uint8_t)o[i]); TEST_ASSERT_EQUAL_UINT8(0xAF, (uint8_t)o[i + 1]); }
  char tiny[3];
  Id3::decodeText(3, t.data(), t.size(), tiny, sizeof tiny);  // cap=3: один 2-байтовий символ + NUL
  TEST_ASSERT_EQUAL_size_t(2, strlen(tiny));
}

static void test_unsync_func() {
  Bytes b = bytes({0x01, 0xFF, 0x00, 0xFE, 0xFF, 0x00, 0x00, 0xFF});
  size_t n = Id3::unsync(b.data(), b.size());
  TEST_ASSERT_EQUAL_size_t(6, n);
  TEST_ASSERT_EQUAL_UINT8(0xFF, b[1]);
  TEST_ASSERT_EQUAL_UINT8(0xFE, b[2]);
  TEST_ASSERT_EQUAL_UINT8(0xFF, b[3]);
  TEST_ASSERT_EQUAL_UINT8(0x00, b[4]);
  TEST_ASSERT_EQUAL_UINT8(0xFF, b[5]);
}

// ---------------- розбір тегів ----------------
static void test_v23_basic() {
  Bytes f;
  putv(f, frame(3, "TPE1", txt(0, "Artist")));
  putv(f, frame(3, "TIT2", txt(3, "Назва, з комою*")));
  putv(f, frame(3, "TALB", txt(0, "Album")));
  Bytes file = tag(3, f, 0, 16);
  audio(file);
  Guarded g;
  Id3::Info inf;
  uint8_t m = run(file, g, &inf);
  TEST_ASSERT_EQUAL_UINT8(Id3::kFoundV2, m);
  TEST_ASSERT_EQUAL_STRING("Artist", g.t.artist);
  TEST_ASSERT_EQUAL_STRING("Назва, з комою*", g.t.title);
  TEST_ASSERT_EQUAL_STRING("Album", g.t.album);
  TEST_ASSERT_EQUAL_UINT8(3, inf.ver);
  TEST_ASSERT_EQUAL_UINT8(3, inf.f[1].enc);
}

static void test_v22_frames() {
  Bytes f;
  putv(f, frame(2, "TP1", txt(0, bytes({0xCF, 0xF0, 0xE8, 0xE2, 0xB3, 0xF2}))));
  putv(f, frame(2, "TT2", txt(0, "Title22")));
  putv(f, frame(2, "TAL", txt(0, "Alb")));
  Bytes file = tag(2, f);
  audio(file);
  Guarded g;
  run(file, g);
  TEST_ASSERT_EQUAL_STRING("Привіт", g.t.artist);
  TEST_ASSERT_EQUAL_STRING("Title22", g.t.title);
  TEST_ASSERT_EQUAL_STRING("Alb", g.t.album);
}

static void test_v24_syncsafe_big_size() {
  std::string longTitle(200, 'x');  // розмір кадру 201 > 127: syncsafe ≠ звичайне число
  Bytes f;
  putv(f, frame(4, "TIT2", txt(3, longTitle.c_str())));
  putv(f, frame(4, "TPE1", txt(3, "A")));
  Bytes file = tag(4, f);
  audio(file);
  Guarded g;
  run(file, g);
  TEST_ASSERT_EQUAL_size_t(kEvMetaFieldMax, strlen(g.t.title));  // обрізано
  TEST_ASSERT_EQUAL_STRING("A", g.t.artist);
}

static void test_v24_multivalue_first() {
  Bytes t;
  put(t, "Перший");
  t.push_back(0);
  put(t, "Другий");
  Bytes f = frame(4, "TPE1", txt(3, t));
  Bytes file = tag(4, f);
  Guarded g;
  run(file, g);
  TEST_ASSERT_EQUAL_STRING("Перший", g.t.artist);
}

static void test_v23_tag_unsync() {
  // UTF-16LE «ab»: payload 01 FF FE 61 00 62 00; у потоці після FF вставлено 00 (і в розмірі кадру 7 — до unsync)
  Bytes raw;
  put(raw, "TIT2");
  be32(raw, 7);
  raw.push_back(0); raw.push_back(0);
  putv(raw, bytes({0x01, 0xFF, 0x00, 0xFE, 0x61, 0x00, 0x62, 0x00}));  // 0x00 після FF — вставка unsync
  Bytes file = tag(3, raw, 0x80);
  audio(file);
  Guarded g;
  run(file, g);
  TEST_ASSERT_EQUAL_STRING("ab", g.t.title);
}

static void test_v24_frame_unsync() {
  Bytes pl = bytes({0x01, 0xFF, 0x00, 0xFE, 0x61, 0x00, 0x62, 0x00});  // сирий розмір 8 (після unsync буде 7)
  Bytes f = frame(4, "TIT2", pl, 0x0002);
  Bytes file = tag(4, f);
  Guarded g;
  run(file, g);
  TEST_ASSERT_EQUAL_STRING("ab", g.t.title);
}

static void test_big_frame_skipped_by_seek() {
  Bytes f;
  putv(f, frame(3, "TPE1", txt(0, "Art")));
  Bytes apic(300000, 0x55);
  putv(f, frame(3, "APIC", apic));
  putv(f, frame(3, "TIT2", txt(0, "AfterCover")));
  Bytes file = tag(3, f);  // розмір тегу ≈ 300 КБ: syncsafe вміщує
  audio(file);
  Guarded g;
  size_t rd = 0;
  uint8_t m = run(file, g, nullptr, &rd);
  TEST_ASSERT_EQUAL_UINT8(Id3::kFoundV2, m);
  TEST_ASSERT_EQUAL_STRING("AfterCover", g.t.title);
  TEST_ASSERT_EQUAL_STRING("Art", g.t.artist);
  TEST_ASSERT_TRUE(rd < 3000);  // обкладинку не читали
}

static void test_big_title_frame_reads_only_start() {
  Bytes big(50000, 'a');
  Bytes f = frame(3, "TIT2", txt(0, big));
  Bytes file = tag(3, f);
  Guarded g;
  size_t rd = 0;
  run(file, g, nullptr, &rd);
  TEST_ASSERT_EQUAL_size_t(kEvMetaFieldMax, strlen(g.t.title));
  TEST_ASSERT_TRUE(rd < 2000);
}

static void test_extended_header() {
  Bytes f;
  Bytes ext;
  be32(ext, 6);  // v2.3: розмір без поля: прапорці(2)+падінг(4)
  ext.push_back(0); ext.push_back(0); be32(ext, 0);
  putv(f, ext);
  putv(f, frame(3, "TIT2", txt(0, "WithExt")));
  Bytes file = tag(3, f, 0x40);
  Guarded g;
  run(file, g);
  TEST_ASSERT_EQUAL_STRING("WithExt", g.t.title);

  Bytes f4;
  Bytes e4;
  ss(e4, 6);  // v2.4: розмір включає поле: 4 + 1 (байт прапорців) + 1 (кількість)
  e4.push_back(1); e4.push_back(0);
  putv(f4, e4);
  putv(f4, frame(4, "TIT2", txt(0, "Ext24")));
  Bytes file4 = tag(4, f4, 0x40);
  Guarded g4;
  run(file4, g4);
  TEST_ASSERT_EQUAL_STRING("Ext24", g4.t.title);
}

static void test_v1_cp1251_and_fallback() {
  Bytes file;
  audio(file);
  Bytes v1(128, 0);
  memcpy(v1.data(), "TAG", 3);
  Bytes title = bytes({0xCF, 0xF0, 0xE8, 0xE2, 0xB3, 0xF2});  // Привіт
  memcpy(v1.data() + 3, title.data(), title.size());
  memcpy(v1.data() + 33, "Artist  ", 8);
  memset(v1.data() + 41, ' ', 22);  // пробіли замість NUL
  putv(file, v1);
  Guarded g;
  uint8_t m = run(file, g);
  TEST_ASSERT_EQUAL_UINT8(Id3::kFoundV1, m);
  TEST_ASSERT_EQUAL_STRING("Привіт", g.t.title);
  TEST_ASSERT_EQUAL_STRING("Artist", g.t.artist);
  TEST_ASSERT_EQUAL_STRING("", g.t.album);
}

static void test_v1_used_when_v2_title_missing() {
  Bytes f = frame(3, "TPE1", txt(0, "V2Artist"));
  Bytes file = tag(3, f);
  audio(file);
  Bytes v1(128, 0);
  memcpy(v1.data(), "TAG", 3);
  memcpy(v1.data() + 3, "V1Title", 7);
  putv(file, v1);
  Guarded g;
  uint8_t m = run(file, g);
  TEST_ASSERT_EQUAL_UINT8(Id3::kFoundV2 | Id3::kFoundV1, m);
  TEST_ASSERT_EQUAL_STRING("V2Artist", g.t.artist);
  TEST_ASSERT_EQUAL_STRING("V1Title", g.t.title);
}

static void test_empty_and_broken() {
  Guarded g;
  Bytes empty;
  TEST_ASSERT_EQUAL_UINT8(0, run(empty, g));
  TEST_ASSERT_EQUAL_STRING("", g.t.title);

  Bytes only = bytes({'I', 'D', '3'});
  TEST_ASSERT_EQUAL_UINT8(0, run(only, g));

  Bytes hdr;  // розмір тегу більший за файл
  put(hdr, "ID3");
  hdr.push_back(3); hdr.push_back(0); hdr.push_back(0);
  ss(hdr, 100000);
  TEST_ASSERT_EQUAL_UINT8(Id3::kFoundV2, run(hdr, g));
  TEST_ASSERT_EQUAL_STRING("", g.t.title);

  Bytes f = frame(3, "TIT2", txt(0, "Cut"));  // кадр обіцяє більше, ніж є
  f[7] = 0x7F;
  Bytes file = tag(3, f);
  run(file, g);
  TEST_ASSERT_EQUAL_STRING("", g.t.title);

  Bytes bad = tag(3, bytes({'T', 'I', '!', '2', 0, 0, 0, 3, 0, 0, 0, 'a', 'b'}));  // некоректний ID
  run(bad, g);
  TEST_ASSERT_EQUAL_STRING("", g.t.title);

  Bytes trunc = tag(3, frame(3, "TIT2", txt(1, bytes({0xFF, 0xFE, 'a'}))));  // непарна кількість байтів UTF-16
  run(trunc, g);
  TEST_ASSERT_EQUAL_STRING("", g.t.title);

  Bytes garbage;  // псевдовипадкові дані, у тому числі з «ID3» на початку
  uint32_t x = 12345;
  for (int rep = 0; rep < 200; rep++) {
    garbage.clear();
    if (rep & 1) put(garbage, "ID3");
    for (int i = 0; i < 300; i++) { x = x * 1103515245u + 12345u; garbage.push_back(x >> 16); }
    if (rep & 1) { garbage[3] = 2 + rep % 3; garbage[4] = 0; garbage[6] = garbage[7] = 0; garbage[8] = 1; garbage[9] = 0; }
    run(garbage, g);
  }
  Bytes tiny = tag(3, frame(3, "TIT2", txt(3, "x")));
  Id3::Tags t;
  MemReader r(tiny);
  uint8_t small[8];
  TEST_ASSERT_EQUAL_UINT8(0, Id3::parse(r, small, sizeof small, t));  // скретч замалий — не пише нічого
}

static void test_dup_and_flags() {
  Bytes f;
  putv(f, frame(3, "TIT2", txt(0, "Packed"), 0x0080));  // стиснутий кадр — пропуск
  putv(f, frame(3, "TIT2", txt(0, "Real")));
  Bytes file = tag(3, f);
  Guarded g;
  run(file, g);
  TEST_ASSERT_EQUAL_STRING("Real", g.t.title);
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_latin1_cafe);
  RUN_TEST(test_cp1251_privit);
  RUN_TEST(test_cp1251_ukrainian_letters);
  RUN_TEST(test_c1_forces_cp1251);
  RUN_TEST(test_utf8_in_latin1_field);
  RUN_TEST(test_utf16le_bom);
  RUN_TEST(test_utf16be_nobom_and_bombe);
  RUN_TEST(test_utf8_enc3_and_surrogate);
  RUN_TEST(test_enc3_invalid_falls_back);
  RUN_TEST(test_multivalue_ctrl_trim);
  RUN_TEST(test_truncate_utf8_boundary);
  RUN_TEST(test_unsync_func);
  RUN_TEST(test_v23_basic);
  RUN_TEST(test_v22_frames);
  RUN_TEST(test_v24_syncsafe_big_size);
  RUN_TEST(test_v24_multivalue_first);
  RUN_TEST(test_v23_tag_unsync);
  RUN_TEST(test_v24_frame_unsync);
  RUN_TEST(test_big_frame_skipped_by_seek);
  RUN_TEST(test_big_title_frame_reads_only_start);
  RUN_TEST(test_extended_header);
  RUN_TEST(test_v1_cp1251_and_fallback);
  RUN_TEST(test_v1_used_when_v2_title_missing);
  RUN_TEST(test_empty_and_broken);
  RUN_TEST(test_dup_and_flags);
  return UNITY_END();
}

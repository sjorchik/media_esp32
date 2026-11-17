// Unit-тести M4b4: USB як джерело — частини, які не потребують заліза. Запуск: pio test -e native -f test_usb_src
//  • скан і плейлист на mock-томі (той самий Scanner, що й для SD: імена, вкладеність, не-mp3, довгі імена, переповнення, порядок);
//  • резюме rsm_usb: серійний номер тому (збіг / інша флешка / без номера), межі шляху;
//  • політика життя USB-носія (відкриття, прапорець MEDIA, виймання, повтор, вихід);
//  • профіль джерела (слот рівня usb, ключ rsm_usb) і серійний номер у розборі BPB/exFAT.
#include <string.h>
#include <unity.h>

#include <string>
#include <vector>

#include "media/imedia.h"
#include "media/playlist.h"
#include "media/scanner.h"
#include "sources/src_profile.h"
#include "sources/usb_media_policy.h"
#include "storage/resume_vol.h"
#include "usb/disk_probe.h"
#include "util/clock.h"

void setUp() {
  NativeClock::set(0);
  NativeClock::setAutoStep(1);  // Scanner міряє час; кожен виклик nowMs() +1 мс
}
void tearDown() {}

namespace {

// ---- mock-том: дерево в памʼяті; порядок записів = порядок додавання (як записи каталогу FAT) ----
struct Node {
  std::string name;
  bool dir = false, hidden = false, longName = false;
  std::vector<Node> kids;
};

class NullReader final : public IFileReader {
 public:
  bool open(const char*) override { return false; }
  size_t read(void*, size_t) override { return 0; }
  bool seek(uint32_t) override { return false; }
  uint32_t size() const override { return 0; }
  uint32_t pos() const override { return 0; }
  bool isOpen() const override { return false; }
  bool failed() const override { return false; }
  void close() override {}
};

class MockVol final : public IMedia {
 public:
  Node root;
  bool up = true;
  bool begin() override { return true; }
  MediaEvent poll(uint32_t) override { return MediaEvent::None; }
  bool mount() override { return up; }
  void unmount() override { up = false; }
  bool mounted() const override { return up; }
  bool present() const override { return up; }
  int openDir(const char* path) override {
    const Node* n = find(path);
    if (!up || !n) return -1;
    for (size_t i = 0; i < h_.size(); i++)
      if (!h_[i].n) { h_[i] = H{n, 0}; return (int)i; }
    h_.push_back(H{n, 0});
    return (int)h_.size() - 1;
  }
  int nextEntry(int h, MediaEntry& e) override {
    if (!up) return -1;
    H& x = h_[h];
    if (x.i >= x.n->kids.size()) return 0;
    const Node& k = x.n->kids[x.i++];
    e.nameOk = !k.longName;
    if (e.nameOk) strncpy(e.name, k.name.c_str(), sizeof e.name - 1), e.name[sizeof e.name - 1] = 0;
    else e.name[0] = 0;
    e.isDir = k.dir;
    e.hidden = k.hidden;
    return 1;
  }
  void closeDir(int h) override { h_[h].n = nullptr; }
  IFileReader& reader() override { return rd_; }
  void info(MediaInfo& o) override { o = MediaInfo{}; }
  int openHandles() const { int c = 0; for (auto& x : h_) c += x.n ? 1 : 0; return c; }

 private:
  struct H { const Node* n; size_t i; };
  std::vector<H> h_;
  NullReader rd_;
  const Node* find(const char* path) const {
    const Node* n = &root;
    std::string p(path);
    size_t i = 0;
    while (i < p.size()) {
      if (p[i] == '/') { i++; continue; }
      size_t j = p.find('/', i);
      std::string c = p.substr(i, j == std::string::npos ? std::string::npos : j - i);
      const Node* nx = nullptr;
      for (auto& k : n->kids) if (k.dir && k.name == c) { nx = &k; break; }
      if (!nx) return nullptr;
      n = nx;
      i = j == std::string::npos ? p.size() : j;
    }
    return n;
  }
};

Node F(const char* n, bool hidden = false) { Node x; x.name = n; x.hidden = hidden; return x; }
Node D(const char* n, std::vector<Node> k) { Node x; x.name = n; x.dir = true; x.kids = std::move(k); return x; }

Scanner::St runScan(Scanner& sc, MockVol& v, Playlist& pl) {
  sc.start(v, pl);
  for (int i = 0; i < 100000 && sc.state() == Scanner::St::Running; i++) sc.step(5);
  return sc.state();
}

std::string trackPath(const Playlist& pl, uint16_t i) {
  char b[300];
  return pl.trackPath(i, b, sizeof b) ? b : "?";
}

}  // namespace

// ---------------------------------------------------------------- скан на mock-томі
void test_scan_names_nesting_order() {
  MockVol v;
  v.root = D("", {F("b.MP3"), F("a.mp3"), F("note.txt"), D("Rock", {F("x.mp3"), D("Live", {F("l1.mp3")}), F("y.mp3")}), F("c.mp3"), D("Jazz", {F("j.mp3")})});
  Playlist pl;
  Scanner sc;
  TEST_ASSERT_EQUAL((int)Scanner::St::Done, (int)runScan(sc, v, pl));
  // порядок: файли каталогу (за записами) → підкаталоги (за записами), рекурсивно
  TEST_ASSERT_EQUAL_UINT16(7, pl.tracks());
  const char* want[] = {"/b.mp3", "/a.mp3", "/c.mp3", "/Rock/x.mp3", "/Rock/y.mp3", "/Rock/Live/l1.mp3", "/Jazz/j.mp3"};
  for (int i = 0; i < 7; i++) TEST_ASSERT_EQUAL_STRING(want[i], trackPath(pl, (uint16_t)i).c_str());
  TEST_ASSERT_EQUAL(0, v.openHandles());  // усі каталоги закрито
  // трек зберігається без розширення, trackPath() завжди дописує ".mp3" (регістр розширення не зберігається)
  TEST_ASSERT_EQUAL_UINT16(0, pl.indexOfPath("/b.mp3"));
  TEST_ASSERT_EQUAL_UINT16(Playlist::kNone, pl.indexOfPath("/nope.mp3"));
}

void test_scan_skips_nonmp3_hidden_dot_long_deep() {
  MockVol v;
  Node longN = F("whatever.mp3");
  longN.longName = true;
  v.root = D("", {F("a.mp3"), F("a.mp3.bak"), F(".hid.mp3"), F("h.mp3", true), longN, F("mp3"), F(".mp3"), F("Z.mP3"),
                  D("L1", {D("L2", {D("L3", {F("ok.mp3"), D("L4", {F("deep.mp3")})})})})});
  Playlist pl;
  Scanner sc;
  runScan(sc, v, pl);
  TEST_ASSERT_EQUAL_UINT16(3, pl.tracks());  // a.mp3, Z.mP3, ok.mp3 (L3 — рівень 3; L4 глибше kScanMaxDepth)
  TEST_ASSERT_EQUAL_STRING("/L1/L2/L3/ok.mp3", trackPath(pl, 2).c_str());
  TEST_ASSERT_EQUAL_UINT32(1, sc.stats().deepDirs);
  TEST_ASSERT_EQUAL_UINT32(1, sc.stats().longName);
  TEST_ASSERT_TRUE(sc.stats().hidden >= 3);  // ".hid.mp3", "h.mp3"(hidden), ".mp3"
}

void test_scan_empty_and_no_mp3() {
  MockVol v;
  v.root = D("", {F("readme.txt"), D("docs", {F("a.pdf")})});
  Playlist pl;
  Scanner sc;
  TEST_ASSERT_EQUAL((int)Scanner::St::Done, (int)runScan(sc, v, pl));
  TEST_ASSERT_EQUAL_UINT16(0, pl.tracks());  // порожній носій → джерело дасть ERR,no_media,empty
}

void test_scan_arena_overflow() {
  MockVol v;
  v.root = D("", {});
  std::string nm(120, 'n');
  for (int i = 0; i < 400; i++) {
    char b[8];
    snprintf(b, sizeof b, "%03d", i);
    v.root.kids.push_back(F((nm + b + ".mp3").c_str()));
  }
  Playlist pl;
  Scanner sc;
  TEST_ASSERT_EQUAL((int)Scanner::St::Overflow, (int)runScan(sc, v, pl));
  TEST_ASSERT_TRUE(sc.stats().overflow);
  TEST_ASSERT_TRUE(pl.tracks() > 0 && pl.tracks() < 400);
  TEST_ASSERT_TRUE(pl.used() <= Playlist::capacity());
  TEST_ASSERT_EQUAL(0, v.openHandles());
}

void test_scan_media_lost_midway() {
  MockVol v;
  v.root = D("", {F("a.mp3"), D("S", {F("b.mp3")})});
  Playlist pl;
  Scanner sc;
  sc.start(v, pl);
  v.up = false;  // флешку висмикнули
  for (int i = 0; i < 100 && sc.state() == Scanner::St::Running; i++) sc.step(5);
  TEST_ASSERT_EQUAL((int)Scanner::St::Failed, (int)sc.state());  // → джерело відкладає вердикт і чекає Removed
}

// ---------------------------------------------------------------- резюме USB із серійним номером
void test_resume_vol_prefix_and_check() {
  char buf[Resume::kPathMax + 1];
  ResumeVol::putPrefix(buf, 0xA1B2C3D4u);
  strcpy(buf + ResumeVol::kPrefix, "/Rock/x.mp3");
  TEST_ASSERT_EQUAL_STRING("A1B2C3D4|/Rock/x.mp3", buf);
  const char* rest = nullptr;
  uint32_t st = 0;
  TEST_ASSERT_EQUAL((int)ResumeVol::Verdict::Match, (int)ResumeVol::check(buf, 0xA1B2C3D4u, rest, &st));
  TEST_ASSERT_EQUAL_STRING("/Rock/x.mp3", rest);
  TEST_ASSERT_EQUAL_HEX32(0xA1B2C3D4u, st);
  TEST_ASSERT_EQUAL((int)ResumeVol::Verdict::OtherVolume, (int)ResumeVol::check(buf, 0x11111111u, rest, &st));
  TEST_ASSERT_EQUAL((int)ResumeVol::Verdict::NoId, (int)ResumeVol::check(buf, 0, rest, &st));
  ResumeVol::putPrefix(buf, 0x0000000Fu);
  TEST_ASSERT_EQUAL_CHAR_ARRAY("0000000F|", buf, 9);
}

void test_resume_vol_bad_format() {
  const char* rest;
  TEST_ASSERT_EQUAL((int)ResumeVol::Verdict::BadFormat, (int)ResumeVol::check("/Rock/x.mp3", 5, rest));
  TEST_ASSERT_EQUAL((int)ResumeVol::Verdict::BadFormat, (int)ResumeVol::check("a1b2c3d4|/x.mp3", 5, rest));  // нижній регістр не пишемо
  TEST_ASSERT_EQUAL((int)ResumeVol::Verdict::BadFormat, (int)ResumeVol::check("A1B2C3D4|", 5, rest));       // порожній шлях
  TEST_ASSERT_EQUAL((int)ResumeVol::Verdict::BadFormat, (int)ResumeVol::check("A1B2C3D4/x.mp3", 5, rest));
  TEST_ASSERT_EQUAL((int)ResumeVol::Verdict::BadFormat, (int)ResumeVol::check(nullptr, 5, rest));
}

void test_resume_vol_blob_roundtrip_and_limit() {
  // блоб Resume (формат без змін) переносить префікс разом зі шляхом; найдовший шлях = kPathMax - 9
  static_assert(ResumeVol::kPathMax == Resume::kPathMax - 9, "межа шляху");
  char full[Resume::kPathMax + 1];
  ResumeVol::putPrefix(full, 0xCAFEBABEu);
  std::string p = "/" + std::string(ResumeVol::kPathMax - 1, 'a');
  memcpy(full + ResumeVol::kPrefix, p.c_str(), p.size() + 1);
  uint8_t blob[Resume::kBlobMax];
  size_t n = Resume::serialize(full, strlen(full), 123456, 7890, blob, sizeof blob);
  TEST_ASSERT_TRUE(n > 0);
  Resume::Record r;
  TEST_ASSERT_EQUAL((int)Resume::ParseErr::Ok, (int)Resume::parse(blob, n, r));
  const char* rest;
  TEST_ASSERT_EQUAL((int)ResumeVol::Verdict::Match, (int)ResumeVol::check(r.path, 0xCAFEBABEu, rest));
  TEST_ASSERT_EQUAL_STRING(p.c_str(), rest);
  TEST_ASSERT_EQUAL_UINT32(123456, r.off);
  // ще на байт довший шлях у блоб не влізе
  std::string tooLong = std::string(full) + "x";
  TEST_ASSERT_EQUAL(0, (int)Resume::serialize(tooLong.c_str(), tooLong.size(), 0, 0, blob, sizeof blob));
}

// ---------------------------------------------------------------- політика життя USB-носія
void test_policy_open_scan_ok_then_removed() {
  UsbMediaPolicy p;
  p.mountResult(true);
  TEST_ASSERT_FALSE(p.flag());  // том є, але треків ще не знаємо: MEDIA usb = 0
  MediaStep s = p.scanDone(12, false);
  TEST_ASSERT_TRUE(s.flagChanged && s.flag && !s.release);  // MEDIA usb → 1
  s = p.removed();  // виймання посеред відтворення
  TEST_ASSERT_TRUE(s.flagChanged && !s.flag && s.release);
  s = p.removed();  // дубль: нічого вдруге (ERR,no_media лише один)
  TEST_ASSERT_TRUE(!s.flagChanged && !s.release);
  TEST_ASSERT_FALSE(p.open());
}

void test_policy_mount_fail_and_empty_and_failed_scan() {
  UsbMediaPolicy a;
  a.mountResult(false);  // немає флешки: нічого закривати, прапорець 0 (STATE idle + ERR,no_media від менеджера)
  MediaStep s = a.deactivate();
  TEST_ASSERT_TRUE(!s.flagChanged && !s.release);
  UsbMediaPolicy b;
  b.mountResult(true);
  s = b.scanDone(0, false);  // порожня флешка → том закрито, прапорець не вмикався
  TEST_ASSERT_TRUE(!s.flagChanged && s.release);
  UsbMediaPolicy c;
  c.mountResult(true);
  s = c.scanDone(5, true);  // остаточний збій скану
  TEST_ASSERT_TRUE(!s.flagChanged && s.release && !c.flag());
}

void test_policy_deactivate_and_remount() {
  UsbMediaPolicy p;
  p.mountResult(true);
  p.scanDone(3, false);
  MediaStep s = p.deactivate();  // вихід з usb: MEDIA → 0, том закрито
  TEST_ASSERT_TRUE(s.flagChanged && !s.flag && s.release);
  p.mountResult(true);  // повторне SRC,usb після вставлення
  s = p.scanDone(3, false);
  TEST_ASSERT_TRUE(s.flagChanged && s.flag);
}

// ---------------------------------------------------------------- профіль
void test_profiles() {
  TEST_ASSERT_EQUAL((int)SrcId::USB, (int)kUsbProfile.id);
  TEST_ASSERT_EQUAL((int)level::Slot::Usb, (int)kUsbProfile.slot);
  TEST_ASSERT_EQUAL((int)ResumeStore::Src::Usb, (int)kUsbProfile.rsm);
  TEST_ASSERT_TRUE(kUsbProfile.ownsMount && kUsbProfile.volId);
  TEST_ASSERT_EQUAL((int)SrcId::SD, (int)kSdProfile.id);
  TEST_ASSERT_EQUAL((int)level::Slot::Sd, (int)kSdProfile.slot);
  TEST_ASSERT_EQUAL((int)ResumeStore::Src::Sd, (int)kSdProfile.rsm);
  TEST_ASSERT_TRUE(!kSdProfile.ownsMount && !kSdProfile.volId);  // SD: поведінка без змін
  TEST_ASSERT_NOT_EQUAL((int)kSdProfile.slot, (int)kUsbProfile.slot);
}

void test_level_slot_is_independent() {  // слот usb має власну ціль (-20 для usb не зачіпає sd)
  level::setDb(level::Slot::Usb, -20);
  level::setDb(level::Slot::Sd, 0);
  TEST_ASSERT_EQUAL(-20, level::getDb(kUsbProfile.slot));
  TEST_ASSERT_EQUAL(0, level::getDb(kSdProfile.slot));
  level::Gain g = level::advance(kUsbProfile.slot, 64);
  TEST_ASSERT_EQUAL(2, level::lastSlot());  // 2 = usb
  TEST_ASSERT_FALSE(g.unity);
  level::setDb(level::Slot::Usb, 0);
}

// ---------------------------------------------------------------- серійний номер у розборі тому
static void p32(uint8_t* p, uint32_t v) { for (int i = 0; i < 4; i++) p[i] = (uint8_t)(v >> (8 * i)); }
static void p16(uint8_t* p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }

void test_serial_fat32_fat16_exfat() {
  uint8_t s[512];
  memset(s, 0, sizeof s);
  s[0] = 0xEB; s[1] = 0x58; s[2] = 0x90;
  p16(s + 11, 512); s[13] = 8; p16(s + 14, 32); s[16] = 2;
  p32(s + 32, 2000000); p32(s + 36, 2000); p32(s + 44, 2); p32(s + 0x43, 0x1234ABCDu);
  s[510] = 0x55; s[511] = 0xAA;
  BpbInfo b;
  TEST_ASSERT_TRUE(parseBpb(s, b));
  TEST_ASSERT_EQUAL_HEX32(0x1234ABCDu, b.serial);  // FAT32: зсув 0x43

  memset(s, 0, sizeof s);
  s[0] = 0xEB; s[1] = 0x3C; s[2] = 0x90;
  p16(s + 11, 512); s[13] = 4; p16(s + 14, 4); s[16] = 2; p16(s + 17, 512);
  p16(s + 19, 40000); p16(s + 22, 40); p32(s + 0x27, 0x0BADF00Du);
  s[510] = 0x55; s[511] = 0xAA;
  TEST_ASSERT_TRUE(parseBpb(s, b));
  TEST_ASSERT_EQUAL_HEX32(0x0BADF00Du, b.serial);  // FAT12/16: зсув 0x27

  memset(s, 0, sizeof s);
  s[0] = 0xEB; s[1] = 0x76; s[2] = 0x90;
  memcpy(s + 3, "EXFAT   ", 8);
  p32(s + 0x50, 128); p32(s + 0x54, 1000); p32(s + 0x58, 2048); p32(s + 0x5C, 50000); p32(s + 0x60, 4);
  s[0x6C] = 9; s[0x6D] = 6; s[0x6E] = 1; p32(s + 0x64, 0xDEADBEEFu);
  s[510] = 0x55; s[511] = 0xAA;
  ExFatInfo e;
  TEST_ASSERT_TRUE(parseExFat(s, e));
  TEST_ASSERT_EQUAL_HEX32(0xDEADBEEFu, e.serial);
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_scan_names_nesting_order);
  RUN_TEST(test_scan_skips_nonmp3_hidden_dot_long_deep);
  RUN_TEST(test_scan_empty_and_no_mp3);
  RUN_TEST(test_scan_arena_overflow);
  RUN_TEST(test_scan_media_lost_midway);
  RUN_TEST(test_resume_vol_prefix_and_check);
  RUN_TEST(test_resume_vol_bad_format);
  RUN_TEST(test_resume_vol_blob_roundtrip_and_limit);
  RUN_TEST(test_policy_open_scan_ok_then_removed);
  RUN_TEST(test_policy_mount_fail_and_empty_and_failed_scan);
  RUN_TEST(test_policy_deactivate_and_remount);
  RUN_TEST(test_profiles);
  RUN_TEST(test_level_slot_is_independent);
  RUN_TEST(test_serial_fat32_fat16_exfat);
  return UNITY_END();
}

#include <stdio.h>
#include <string.h>
#include <unity.h>

#include <string>
#include <vector>

#include "media/imedia.h"
#include "media/playlist.h"
#include "media/scanner.h"
#include "util/clock.h"

// M4c1: однопрохідний Scanner (USB) дає ті самі треки, порядок і статистику, що двопрохідний (SD), але читає каталоги вдвічі менше.
void setUp() {
  NativeClock::set(0);
  NativeClock::setAutoStep(1);
}
void tearDown() {}

namespace {
struct Node {
  std::string name;
  bool dir = false, hidden = false, longName = false;
  std::vector<Node> kids;
};

class MockVol final : public IMedia {
 public:
  Node root;
  bool up = true;
  int nextCalls = 0, opens = 0;
  bool begin() override { return true; }
  MediaEvent poll(uint32_t) override { return MediaEvent::None; }
  bool mount() override { return up; }
  void unmount() override { up = false; }
  bool mounted() const override { return up; }
  bool present() const override { return up; }
  int openDir(const char* path) override {
    const Node* n = find(path);
    if (!up || !n) return -1;
    opens++;
    for (size_t i = 0; i < h_.size(); i++)
      if (!h_[i].n) { h_[i] = H{n, 0}; return (int)i; }
    h_.push_back(H{n, 0});
    return (int)h_.size() - 1;
  }
  int nextEntry(int h, MediaEntry& e) override {
    if (!up) return -1;
    nextCalls++;
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
  IFileReader& reader() override { return *(IFileReader*)nullptr; }  // Scanner читач не використовує
  void info(MediaInfo& o) override { memset(&o, 0, sizeof o); }
  int openHandles() const { int c = 0; for (auto& x : h_) c += x.n ? 1 : 0; return c; }

 private:
  struct H { const Node* n; size_t i; };
  std::vector<H> h_;
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

Node F(const std::string& n, bool hidden = false) { Node x; x.name = n; x.hidden = hidden; return x; }
Node D(const std::string& n, std::vector<Node> k) { Node x; x.name = n; x.dir = true; x.kids = std::move(k); return x; }

struct Run {
  Scanner::St st;
  std::vector<std::string> paths;
  ScanStats s;
  int nextCalls, opens;
};

Run runScan(MockVol& v, bool onePass) {
  Playlist pl;
  Scanner sc;
  sc.setOnePass(onePass);
  v.nextCalls = v.opens = 0;
  sc.start(v, pl);
  for (int i = 0; i < 1000000 && sc.state() == Scanner::St::Running; i++) sc.step(5);
  Run r{sc.state(), {}, sc.stats(), v.nextCalls, v.opens};
  for (uint16_t i = 0; i < pl.tracks(); i++) {
    char b[300];
    r.paths.push_back(pl.trackPath(i, b, sizeof b) ? b : "?");
  }
  TEST_ASSERT_EQUAL(0, v.openHandles());
  return r;
}

void same(MockVol& v, bool expectFewer = true) {
  Run a = runScan(v, false), b = runScan(v, true);
  TEST_ASSERT_EQUAL((int)a.st, (int)b.st);
  TEST_ASSERT_EQUAL((size_t)a.paths.size(), (size_t)b.paths.size());
  for (size_t i = 0; i < a.paths.size(); i++) TEST_ASSERT_EQUAL_STRING(a.paths[i].c_str(), b.paths[i].c_str());
  TEST_ASSERT_EQUAL_UINT32(a.s.entries, b.s.entries);
  TEST_ASSERT_EQUAL_UINT32(a.s.hidden, b.s.hidden);
  TEST_ASSERT_EQUAL_UINT32(a.s.longName, b.s.longName);
  TEST_ASSERT_EQUAL_UINT32(a.s.deepDirs, b.s.deepDirs);
  TEST_ASSERT_EQUAL_UINT32(a.s.dirErr, b.s.dirErr);
  TEST_ASSERT_EQUAL_UINT32(a.s.dropped, b.s.dropped);
  TEST_ASSERT_EQUAL_UINT16(a.s.tracks, b.s.tracks);
  TEST_ASSERT_EQUAL_UINT16(a.s.foldersTotal, b.s.foldersTotal);
  TEST_ASSERT_EQUAL_UINT16(a.s.foldersWithTracks, b.s.foldersWithTracks);
  if (expectFewer) TEST_ASSERT_TRUE(b.nextCalls < a.nextCalls);
  TEST_ASSERT_TRUE(b.opens <= a.opens);
}
}  // namespace

void test_nesting_order_same() {
  MockVol v;
  v.root = D("", {F("b.MP3"), F("a.mp3"), F("note.txt"), D("Rock", {F("x.mp3"), D("Live", {F("l1.mp3")}), F("y.mp3")}), F("c.mp3"), D("Jazz", {F("j.mp3")})});
  same(v);
  Run b = runScan(v, true);
  TEST_ASSERT_EQUAL_UINT16(7, (uint16_t)b.paths.size());
  TEST_ASSERT_EQUAL_STRING("/Rock/Live/l1.mp3", b.paths[5].c_str());
  TEST_ASSERT_EQUAL_UINT32(0, b.s.reread);
}

void test_hidden_dot_long_deep_same() {
  MockVol v;
  Node ln = D("bad", {F("z.mp3")});
  ln.longName = true;
  v.root = D("", {F(".hid.mp3"), F("h.mp3", true), D(".git", {F("g.mp3")}), D("hd", {F("q.mp3")}), ln,
                  D("L1", {D("L2", {D("L3", {D("L4", {F("deep.mp3")}), F("l3.mp3")}), F("l2.mp3")}), F("l1.mp3")}), F("ok.mp3")});
  v.root.kids[3].hidden = true;
  same(v);
  Run b = runScan(v, true);
  TEST_ASSERT_EQUAL_UINT32(1, b.s.deepDirs);
}

void test_empty_and_dirs_only() {
  MockVol v;
  v.root = D("", {});
  same(v, false);
  v.root = D("", {D("A", {}), D("B", {D("C", {})})});
  same(v, false);
}

void test_pool_overflow_falls_back_to_reread() {
  MockVol v;
  std::vector<Node> k;
  for (int i = 0; i < 40; i++) {
    char nm[64];
    snprintf(nm, sizeof nm, "very_long_folder_name_number_%02d_xxxxxxxx", i);
    k.push_back(D(nm, {F("t.mp3")}));
  }
  v.root = D("", k);
  same(v, false);
  Run b = runScan(v, true);
  TEST_ASSERT_EQUAL_UINT16(40, (uint16_t)b.paths.size());
  TEST_ASSERT_TRUE(b.s.reread >= 1);
}

void test_arena_overflow_same() {
  MockVol v;
  std::vector<Node> k;
  for (int d = 0; d < 6; d++) {
    std::vector<Node> f;
    for (int i = 0; i < 900; i++) {
      char nm[64];
      snprintf(nm, sizeof nm, "track_number_%04d_with_some_long_name.mp3", i);
      f.push_back(F(nm));
    }
    char dn[16];
    snprintf(dn, sizeof dn, "d%d", d);
    k.push_back(D(dn, f));
  }
  v.root = D("", k);
  Run a = runScan(v, false), b = runScan(v, true);
  TEST_ASSERT_EQUAL((int)Scanner::St::Overflow, (int)a.st);
  TEST_ASSERT_EQUAL((int)Scanner::St::Overflow, (int)b.st);
  TEST_ASSERT_EQUAL((size_t)a.paths.size(), (size_t)b.paths.size());
  TEST_ASSERT_EQUAL_UINT32(a.s.dropped, b.s.dropped);
}

void test_reads_reduced_big_tree() {
  MockVol v;
  std::vector<Node> k;
  for (int d = 0; d < 15; d++) {
    std::vector<Node> f;
    for (int i = 0; i < 78; i++) {
      char nm[40];
      snprintf(nm, sizeof nm, "t%02d.mp3", i);
      f.push_back(F(nm));
    }
    char dn[16];
    snprintf(dn, sizeof dn, "folder%d", d);
    k.push_back(D(dn, f));
  }
  v.root = D("", k);
  Run a = runScan(v, false), b = runScan(v, true);
  TEST_ASSERT_EQUAL_UINT16(1170, (uint16_t)b.paths.size());
  TEST_ASSERT_TRUE(b.nextCalls < a.nextCalls * 6 / 10);  // майже вдвічі менше
}

void test_abort_releases_handles_and_pool() {
  MockVol v;
  v.root = D("", {F("a.mp3"), D("X", {F("b.mp3"), D("Y", {F("c.mp3")})}), D("Z", {F("d.mp3")})});
  Playlist pl;
  Scanner sc;
  sc.setOnePass(true);
  sc.start(v, pl);
  for (int i = 0; i < 4 && sc.state() == Scanner::St::Running; i++) sc.step(0);
  sc.abort();
  TEST_ASSERT_EQUAL((int)Scanner::St::Idle, (int)sc.state());
  TEST_ASSERT_EQUAL(0, v.openHandles());
  sc.start(v, pl);  // повторний запуск після переривання — чистий стан
  for (int i = 0; i < 1000 && sc.state() == Scanner::St::Running; i++) sc.step(5);
  TEST_ASSERT_EQUAL((int)Scanner::St::Done, (int)sc.state());
  TEST_ASSERT_EQUAL_UINT16(4, pl.tracks());
}

void test_disk_gone_midscan_fails_clean() {
  MockVol v;
  v.root = D("", {F("a.mp3"), D("X", {F("b.mp3")})});
  Playlist pl;
  Scanner sc;
  sc.setOnePass(true);
  sc.start(v, pl);
  sc.step(0);
  v.up = false;
  for (int i = 0; i < 100 && sc.state() == Scanner::St::Running; i++) sc.step(5);
  TEST_ASSERT_EQUAL((int)Scanner::St::Failed, (int)sc.state());
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_nesting_order_same);
  RUN_TEST(test_hidden_dot_long_deep_same);
  RUN_TEST(test_empty_and_dirs_only);
  RUN_TEST(test_pool_overflow_falls_back_to_reread);
  RUN_TEST(test_arena_overflow_same);
  RUN_TEST(test_reads_reduced_big_tree);
  RUN_TEST(test_abort_releases_handles_and_pool);
  RUN_TEST(test_disk_gone_midscan_fails_clean);
  return UNITY_END();
}

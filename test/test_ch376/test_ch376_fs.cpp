#include <stdio.h>
#include <string.h>
#include <unity.h>

#include <string>
#include <vector>

#include "config.h"
#include "mock_fs_link.h"
#include "usb/ch376_file_reader.h"
#include "usb/ch376_fs.h"

// Тести файлового шару CH376S (M4b1) на mock-у з test/test_ch376/mock_fs_link.h.
// Дерево: ROOT.TXT(50) MUSIC/{ALBUM/TRACK01.MP3(1000), SONG.MP3(300)} BIG.BIN(100000) EMPTYD/ ; id вузла = індекс (патерн вмісту).

namespace {

struct Tree {
  MockFsLink L;
  int idTrack = 0, idSong = 0, idBig = 0, idRoot = 0;
  Tree() {
    idRoot = L.addFile(0, "ROOT.TXT", 50);
    const int music = L.addDir(0, "MUSIC");
    const int album = L.addDir(music, "ALBUM");
    idTrack = L.addFile(album, "TRACK01.MP3", 1000);
    idSong = L.addFile(music, "SONG.MP3", 300);
    idBig = L.addFile(0, "BIG.BIN", 100000);
    L.addDir(0, "EMPTYD");
    L.volLabel = true;
  }
};

bool patOk(const uint8_t* p, int id, uint32_t off, size_t n) {
  for (size_t i = 0; i < n; i++)
    if (p[i] != fsPattern(id, off + (uint32_t)i)) return false;
  return true;
}

struct Names {
  std::vector<std::string> v;
  std::vector<uint32_t> sizes;
  std::vector<bool> dirs;
};
bool collect(const Ch376DirEntry& e, void* c) {
  Names* n = static_cast<Names*>(c);
  n->v.push_back(e.name);
  n->sizes.push_back(e.size);
  n->dirs.push_back(e.isDir());
  return true;
}

}  // namespace

// ---------------------------------------------------------------- шлях і відкриття

void test_fs_path_root_is_dir() {
  Tree t;
  Ch376Fs fs(t.L);
  FsKind k;
  TEST_ASSERT_EQUAL_INT((int)FsErr::Ok, (int)fs.openPath("/", k));
  TEST_ASSERT_EQUAL_INT((int)FsKind::Dir, (int)k);
  TEST_ASSERT_EQUAL_UINT32(1, t.L.names.size());
  TEST_ASSERT_EQUAL_STRING("/", t.L.names[0].c_str());
}

void test_fs_path_by_elements() {
  Tree t;
  Ch376Fs fs(t.L);
  FsKind k;
  TEST_ASSERT_EQUAL_INT((int)FsErr::Ok, (int)fs.openPath("/MUSIC/ALBUM/TRACK01.MP3", k));
  TEST_ASSERT_EQUAL_INT((int)FsKind::File, (int)k);
  TEST_ASSERT_TRUE(fs.fileOpen());
  TEST_ASSERT_EQUAL_UINT32(3, t.L.names.size());
  TEST_ASSERT_EQUAL_STRING("/MUSIC", t.L.names[0].c_str());
  TEST_ASSERT_EQUAL_STRING("ALBUM", t.L.names[1].c_str());
  TEST_ASSERT_EQUAL_STRING("TRACK01.MP3", t.L.names[2].c_str());
  uint32_t sz = 0;
  TEST_ASSERT_EQUAL_INT((int)FsErr::Ok, (int)fs.fileSize(sz));
  TEST_ASSERT_EQUAL_UINT32(1000, sz);
}

void test_fs_path_uppercased_and_slashes() {
  Tree t;
  Ch376Fs fs(t.L);
  FsKind k;
  TEST_ASSERT_EQUAL_INT((int)FsErr::Ok, (int)fs.openPath("//music//album/", k));
  TEST_ASSERT_EQUAL_INT((int)FsKind::Dir, (int)k);
  TEST_ASSERT_EQUAL_STRING("/MUSIC", t.L.names[0].c_str());
  TEST_ASSERT_EQUAL_STRING("ALBUM", t.L.names[1].c_str());
  TEST_ASSERT_EQUAL_INT((int)FsErr::Ok, (int)fs.openPath("/music/album/track01.mp3", k));
  TEST_ASSERT_EQUAL_INT((int)FsKind::File, (int)k);
}

void test_fs_open_missing() {
  Tree t;
  Ch376Fs fs(t.L);
  FsKind k;
  TEST_ASSERT_EQUAL_INT((int)FsErr::NotFound, (int)fs.openPath("/MUSIC/NOFILE.MP3", k));
  TEST_ASSERT_EQUAL_INT((int)FsKind::None, (int)k);
  TEST_ASSERT_EQUAL_INT((int)FsErr::NotFound, (int)fs.openPath("/NODIR/X.MP3", k));
  TEST_ASSERT_FALSE(fs.fileOpen());
}

void test_fs_file_in_middle_is_notdir_and_closed() {
  Tree t;
  Ch376Fs fs(t.L);
  FsKind k;
  TEST_ASSERT_EQUAL_INT((int)FsErr::NotDir, (int)fs.openPath("/ROOT.TXT/X", k));
  TEST_ASSERT_FALSE(fs.fileOpen());
  TEST_ASSERT_EQUAL_INT(-1, t.L.openFile);
}

void test_fs_bad_names_never_touch_chip() {
  Tree t;
  Ch376Fs fs(t.L);
  FsKind k;
  const char* bad[] = {"/TOOLONGNAME.MP3", "/A.B.C", "/A*B", "/..", "/MUSIC/SP ACE.MP3", "/NAME.LONG", "/.HID", "/1/2/3/4/5/6/7/8/9"};
  for (const char* p : bad) TEST_ASSERT_EQUAL_INT((int)FsErr::BadName, (int)fs.openPath(p, k));
  TEST_ASSERT_EQUAL_UINT32(0, t.L.frames.size());
  TEST_ASSERT_EQUAL_INT((int)FsErr::BadArg, (int)fs.openPath(nullptr, k));
  TEST_ASSERT_TRUE(Ch376Fs::valid83("TRACK01.MP3", 11));
  TEST_ASSERT_TRUE(Ch376Fs::valid83("a_b-c~1", 7));
}

void test_fs_frame_bytes() {
  Tree t;
  Ch376Fs fs(t.L);
  FsKind k;
  TEST_ASSERT_EQUAL_INT((int)FsErr::Ok, (int)fs.openPath("/BIG.BIN", k));
  // кадри: GET_STATUS (скид зайвої події), SET_FILE_NAME "/BIG.BIN\0" одним записом, FILE_OPEN
  const std::vector<uint8_t> nm = {0x57, 0xAB, 0x2F, '/', 'B', 'I', 'G', '.', 'B', 'I', 'N', 0x00};
  bool found = false;
  for (auto& f : t.L.frames) found = found || f == nm;
  TEST_ASSERT_TRUE(found);
  size_t got = 0;
  uint8_t b[10];
  TEST_ASSERT_EQUAL_INT((int)FsErr::Ok, (int)fs.read(b, 10, got));
  const std::vector<uint8_t> rd = {0x57, 0xAB, 0x3A, 10, 0};
  found = false;
  for (auto& f : t.L.frames) found = found || f == rd;
  TEST_ASSERT_TRUE(found);
  TEST_ASSERT_EQUAL_INT((int)FsErr::Ok, (int)fs.locate(0x01020304));
  const std::vector<uint8_t> loc = {0x57, 0xAB, 0x39, 4, 3, 2, 1};
  found = false;
  for (auto& f : t.L.frames) found = found || f == loc;
  TEST_ASSERT_TRUE(found);
  TEST_ASSERT_EQUAL_INT((int)FsErr::Ok, (int)fs.close());
  const std::vector<uint8_t> cl = {0x57, 0xAB, 0x36, 0};
  found = false;
  for (auto& f : t.L.frames) found = found || f == cl;
  TEST_ASSERT_TRUE(found);
}

void test_fs_unknown_status_mapped() {
  Tree t;
  Ch376Fs fs(t.L);
  FsKind k;
  t.L.openStatus = 0x77;
  TEST_ASSERT_EQUAL_INT((int)FsErr::ChipStatus, (int)fs.openPath("/BIG.BIN", k));
  TEST_ASSERT_EQUAL_UINT8(0x77, fs.lastStatus());
  TEST_ASSERT_EQUAL_INT((int)FsErr::Disconnected, (int)Ch376Fs::mapStatus(0x16));
  TEST_ASSERT_EQUAL_INT((int)FsErr::ReadError, (int)Ch376Fs::mapStatus(0x1F));
}

void test_fs_status_no_reply() {
  Tree t;
  Ch376Fs fs(t.L);
  FsKind k;
  t.L.silentStatus = true;
  TEST_ASSERT_EQUAL_INT((int)FsErr::NoReply, (int)fs.openPath("/BIG.BIN", k));
}

// ---------------------------------------------------------------- читання

void test_fs_read_exact_n_and_pattern() {
  Tree t;
  Ch376Fs fs(t.L);
  FsKind k;
  fs.openPath("/MUSIC/ALBUM/TRACK01.MP3", k);
  uint8_t b[100];
  size_t got = 0;
  TEST_ASSERT_EQUAL_INT((int)FsErr::Ok, (int)fs.read(b, 100, got));
  TEST_ASSERT_EQUAL_UINT32(100, got);
  TEST_ASSERT_TRUE(patOk(b, t.idTrack, 0, 100));
}

void test_fs_read_across_64_boundaries() {
  Tree t;
  Ch376Fs fs(t.L);
  FsKind k;
  fs.openPath("/BIG.BIN", k);
  TEST_ASSERT_EQUAL_INT((int)FsErr::Ok, (int)fs.locate(60));
  uint8_t b[130];
  size_t got = 0;
  TEST_ASSERT_EQUAL_INT((int)FsErr::Ok, (int)fs.read(b, 130, got));
  TEST_ASSERT_EQUAL_UINT32(130, got);
  TEST_ASSERT_TRUE(patOk(b, t.idBig, 60, 130));
}

void test_fs_read_to_eof_short() {
  Tree t;
  Ch376Fs fs(t.L);
  FsKind k;
  fs.openPath("/ROOT.TXT", k);
  uint8_t b[200];
  size_t got = 0;
  TEST_ASSERT_EQUAL_INT((int)FsErr::Ok, (int)fs.read(b, 200, got));
  TEST_ASSERT_EQUAL_UINT32(50, got);
  TEST_ASSERT_TRUE(patOk(b, t.idRoot, 0, 50));
  TEST_ASSERT_EQUAL_INT((int)FsErr::Ok, (int)fs.read(b, 200, got));
  TEST_ASSERT_EQUAL_UINT32(0, got);
}

void test_fs_read_over_65535_splits_requests() {
  Tree t;
  Ch376Fs fs(t.L);
  FsKind k;
  fs.openPath("/BIG.BIN", k);
  std::vector<uint8_t> b(100000);
  size_t got = 0;
  TEST_ASSERT_EQUAL_INT((int)FsErr::Ok, (int)fs.read(b.data(), b.size(), got));
  TEST_ASSERT_EQUAL_UINT32(100000, got);
  TEST_ASSERT_TRUE(patOk(b.data(), t.idBig, 0, 100000));
  TEST_ASSERT_EQUAL_INT(2, t.L.cmdCount[0x3A]);  // 65535 + 34465
}

void test_fs_locate_start_mid_end_and_read() {
  Tree t;
  Ch376Fs fs(t.L);
  FsKind k;
  fs.openPath("/MUSIC/ALBUM/TRACK01.MP3", k);
  uint8_t b[16];
  size_t got = 0;
  const uint32_t offs[] = {0, 500, 984};
  for (uint32_t o : offs) {
    TEST_ASSERT_EQUAL_INT((int)FsErr::Ok, (int)fs.locate(o));
    TEST_ASSERT_EQUAL_INT((int)FsErr::Ok, (int)fs.read(b, 16, got));
    TEST_ASSERT_EQUAL_UINT32(16, got);
    TEST_ASSERT_TRUE(patOk(b, t.idTrack, o, 16));
  }
  TEST_ASSERT_EQUAL_INT((int)FsErr::Ok, (int)fs.locate(1000));  // кінець
  TEST_ASSERT_EQUAL_INT((int)FsErr::Ok, (int)fs.read(b, 16, got));
  TEST_ASSERT_EQUAL_UINT32(0, got);
}

void test_fs_not_open_errors() {
  Tree t;
  Ch376Fs fs(t.L);
  size_t got = 0;
  uint8_t b[4];
  uint32_t sz;
  TEST_ASSERT_EQUAL_INT((int)FsErr::NotOpen, (int)fs.read(b, 4, got));
  TEST_ASSERT_EQUAL_INT((int)FsErr::NotOpen, (int)fs.locate(0));
  TEST_ASSERT_EQUAL_INT((int)FsErr::NotOpen, (int)fs.fileSize(sz));
  TEST_ASSERT_EQUAL_INT((int)FsErr::Ok, (int)fs.close());
}

// ---------------------------------------------------------------- перелік

void test_fs_list_root_filters_label() {
  Tree t;
  Ch376Fs fs(t.L);
  Names n;
  uint16_t cnt = 0;
  bool more = true;
  TEST_ASSERT_EQUAL_INT((int)FsErr::Ok, (int)fs.list("/", collect, &n, 64, cnt, more));
  TEST_ASSERT_EQUAL_UINT32(4, cnt);  // ROOT.TXT MUSIC BIG.BIN EMPTYD (мітка тому пропущена)
  TEST_ASSERT_FALSE(more);
  TEST_ASSERT_EQUAL_STRING("ROOT.TXT", n.v[0].c_str());
  TEST_ASSERT_EQUAL_UINT32(50, n.sizes[0]);
  TEST_ASSERT_TRUE(n.dirs[1]);
  TEST_ASSERT_EQUAL_STRING("MUSIC", n.v[1].c_str());
  TEST_ASSERT_EQUAL_STRING("BIG.BIN", n.v[2].c_str());
  TEST_ASSERT_EQUAL_UINT32(100000, n.sizes[2]);
}

void test_fs_list_subdir_skips_dots_and_has_inner_dir() {
  Tree t;
  Ch376Fs fs(t.L);
  Names n;
  uint16_t cnt = 0;
  bool more = false;
  TEST_ASSERT_EQUAL_INT((int)FsErr::Ok, (int)fs.list("/MUSIC", collect, &n, 64, cnt, more));
  TEST_ASSERT_EQUAL_UINT32(2, cnt);  // "." і ".." пропущені
  TEST_ASSERT_EQUAL_STRING("ALBUM", n.v[0].c_str());
  TEST_ASSERT_TRUE(n.dirs[0]);
  TEST_ASSERT_EQUAL_STRING("SONG.MP3", n.v[1].c_str());
  TEST_ASSERT_FALSE(n.dirs[1]);
}

void test_fs_list_empty_dir() {
  Tree t;
  Ch376Fs fs(t.L);
  Names n;
  uint16_t cnt = 9;
  bool more = true;
  TEST_ASSERT_EQUAL_INT((int)FsErr::Ok, (int)fs.list("/EMPTYD", collect, &n, 64, cnt, more));
  TEST_ASSERT_EQUAL_UINT32(0, cnt);
  TEST_ASSERT_FALSE(more);
}

void test_fs_list_limit_64() {
  Tree t;
  const int many = t.L.addDir(0, "MANY");
  const int sixty4 = t.L.addDir(0, "SIXTY4");
  char nm[16];
  for (int i = 0; i < 70; i++) {
    snprintf(nm, sizeof nm, "F%03d.TXT", i);
    t.L.addFile(many, nm, 1);
  }
  for (int i = 0; i < 64; i++) {
    snprintf(nm, sizeof nm, "G%03d.TXT", i);
    t.L.addFile(sixty4, nm, 1);
  }
  Ch376Fs fs(t.L);
  Names n;
  uint16_t cnt = 0;
  bool more = false;
  TEST_ASSERT_EQUAL_INT((int)FsErr::Ok, (int)fs.list("/MANY", collect, &n, kCh376DiagLsMax, cnt, more));
  TEST_ASSERT_EQUAL_UINT32(64, cnt);
  TEST_ASSERT_TRUE(more);  // обрізано
  TEST_ASSERT_EQUAL_INT((int)FsErr::Ok, (int)fs.list("/SIXTY4", collect, &n, kCh376DiagLsMax, cnt, more));
  TEST_ASSERT_EQUAL_UINT32(64, cnt);
  TEST_ASSERT_FALSE(more);  // рівно 64 — не обрізано
  TEST_ASSERT_EQUAL_INT((int)FsErr::Ok, (int)fs.list("/MANY", collect, &n, 100, cnt, more));
  TEST_ASSERT_EQUAL_UINT32(70, cnt);
  TEST_ASSERT_FALSE(more);
}

void test_fs_list_errors() {
  Tree t;
  Ch376Fs fs(t.L);
  Names n;
  uint16_t cnt = 0;
  bool more = false;
  TEST_ASSERT_EQUAL_INT((int)FsErr::NotDir, (int)fs.list("/ROOT.TXT", collect, &n, 64, cnt, more));
  TEST_ASSERT_FALSE(fs.fileOpen());
  TEST_ASSERT_EQUAL_INT((int)FsErr::NotFound, (int)fs.list("/NOPE", collect, &n, 64, cnt, more));
  TEST_ASSERT_EQUAL_INT((int)FsErr::BadArg, (int)fs.list("/", nullptr, &n, 64, cnt, more));
}

// ---------------------------------------------------------------- збої

void test_fs_disconnect_mid_read() {
  Tree t;
  Ch376Fs fs(t.L);
  FsKind k;
  fs.openPath("/BIG.BIN", k);
  t.L.ejectAtBlock = 5;
  uint8_t b[1000];
  size_t got = 0;
  TEST_ASSERT_EQUAL_INT((int)FsErr::Disconnected, (int)fs.read(b, 1000, got));
  TEST_ASSERT_EQUAL_UINT32(256, got);  // 4 блоки до зникнення
  TEST_ASSERT_TRUE(patOk(b, t.idBig, 0, 256));
  TEST_ASSERT_FALSE(fs.fileOpen());
  TEST_ASSERT_EQUAL_INT((int)FsErr::Disconnected, (int)fs.openPath("/BIG.BIN", k));
}

void test_fs_block_error_then_recover() {
  Tree t;
  Ch376Fs fs(t.L);
  FsKind k;
  fs.openPath("/BIG.BIN", k);
  t.L.errAtBlock = 3;
  uint8_t b[300];
  size_t got = 0;
  TEST_ASSERT_EQUAL_INT((int)FsErr::ReadError, (int)fs.read(b, 300, got));
  TEST_ASSERT_EQUAL_UINT32(128, got);
  TEST_ASSERT_EQUAL_UINT32(1, fs.stats().errors);
  fs.abort();
  TEST_ASSERT_FALSE(fs.fileOpen());
  TEST_ASSERT_EQUAL_INT((int)FsErr::Ok, (int)fs.openPath("/BIG.BIN", k));  // повторне відкриття після помилки
  TEST_ASSERT_EQUAL_INT((int)FsErr::Ok, (int)fs.locate(128));
  TEST_ASSERT_EQUAL_INT((int)FsErr::Ok, (int)fs.read(b, 172, got));
  TEST_ASSERT_EQUAL_UINT32(172, got);
  TEST_ASSERT_TRUE(patOk(b, t.idBig, 128, 172));
}

void test_fs_truncated_reply_times_out() {
  Tree t;
  Ch376Fs fs(t.L);
  FsKind k;
  fs.openPath("/BIG.BIN", k);
  t.L.truncAtBlock = 2;
  uint8_t b[300];
  size_t got = 0;
  TEST_ASSERT_EQUAL_INT((int)FsErr::Timeout, (int)fs.read(b, 300, got));
  TEST_ASSERT_EQUAL_UINT32(64, got);
}

void test_fs_garbage_length_is_badreply() {
  Tree t;
  Ch376Fs fs(t.L);
  FsKind k;
  fs.openPath("/BIG.BIN", k);
  t.L.badLenAtBlock = 2;
  uint8_t b[300];
  size_t got = 0;
  TEST_ASSERT_EQUAL_INT((int)FsErr::BadReply, (int)fs.read(b, 300, got));
  TEST_ASSERT_EQUAL_UINT32(64, got);
}

// ---------------------------------------------------------------- читач (IFileReader)

void test_rd_open_read_seek_close() {
  Tree t;
  Ch376Fs fs(t.L);
  Ch376FileReader r(fs);
  TEST_ASSERT_TRUE(r.open("/MUSIC/SONG.MP3"));
  TEST_ASSERT_TRUE(r.isOpen());
  TEST_ASSERT_EQUAL_UINT32(300, r.size());
  uint8_t b[100];
  TEST_ASSERT_EQUAL_UINT32(100, r.read(b, 100));
  TEST_ASSERT_TRUE(patOk(b, t.idSong, 0, 100));
  TEST_ASSERT_EQUAL_UINT32(100, r.pos());
  TEST_ASSERT_TRUE(r.seek(250));
  TEST_ASSERT_EQUAL_UINT32(50, r.read(b, 100));  // коротке читання в кінці
  TEST_ASSERT_TRUE(patOk(b, t.idSong, 250, 50));
  TEST_ASSERT_FALSE(r.failed());
  TEST_ASSERT_EQUAL_UINT32(0, r.read(b, 100));
  TEST_ASSERT_FALSE(r.failed());
  r.close();
  TEST_ASSERT_FALSE(r.isOpen());
  TEST_ASSERT_EQUAL_INT(-1, t.L.openFile);
}

void test_rd_open_errors() {
  Tree t;
  Ch376Fs fs(t.L);
  Ch376FileReader r(fs);
  TEST_ASSERT_FALSE(r.open("/MUSIC"));
  TEST_ASSERT_EQUAL_INT((int)FsErr::IsDir, (int)r.lastErr());
  TEST_ASSERT_TRUE(r.failed());
  TEST_ASSERT_FALSE(r.open("/MUSIC/NOFILE.MP3"));
  TEST_ASSERT_EQUAL_INT((int)FsErr::NotFound, (int)r.lastErr());
  TEST_ASSERT_FALSE(r.open("/THIS/PATH/IS/MUCH/TOO/LONG/FOR/THE/READER/BUFFER.MP3"));
  TEST_ASSERT_EQUAL_INT((int)FsErr::BadName, (int)r.lastErr());
  TEST_ASSERT_TRUE(r.open("/BIG.BIN"));  // після помилок усе працює
  TEST_ASSERT_FALSE(r.failed());
}

void test_rd_seek_bounds() {
  Tree t;
  Ch376Fs fs(t.L);
  Ch376FileReader r(fs);
  TEST_ASSERT_TRUE(r.open("/MUSIC/ALBUM/TRACK01.MP3"));
  TEST_ASSERT_TRUE(r.seek(0));
  TEST_ASSERT_TRUE(r.seek(1000));
  uint8_t b[8];
  TEST_ASSERT_EQUAL_UINT32(0, r.read(b, 8));
  TEST_ASSERT_FALSE(r.failed());
  TEST_ASSERT_FALSE(r.seek(1001));  // за межі: false, без збою
  TEST_ASSERT_FALSE(r.failed());
  TEST_ASSERT_TRUE(r.seek(10));
  TEST_ASSERT_EQUAL_UINT32(8, r.read(b, 8));
  TEST_ASSERT_TRUE(patOk(b, t.idTrack, 10, 8));
}

void test_rd_read_discard_null() {
  Tree t;
  Ch376Fs fs(t.L);
  Ch376FileReader r(fs);
  TEST_ASSERT_TRUE(r.open("/BIG.BIN"));
  TEST_ASSERT_EQUAL_UINT32(5000, r.read(nullptr, 5000));
  TEST_ASSERT_EQUAL_UINT32(5000, r.pos());
  uint8_t b[8];
  TEST_ASSERT_EQUAL_UINT32(8, r.read(b, 8));
  TEST_ASSERT_TRUE(patOk(b, t.idBig, 5000, 8));
}

void test_rd_disconnect_clean_state_and_reopen() {
  Tree t;
  Ch376Fs fs(t.L);
  Ch376FileReader r(fs);
  TEST_ASSERT_TRUE(r.open("/BIG.BIN"));
  t.L.ejectAtBlock = 5;
  uint8_t b[1000];
  TEST_ASSERT_EQUAL_UINT32(256, r.read(b, 1000));
  TEST_ASSERT_TRUE(r.failed());
  TEST_ASSERT_FALSE(r.isOpen());
  TEST_ASSERT_EQUAL_INT((int)FsErr::Disconnected, (int)r.lastErr());
  TEST_ASSERT_EQUAL_UINT32(0, r.read(b, 10));
  TEST_ASSERT_FALSE(r.seek(0));
  TEST_ASSERT_EQUAL_UINT32(0, fs.stats().retries);  // відключення не повторюється
  t.L.ejected = false;  // флешку повернули
  t.L.ejectAtBlock = -1;
  TEST_ASSERT_TRUE(r.open("/BIG.BIN"));
  TEST_ASSERT_FALSE(r.failed());
  TEST_ASSERT_EQUAL_UINT32(100, r.read(b, 100));
  TEST_ASSERT_TRUE(patOk(b, t.idBig, 0, 100));
}

void test_rd_retry_recovers_from_block_error() {
  Tree t;
  Ch376Fs fs(t.L);
  Ch376FileReader r(fs);
  TEST_ASSERT_TRUE(r.open("/BIG.BIN"));
  t.L.errAtBlock = 3;
  uint8_t b[500];
  TEST_ASSERT_EQUAL_UINT32(500, r.read(b, 500));
  TEST_ASSERT_FALSE(r.failed());
  TEST_ASSERT_TRUE(patOk(b, t.idBig, 0, 500));
  TEST_ASSERT_EQUAL_UINT32(1, fs.stats().retries);
  TEST_ASSERT_EQUAL_UINT32(500, r.pos());
}

void test_rd_gives_up_after_retries() {
  Tree t;
  Ch376Fs fs(t.L);
  Ch376FileReader r(fs);
  TEST_ASSERT_TRUE(r.open("/BIG.BIN"));
  t.L.silentStatus = true;  // чіп мовчить: ні читання, ні повторне відкриття не вдаються
  uint8_t b[100];
  TEST_ASSERT_EQUAL_UINT32(0, r.read(b, 100));
  TEST_ASSERT_TRUE(r.failed());
  TEST_ASSERT_FALSE(r.isOpen());
  t.L.silentStatus = false;
  TEST_ASSERT_TRUE(r.open("/BIG.BIN"));
}

// ---------------------------------------------------------------- INT / опитування, статистика, бюджет

void test_wait_int_vs_poll() {
  Tree t;
  t.L.latencyUs = 300;
  Ch376Fs fs(t.L);
  Ch376FileReader r(fs);
  TEST_ASSERT_TRUE(r.open("/BIG.BIN"));
  fs.resetStats();
  TEST_ASSERT_EQUAL_UINT32(640, r.read(nullptr, 640));
  TEST_ASSERT_EQUAL_UINT32(0, fs.stats().polls);  // INT: рівно один GET_STATUS на подію
  TEST_ASSERT_TRUE(fs.stats().intWaits >= 10);
  TEST_ASSERT_EQUAL_UINT32(0, fs.stats().intMisses);
  fs.setUseInt(false);
  fs.resetStats();
  TEST_ASSERT_EQUAL_UINT32(640, r.read(nullptr, 640));
  TEST_ASSERT_TRUE(fs.stats().polls > 0);  // опитування: «події ще немає» повторюється
  TEST_ASSERT_EQUAL_UINT32(0, fs.stats().intWaits);
  fs.reset();
  TEST_ASSERT_EQUAL_INT(kCh376FsUseInt ? 1 : 0, fs.useInt() ? 1 : 0);
}

void test_wait_without_int_pin_polls() {
  Tree t;
  t.L.intPin = false;
  t.L.latencyUs = 300;
  Ch376Fs fs(t.L);
  Ch376FileReader r(fs);
  TEST_ASSERT_TRUE(r.open("/BIG.BIN"));
  TEST_ASSERT_EQUAL_UINT32(256, r.read(nullptr, 256));
  TEST_ASSERT_TRUE(fs.stats().polls > 0);
  TEST_ASSERT_EQUAL_UINT32(0, fs.stats().intWaits);
  TEST_ASSERT_FALSE(fs.intUsable());
}

void test_stats_block_times() {
  Tree t;
  t.L.latencyUs = 500;
  Ch376Fs fs(t.L);
  Ch376FileReader r(fs);
  TEST_ASSERT_TRUE(r.open("/MUSIC/ALBUM/TRACK01.MP3"));
  fs.resetStats();
  TEST_ASSERT_EQUAL_UINT32(1000, r.read(nullptr, 1000));
  const Ch376FsStats& s = fs.stats();
  TEST_ASSERT_EQUAL_UINT32(16, s.blocks);  // ceil(1000/64)
  TEST_ASSERT_EQUAL_UINT32(1000, s.bytes);
  TEST_ASSERT_TRUE(s.blkMinUs >= 500);
  TEST_ASSERT_TRUE(s.blkMaxUs >= s.blkMinUs);
  TEST_ASSERT_TRUE(s.blkSumUs >= 16u * 500u);
  TEST_ASSERT_EQUAL_UINT32(0, s.errors);
}

void test_negotiate_list_is_verified_speeds_only() {
  TEST_ASSERT_EQUAL_UINT32(3, kCh376NegotiateN);
  TEST_ASSERT_EQUAL_UINT32(1000000, kCh376NegotiateTable[0].baud);
  TEST_ASSERT_EQUAL_UINT8(0xFA, kCh376NegotiateTable[0].hi);
  TEST_ASSERT_EQUAL_UINT32(460800, kCh376NegotiateTable[1].baud);
  TEST_ASSERT_EQUAL_UINT8(0xF3, kCh376NegotiateTable[1].hi);
  TEST_ASSERT_EQUAL_UINT32(115200, kCh376NegotiateTable[2].baud);
  TEST_ASSERT_EQUAL_UINT8(0xCC, kCh376NegotiateTable[2].hi);
}

void test_static_ram_budget() {
  // Нова статична памʼять при ENABLE_USB_CH376=1 ≤ 0,3 КБ (на ESP32 вказівники 4 Б, тут 8 Б — запас на це).
  TEST_ASSERT_TRUE(sizeof(Ch376Fs) + sizeof(Ch376FileReader) <= 300);
}

void test_fs_raw_open_and_gap() {
  Tree t;
  Ch376Fs fs(t.L);
  uint8_t st = 0, ent[32];
  bool has = true;
  fs.setGapUs(200);
  const uint32_t t0 = t.L.nowV;
  TEST_ASSERT_EQUAL_INT((int)FsErr::Ok, (int)fs.rawOpen("/BIG.BIN", st, ent, has));
  TEST_ASSERT_EQUAL_UINT8(0x14, st);
  TEST_ASSERT_FALSE(has);
  TEST_ASSERT_TRUE(t.L.nowV - t0 >= 200);  // пауза між імʼям і FILE_OPEN витримана
  TEST_ASSERT_EQUAL_INT((int)FsErr::Ok, (int)fs.close());
  TEST_ASSERT_EQUAL_INT((int)FsErr::Ok, (int)fs.rawOpen("/", st, ent, has));
  TEST_ASSERT_EQUAL_UINT8(0x41, st);
  TEST_ASSERT_EQUAL_INT((int)FsErr::Ok, (int)fs.rawOpen("*", st, ent, has));
  TEST_ASSERT_EQUAL_UINT8(0x1D, st);
  TEST_ASSERT_TRUE(has);
  TEST_ASSERT_EQUAL_UINT8('M', ent[0]);  // перший запис — мітка тому (mock)
  fs.reset();
  TEST_ASSERT_EQUAL_UINT32(0, fs.gapUs());
}

void test_fs_size_with_prefix_byte() {
  Tree t;
  t.L.prefixSize = true;
  Ch376Fs fs(t.L);
  FsKind k;
  fs.openPath("/ROOT.TXT", k);
  uint32_t sz = 99;
  TEST_ASSERT_EQUAL_INT((int)FsErr::Ok, (int)fs.fileSize(sz));
  TEST_ASSERT_EQUAL_UINT32(50, sz);
  t.L.prefixSize = false;
  fs.openPath("/BIG.BIN", k);
  TEST_ASSERT_EQUAL_INT((int)FsErr::Ok, (int)fs.fileSize(sz));
  TEST_ASSERT_EQUAL_UINT32(100000, sz);
}

void test_fs_duplicate_status_is_dropped() {
  Tree t;
  t.L.dupStatus = true;
  t.L.byteUs = 10;
  Ch376Fs fs(t.L);
  Ch376FileReader r(fs);
  TEST_ASSERT_TRUE(r.open("/BIG.BIN"));
  uint8_t b[700];
  TEST_ASSERT_EQUAL_UINT32(700, r.read(b, 700));
  TEST_ASSERT_TRUE(patOk(b, t.idBig, 0, 700));
  TEST_ASSERT_EQUAL_UINT32(0, fs.stats().errors);
}

void test_fs_big_blocks_255() {
  Tree t;
  t.L.blockLen = 255;  // як на реальному CH376S
  Ch376Fs fs(t.L);
  Ch376FileReader r(fs);
  TEST_ASSERT_TRUE(r.open("/BIG.BIN"));
  std::vector<uint8_t> b(1045);
  TEST_ASSERT_EQUAL_UINT32(1045, r.read(b.data(), 1045));
  TEST_ASSERT_TRUE(patOk(b.data(), t.idBig, 0, 1045));
  TEST_ASSERT_EQUAL_UINT32(5, fs.stats().blocks);  // 4×255 + 25
  TEST_ASSERT_EQUAL_UINT32(1045, fs.stats().bytes);
  TEST_ASSERT_EQUAL_UINT32(0, fs.stats().errors);
  TEST_ASSERT_TRUE(r.seek(99990));
  TEST_ASSERT_EQUAL_UINT32(10, r.read(b.data(), 500));  // хвіст файлу: блок 10 Б
  TEST_ASSERT_TRUE(patOk(b.data(), t.idBig, 99990, 10));
  TEST_ASSERT_TRUE(r.seek(0));
  TEST_ASSERT_EQUAL_UINT32(700, r.read(nullptr, 700));  // відкидання без буфера
}

void run_fs_tests() {
  RUN_TEST(test_fs_path_root_is_dir);
  RUN_TEST(test_fs_path_by_elements);
  RUN_TEST(test_fs_path_uppercased_and_slashes);
  RUN_TEST(test_fs_open_missing);
  RUN_TEST(test_fs_file_in_middle_is_notdir_and_closed);
  RUN_TEST(test_fs_bad_names_never_touch_chip);
  RUN_TEST(test_fs_frame_bytes);
  RUN_TEST(test_fs_unknown_status_mapped);
  RUN_TEST(test_fs_status_no_reply);
  RUN_TEST(test_fs_read_exact_n_and_pattern);
  RUN_TEST(test_fs_read_across_64_boundaries);
  RUN_TEST(test_fs_read_to_eof_short);
  RUN_TEST(test_fs_read_over_65535_splits_requests);
  RUN_TEST(test_fs_locate_start_mid_end_and_read);
  RUN_TEST(test_fs_not_open_errors);
  RUN_TEST(test_fs_list_root_filters_label);
  RUN_TEST(test_fs_list_subdir_skips_dots_and_has_inner_dir);
  RUN_TEST(test_fs_list_empty_dir);
  RUN_TEST(test_fs_list_limit_64);
  RUN_TEST(test_fs_list_errors);
  RUN_TEST(test_fs_disconnect_mid_read);
  RUN_TEST(test_fs_block_error_then_recover);
  RUN_TEST(test_fs_truncated_reply_times_out);
  RUN_TEST(test_fs_garbage_length_is_badreply);
  RUN_TEST(test_rd_open_read_seek_close);
  RUN_TEST(test_rd_open_errors);
  RUN_TEST(test_rd_seek_bounds);
  RUN_TEST(test_rd_read_discard_null);
  RUN_TEST(test_rd_disconnect_clean_state_and_reopen);
  RUN_TEST(test_rd_retry_recovers_from_block_error);
  RUN_TEST(test_rd_gives_up_after_retries);
  RUN_TEST(test_wait_int_vs_poll);
  RUN_TEST(test_wait_without_int_pin_polls);
  RUN_TEST(test_stats_block_times);
  RUN_TEST(test_negotiate_list_is_verified_speeds_only);
  RUN_TEST(test_static_ram_budget);
  RUN_TEST(test_fs_raw_open_and_gap);
  RUN_TEST(test_fs_size_with_prefix_byte);
  RUN_TEST(test_fs_duplicate_status_is_dropped);
  RUN_TEST(test_fs_big_blocks_255);
}

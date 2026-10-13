#include "media/scanner.h"

#include <Arduino.h>
#include <string.h>
#include <strings.h>

void Scanner::start(IMedia& m, Playlist& pl) {
  abort();
  m_ = &m;
  pl_ = &pl;
  pl.clear();
  stats_ = ScanStats{};
  t0_ = millis();
  depth_ = 0;
  pathLen_ = 0;
  path_[0] = 0;
  int h = m.openDir("/");
  lv_[0] = Level{h, 0, Playlist::kNone, 0, 0};
  st_ = (h < 0) ? St::Failed : St::Running;
}

void Scanner::closeAll() {
  if (!m_) return;
  for (int d = 0; d <= depth_ && d <= kScanMaxDepth; d++) {
    if (lv_[d].h >= 0) m_->closeDir(lv_[d].h);
    lv_[d].h = -1;
  }
}

void Scanner::abort() {
  closeAll();
  st_ = St::Idle;
}

void Scanner::finish() {
  closeAll();
  stats_.wallMs = millis() - t0_;
  stats_.tracks = pl_->tracks();
  stats_.foldersTotal = pl_->folders();
  uint16_t n = 0;
  for (int f = 0; f < pl_->folders(); f++) if (pl_->folderTracks(f)) n++;
  stats_.foldersWithTracks = n;
}

Scanner::St Scanner::step(uint32_t budgetMs) {
  if (st_ != St::Running) return st_;
  uint32_t t = millis();
  do {
    advance();
  } while (st_ == St::Running && (uint32_t)(millis() - t) < budgetMs);
  stats_.busyMs += millis() - t;
  if (st_ != St::Running) finish();
  return st_;
}

void Scanner::popLevel() {
  if (depth_ == 0) { st_ = St::Done; return; }
  depth_--;
  pathLen_ = lv_[depth_].pathLen;
  path_[pathLen_] = 0;
}

// Реєструє поточний каталог і всіх незареєстрованих предків (корінь = "/")
bool Scanner::ensureFolders() {
  for (int d = 0; d <= depth_; d++) {
    Level& X = lv_[d];
    if (X.folder != Playlist::kNone) continue;
    uint16_t parent = d ? lv_[d - 1].folder : Playlist::kNone;
    const char* nm = "/";
    size_t len = 1;
    if (d) {
      nm = path_ + X.nameAt;
      const char* sl = strchr(nm, '/');
      len = sl ? (size_t)(sl - nm) : strlen(nm);
    }
    int f = pl_->addFolder(parent, nm, len);
    if (f < 0) return false;
    X.folder = (uint16_t)f;
  }
  return true;
}

void Scanner::advance() {
  Level& L = lv_[depth_];
  int r = m_->nextEntry(L.h, e_);
  if (r < 0) { st_ = St::Failed; return; }
  if (r == 0) {  // кінець каталогу
    m_->closeDir(L.h);
    L.h = -1;
    if (L.pass == 0) {  // другий прохід: підкаталоги
      int h = m_->openDir(dirPath());
      if (h < 0) {
        if (!m_->mounted()) { st_ = St::Failed; return; }
        stats_.dirErr++;
        popLevel();
        return;
      }
      L.h = h;
      L.pass = 1;
      return;
    }
    popLevel();
    return;
  }
  bool first = (L.pass == 0);  // кожен запис бачимо двічі (файли, потім підкаталоги): лічимо в першому проході
  if (first) stats_.entries++;
  if (!e_.nameOk) { if (first) stats_.longName++; return; }
  if (e_.name[0] == '.' || e_.hidden) { if (first) stats_.hidden++; return; }

  if (L.pass == 0) {
    if (e_.isDir) return;
    size_t n = strlen(e_.name);
    if (n <= 4 || strcasecmp(e_.name + n - 4, ".mp3") != 0) return;
    if (!ensureFolders() || !pl_->addTrack(e_.name, n - 4)) {
      stats_.overflow = true;
      stats_.dropped++;
      st_ = St::Overflow;
    }
    return;
  }

  if (!e_.isDir) return;
  if (depth_ >= kScanMaxDepth) { stats_.deepDirs++; return; }
  size_t n = strlen(e_.name);
  if ((size_t)pathLen_ + 1 + n >= kScanPathMax) { stats_.longName++; return; }
  uint16_t parentLen = pathLen_;
  path_[pathLen_++] = '/';
  memcpy(path_ + pathLen_, e_.name, n);
  pathLen_ += (uint16_t)n;
  path_[pathLen_] = 0;
  int h = m_->openDir(path_);
  if (h < 0) {
    pathLen_ = parentLen;
    path_[pathLen_] = 0;
    if (!m_->mounted()) st_ = St::Failed;
    else stats_.dirErr++;
    return;
  }
  depth_++;
  lv_[depth_] = Level{h, 0, Playlist::kNone, pathLen_, (uint16_t)(parentLen + 1)};
}

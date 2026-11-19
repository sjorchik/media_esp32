#include "media/scanner.h"

#include "util/clock.h"
#include <string.h>
#include <strings.h>

#if ENABLE_USB_CH376
#include <new>
#endif

Scanner::Level Scanner::mkLevel(int h, uint16_t pathLen, uint16_t nameAt) const {
#if ENABLE_USB_CH376
  return Level{h, 0, Playlist::kNone, pathLen, nameAt, poolTop_, poolTop_, poolTop_, false};
#else
  return Level{h, 0, Playlist::kNone, pathLen, nameAt};
#endif
}

void Scanner::start(IMedia& m, Playlist& pl) {
  abort();
  m_ = &m;
  pl_ = &pl;
  pl.clear();
  stats_ = ScanStats{};
  t0_ = nowMs();
  depth_ = 0;
  pathLen_ = 0;
  path_[0] = 0;
#if ENABLE_USB_CH376
  poolTop_ = 0;
  if (onePass_) pool_ = new (std::nothrow) uint8_t[kUsbScanSubPoolBytes];  // nullptr → кожен каталог перечитується (як раніше)
#endif
  int h = m.openDir("/");
  lv_[0] = mkLevel(h, 0, 0);
  st_ = (h < 0) ? St::Failed : St::Running;
}

void Scanner::closeAll() {
  if (!m_) return;
  for (int d = 0; d <= depth_ && d <= kScanMaxDepth; d++) {
    if (lv_[d].h >= 0) m_->closeDir(lv_[d].h);
    lv_[d].h = -1;
  }
}

#if ENABLE_USB_CH376
void Scanner::freePool() {
  delete[] pool_;
  pool_ = nullptr;
  poolTop_ = 0;
}

// Додає імʼя підкаталогу в пул (стек: імена рівня йдуть підряд, глибші рівні — після них). false — не влізло.
bool Scanner::poolPush(Level& L, const char* name, size_t n) {
  if (!pool_ || n == 0 || n > 255 || (size_t)poolTop_ + 1 + n > kUsbScanSubPoolBytes) {
    L.poolFull = true;
    return false;
  }
  pool_[poolTop_++] = (uint8_t)n;
  memcpy(pool_ + poolTop_, name, n);
  poolTop_ = (uint16_t)(poolTop_ + n);
  L.subEnd = poolTop_;
  return true;
}
#endif

void Scanner::abort() {
  closeAll();
#if ENABLE_USB_CH376
  freePool();
#endif
  st_ = St::Idle;
}

void Scanner::finish() {
  closeAll();
#if ENABLE_USB_CH376
  freePool();
#endif
  stats_.wallMs = nowMs() - t0_;
  stats_.tracks = pl_->tracks();
  stats_.foldersTotal = pl_->folders();
  uint16_t n = 0;
  for (int f = 0; f < pl_->folders(); f++) if (pl_->folderTracks(f)) n++;
  stats_.foldersWithTracks = n;
}

Scanner::St Scanner::step(uint32_t budgetMs) {
  if (st_ != St::Running) return st_;
  uint32_t t = nowMs();
  do {
#if ENABLE_USB_CH376
    const uint32_t a0 = nowMs();
    advance();
    const uint32_t ad = nowMs() - a0;
    if (ad > stats_.advMaxMs) stats_.advMaxMs = ad;
#else
    advance();
#endif
  } while (st_ == St::Running && (uint32_t)(nowMs() - t) < budgetMs);
  stats_.busyMs += nowMs() - t;
  if (st_ != St::Running) finish();
  return st_;
}

void Scanner::popLevel() {
#if ENABLE_USB_CH376
  poolTop_ = lv_[depth_].subBeg;  // імена цього рівня (і глибших) більше не потрібні
#endif
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

// Зайти в підкаталог name (довжина n): глибина, довжина шляху, openDir. Правила й лічильники ті самі, що в другому проході.
bool Scanner::descend(const char* name, size_t n) {
  if (depth_ >= kScanMaxDepth) { stats_.deepDirs++; return false; }
  if ((size_t)pathLen_ + 1 + n >= kScanPathMax) { stats_.longName++; return false; }
  uint16_t parentLen = pathLen_;
  path_[pathLen_++] = '/';
  memcpy(path_ + pathLen_, name, n);
  pathLen_ += (uint16_t)n;
  path_[pathLen_] = 0;
  int h = m_->openDir(path_);
  if (h < 0) {
    pathLen_ = parentLen;
    path_[pathLen_] = 0;
    if (!m_->mounted()) st_ = St::Failed;
    else stats_.dirErr++;
    return false;
  }
  depth_++;
  lv_[depth_] = mkLevel(h, pathLen_, (uint16_t)(parentLen + 1));
  return true;
}

void Scanner::advance() {
  Level& L = lv_[depth_];
#if ENABLE_USB_CH376
  if (L.pass == 1 && L.h < 0) {  // один прохід: підкаталоги беремо з пулу, каталог не перечитуємо
    if (L.subPos >= L.subEnd) { popLevel(); return; }
    const size_t n = pool_[L.subPos];  // n ≤ 255 < kMediaNameMax: беремо імʼя в e_.name (запис каталогу зараз не потрібен)
    memcpy(e_.name, pool_ + L.subPos + 1, n);
    e_.name[n] = 0;
    L.subPos = (uint16_t)(L.subPos + 1 + n);
    descend(e_.name, n);
    return;
  }
#endif
  int r = m_->nextEntry(L.h, e_);
  if (r < 0) { st_ = St::Failed; return; }
  if (r == 0) {  // кінець каталогу
    m_->closeDir(L.h);
    L.h = -1;
    if (L.pass == 0) {
#if ENABLE_USB_CH376
      if (onePass_ && !L.poolFull) {  // усі імена підкаталогів у пулі: другого проходу нема
        L.pass = 1;
        L.subPos = L.subBeg;
        return;
      }
      if (onePass_) stats_.reread++;
      L.subEnd = L.subBeg;  // неповний список не використовуємо: поточний прохід перечитає каталог
      poolTop_ = L.subBeg;
#endif
      int h = m_->openDir(dirPath());  // другий прохід: підкаталоги
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
    if (e_.isDir) {
#if ENABLE_USB_CH376
      if (onePass_ && !L.poolFull) poolPush(L, e_.name, strlen(e_.name));
#endif
      return;
    }
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
  descend(e_.name, strlen(e_.name));
}

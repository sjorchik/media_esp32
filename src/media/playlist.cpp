#include "media/playlist.h"

#include <string.h>

namespace {

alignas(4) uint8_t s_arena[kPlaylistArenaBytes];
static_assert(kPlaylistArenaBytes % 4 == 0 && kPlaylistArenaBytes <= 65535, "арена: кратна 4 і ≤ 65535");

struct Rec {
  uint16_t parent, nameOff, first, count;
};
constexpr size_t kRec = sizeof(Rec);
static_assert(kRec == 8, "Rec має бути 8 байт");

inline Rec getRec(int f) {
  Rec r;
  memcpy(&r, s_arena + kPlaylistArenaBytes - (size_t)(f + 1) * kRec, kRec);
  return r;
}
inline void putRec(int f, const Rec& r) { memcpy(s_arena + kPlaylistArenaBytes - (size_t)(f + 1) * kRec, &r, kRec); }

// Копіює рядок у out (з NUL); false, якщо не влізло
bool copyStr(const char* s, char* out, size_t cap) {
  size_t n = strlen(s);
  if (!out || n + 1 > cap) return false;
  memcpy(out, s, n + 1);
  return true;
}

// Чи шлях папки f дорівнює dir[0..len) ("" = корінь, "/A/B"). Іде від листа до кореня, без буфера.
bool dirMatches(int f, const char* dir, size_t len) {
  size_t end = len;
  int cur = f;
  for (int guard = 0; guard < 16; guard++) {
    Rec r = getRec(cur);
    if (r.parent == Playlist::kNone) return end == 0;  // корінь у шлях не входить
    const char* nm = (const char*)s_arena + r.nameOff;
    size_t l = strlen(nm);
    if (end < l + 1 || dir[end - 1 - l] != '/' || memcmp(dir + end - l, nm, l) != 0) return false;
    end -= l + 1;
    cur = r.parent;
  }
  return false;
}

}  // namespace

void Playlist::clear() {
  nTracks_ = nFolders_ = 0;
  pool_ = 0;
}

// Вільне місце з урахуванням резерву kPermBytesPerTrack на кожен уже доданий трек
size_t Playlist::freeBytes() const {
  return kPlaylistArenaBytes - pool_ - (size_t)nFolders_ * kRec - (size_t)nTracks_ * kPermBytesPerTrack;
}

uint16_t* Playlist::permBuf() {
  if (!nTracks_) return nullptr;
  size_t off = kPlaylistArenaBytes - (size_t)nFolders_ * kRec - (size_t)nTracks_ * kPermBytesPerTrack;  // парне
  return reinterpret_cast<uint16_t*>(s_arena + off);
}

int Playlist::addFolder(uint16_t parent, const char* name, size_t len) {
  if (nFolders_ >= kNone - 1 || freeBytes() < kRec + len + 1) return -1;
  memcpy(s_arena + pool_, name, len);
  s_arena[pool_ + len] = 0;
  Rec r{parent, (uint16_t)pool_, nTracks_, 0};
  putRec(nFolders_, r);
  pool_ += len + 1;
  return nFolders_++;
}

bool Playlist::addTrack(const char* stem, size_t len) {
  if (!nFolders_ || nTracks_ >= kNone - 1 || freeBytes() < len + 1 + kPermBytesPerTrack) return false;
  memcpy(s_arena + pool_, stem, len);
  s_arena[pool_ + len] = 0;
  pool_ += len + 1;
  Rec r = getRec(nFolders_ - 1);
  r.count++;
  putRec(nFolders_ - 1, r);
  nTracks_++;
  return true;
}

int Playlist::folderOf(uint16_t t) const {
  if (t >= nTracks_) return -1;
  int lo = 0, hi = (int)nFolders_ - 1, ans = -1;
  while (lo <= hi) {  // остання папка з first ≤ t (серед рівних first — «справжня», вона додається останньою)
    int mid = (lo + hi) / 2;
    if (getRec(mid).first <= t) { ans = mid; lo = mid + 1; }
    else hi = mid - 1;
  }
  if (ans < 0) return -1;
  Rec r = getRec(ans);
  return (t < (uint32_t)r.first + r.count) ? ans : -1;
}

uint16_t Playlist::folderFirst(int f) const { return (f >= 0 && f < nFolders_) ? getRec(f).first : 0; }
uint16_t Playlist::folderTracks(int f) const { return (f >= 0 && f < nFolders_) ? getRec(f).count : 0; }

bool Playlist::folderName(int f, char* out, size_t cap) const {
  if (f < 0 || f >= nFolders_) return false;
  return copyStr((const char*)s_arena + getRec(f).nameOff, out, cap);
}

bool Playlist::folderPath(int f, char* out, size_t cap) const {
  if (f < 0 || f >= nFolders_ || !out || cap < 2) return false;
  const char* part[8];
  int n = 0;
  for (int cur = f; cur != kNone;) {
    Rec r = getRec(cur);
    if (r.parent == kNone) break;  // корінь у шлях не входить
    if (n >= 8) return false;
    part[n++] = (const char*)s_arena + r.nameOff;
    cur = r.parent;
  }
  size_t w = 0;
  if (n == 0) { out[0] = '/'; out[1] = 0; return true; }
  for (int i = n - 1; i >= 0; i--) {
    size_t l = strlen(part[i]);
    if (w + 1 + l + 1 > cap) return false;
    out[w++] = '/';
    memcpy(out + w, part[i], l);
    w += l;
  }
  out[w] = 0;
  return true;
}

bool Playlist::trackName(uint16_t i, char* out, size_t cap) const {
  int f = folderOf(i);
  if (f < 0) return false;
  Rec r = getRec(f);
  const char* p = (const char*)s_arena + r.nameOff;
  p += strlen(p) + 1;  // пропустити імʼя папки
  for (uint16_t k = i - r.first; k; k--) p += strlen(p) + 1;
  return copyStr(p, out, cap);
}

bool Playlist::trackPath(uint16_t i, char* out, size_t cap) const {
  int f = folderOf(i);
  if (f < 0 || !folderPath(f, out, cap)) return false;
  size_t w = strlen(out);
  if (w > 1) out[w++] = '/';  // для кореня "/" вже є
  char nm[kMediaNameMax];
  if (!trackName(i, nm, sizeof nm)) return false;
  size_t l = strlen(nm);
  if (w + l + 5 > cap) return false;
  memcpy(out + w, nm, l);
  memcpy(out + w + l, ".mp3", 5);
  return true;
}

uint16_t Playlist::indexOfPath(const char* path) const {
  if (!path || path[0] != '/' || !nTracks_) return kNone;
  const size_t n = strlen(path);
  if (n < 6 || memcmp(path + n - 4, ".mp3", 4) != 0) return kNone;  // "/" + хоч 1 символ + ".mp3"
  size_t slash = n;
  while (path[--slash] != '/') {}                                     // path[0] == '/', тож зупиниться
  const size_t dirLen = slash;                                        // 0 для кореня
  const char* stem = path + slash + 1;
  const size_t stemLen = n - 4 - (slash + 1);
  if (!stemLen) return kNone;
  for (int f = 0; f < nFolders_; f++) {
    Rec r = getRec(f);
    if (!r.count || !dirMatches(f, path, dirLen)) continue;
    const char* p = (const char*)s_arena + r.nameOff;
    p += strlen(p) + 1;  // пропустити імʼя папки
    for (uint16_t k = 0; k < r.count; k++) {
      size_t l = strlen(p);
      if (l == stemLen && memcmp(p, stem, l) == 0) return (uint16_t)(r.first + k);
      p += l + 1;
    }
  }
  return kNone;
}

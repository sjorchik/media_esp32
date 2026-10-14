#include "media/id3.h"

#include <string.h>

namespace Id3 {
namespace {

// cp1251 0x80..0xBF → Unicode (0 = не визначено). 0xC0..0xFF = U+0410..U+044F.
const uint16_t kCp1251Hi[64] = {
    0x0402, 0x0403, 0x201A, 0x0453, 0x201E, 0x2026, 0x2020, 0x2021, 0x20AC, 0x2030, 0x0409, 0x2039, 0x040A, 0x040C, 0x040B, 0x040F,
    0x0452, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014, 0x0000, 0x2122, 0x0459, 0x203A, 0x045A, 0x045C, 0x045B, 0x045F,
    0x00A0, 0x040E, 0x045E, 0x0408, 0x00A4, 0x0490, 0x00A6, 0x00A7, 0x0401, 0x00A9, 0x0404, 0x00AB, 0x00AC, 0x00AD, 0x00AE, 0x0407,
    0x00B0, 0x00B1, 0x0406, 0x0456, 0x0491, 0x00B5, 0x00B6, 0x00B7, 0x0451, 0x2116, 0x0454, 0x00BB, 0x0458, 0x0405, 0x0455, 0x0457};

inline uint32_t cp1251(uint8_t b) {
  if (b < 0x80) return b;
  if (b >= 0xC0) return 0x0410u + (b - 0xC0u);
  uint16_t c = kCp1251Hi[b - 0x80];
  return c ? c : '?';
}

// Писач UTF-8 з обмеженням: символ або влізає цілком, або запис зупиняється (обрізання по межі символу).
struct W {
  char* o;
  size_t cap, n;
  bool full;
  W(char* out, size_t c) : o(out), cap(c), n(0), full(false) {
    if (cap) o[0] = 0;
  }
  void put(uint32_t cp) {
    if (full || !cap || cp == 0xFEFF) return;           // BOM/ZWNBSP відкидаємо
    if (cp < 0x20 || (cp >= 0x7F && cp <= 0x9F)) cp = ' ';  // керуючі → пробіл
    char b[4];
    size_t l;
    if (cp < 0x80) { b[0] = (char)cp; l = 1; }
    else if (cp < 0x800) { b[0] = (char)(0xC0 | (cp >> 6)); b[1] = (char)(0x80 | (cp & 0x3F)); l = 2; }
    else if (cp < 0x10000) {
      b[0] = (char)(0xE0 | (cp >> 12)); b[1] = (char)(0x80 | ((cp >> 6) & 0x3F)); b[2] = (char)(0x80 | (cp & 0x3F)); l = 3;
    } else {
      b[0] = (char)(0xF0 | ((cp >> 18) & 0x07)); b[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
      b[2] = (char)(0x80 | ((cp >> 6) & 0x3F)); b[3] = (char)(0x80 | (cp & 0x3F)); l = 4;
    }
    if (n + l + 1 > cap) { full = true; return; }
    memcpy(o + n, b, l);
    n += l;
  }
  size_t finish() {
    if (!cap) return 0;
    while (n && o[n - 1] == ' ') n--;
    o[n] = 0;
    return n;
  }
};

// Строгий UTF-8. Повертає довжину (1..4), 0 = некоректно; inc=true, якщо коректний лише префікс і дані скінчились.
size_t dec8(const uint8_t* p, size_t n, uint32_t& cp, bool& inc) {
  inc = false;
  uint8_t c = p[0];
  if (c < 0x80) { cp = c; return 1; }
  size_t len;
  uint8_t lo = 0x80, hi = 0xBF;
  uint32_t v;
  if (c >= 0xC2 && c <= 0xDF) { len = 2; v = c & 0x1F; }
  else if (c >= 0xE0 && c <= 0xEF) { len = 3; v = c & 0x0F; if (c == 0xE0) lo = 0xA0; if (c == 0xED) hi = 0x9F; }
  else if (c >= 0xF0 && c <= 0xF4) { len = 4; v = c & 0x07; if (c == 0xF0) lo = 0x90; if (c == 0xF4) hi = 0x8F; }
  else return 0;
  for (size_t k = 1; k < len; k++) {
    if (k >= n) { inc = true; return 0; }
    uint8_t d = p[k];
    uint8_t l = k == 1 ? lo : 0x80, h = k == 1 ? hi : 0xBF;
    if (d < l || d > h) return 0;
    v = (v << 6) | (d & 0x3F);
  }
  cp = v;
  return len;
}

// Усе коректно (ASCII теж); tailOk — допустити неповну послідовність наприкінці. okLen — довжина коректної частини.
bool validUtf8(const uint8_t* p, size_t n, bool tailOk, size_t& okLen, bool& multi) {
  multi = false;
  size_t i = 0;
  while (i < n) {
    if (p[i] < 0x80) { i++; continue; }
    uint32_t cp;
    bool inc;
    size_t l = dec8(p + i, n - i, cp, inc);
    if (!l) {
      if (inc && tailOk) break;
      okLen = i;
      return false;
    }
    multi = true;
    i += l;
  }
  okLen = i;
  return true;
}

size_t strEnd8(const uint8_t* p, size_t n) {  // довжина до першого NUL
  size_t i = 0;
  while (i < n && p[i]) i++;
  return i;
}

void putUtf8(W& w, const uint8_t* p, size_t n, bool lossy) {
  size_t i = 0;
  while (i < n && !w.full) {
    uint32_t cp;
    bool inc;
    size_t l = dec8(p + i, n - i, cp, inc);
    if (!l) {
      if (lossy) w.put('?');
      i++;
      continue;
    }
    w.put(cp);
    i += l;
  }
}

void putUtf16(W& w, const uint8_t* p, size_t n, int mode) {  // mode: 0 = за BOM, 1 = LE, 2 = BE
  size_t i = 0;
  bool be = mode == 2;
  if (mode == 0 && n >= 2) {
    if (p[0] == 0xFF && p[1] == 0xFE) { be = false; i = 2; }
    else if (p[0] == 0xFE && p[1] == 0xFF) { be = true; i = 2; }
    else be = (p[0] == 0 && p[1] != 0);  // без BOM: нульовий старший байт першим → BE
  }
  auto rd16 = [&](size_t k) -> uint32_t { return be ? ((uint32_t)p[k] << 8) | p[k + 1] : ((uint32_t)p[k + 1] << 8) | p[k]; };
  while (i + 1 < n && !w.full) {
    uint32_t u = rd16(i);
    i += 2;
    if (u == 0) break;  // перше значення
    if (u >= 0xD800 && u <= 0xDBFF) {
      if (i + 1 < n) {
        uint32_t lo = rd16(i);
        if (lo >= 0xDC00 && lo <= 0xDFFF) {
          w.put(0x10000u + ((u - 0xD800u) << 10) + (lo - 0xDC00u));
          i += 2;
          continue;
        }
      }
      w.put('?');
    } else if (u >= 0xDC00 && u <= 0xDFFF) {
      w.put('?');
    } else {
      w.put(u);
    }
  }
}

// ---- потік тегу: вікно читання, зняття unsynchronisation, ліміт прочитаних байтів ----
class Stream {
 public:
  Stream(IFileReader& r, uint8_t* win, size_t winCap, uint32_t budget) : rd_(r), win_(win), winCap_(winCap), budget_(budget) {}
  void begin(uint32_t rawStart, uint32_t rawEnd, bool unsync) {
    raw_ = rawStart;
    end_ = rawEnd;
    unsync_ = unsync;
  }
  int get() {
    for (;;) {
      if (raw_ >= end_) return -1;
      if (raw_ < winPos_ || raw_ >= winPos_ + winLen_) {
        if (!fill()) return -1;
      }
      uint8_t b = win_[raw_ - winPos_];
      raw_++;
      if (unsync_) {
        if (prevFF_ && b == 0) { prevFF_ = false; continue; }  // FF 00 → FF
        prevFF_ = (b == 0xFF);
      }
      return b;
    }
  }
  bool read(uint8_t* dst, size_t n) {
    for (size_t i = 0; i < n; i++) {
      int c = get();
      if (c < 0) return false;
      dst[i] = (uint8_t)c;
    }
    return true;
  }
  // Пропуск n логічних байтів: без unsync — лише зсув позиції (seek при наступному читанні).
  bool skip(uint32_t n) {
    if (!unsync_) {
      if (n > end_ - raw_) { raw_ = end_; return false; }
      raw_ += n;
      return true;
    }
    for (uint32_t i = 0; i < n; i++)
      if (get() < 0) return false;
    return true;
  }
  uint32_t remainingRaw() const { return end_ - raw_; }
  uint32_t readBytes() const { return readBytes_; }
  bool cut() const { return cut_; }

 private:
  bool fill() {
    if (readBytes_ >= budget_) { cut_ = true; return false; }
    uint32_t left = end_ - raw_;
    size_t want = left < winCap_ ? left : winCap_;
    if (rd_.pos() != raw_ && !rd_.seek(raw_)) return false;
    size_t n = rd_.read(win_, want);
    if (n == 0) return false;
    winPos_ = raw_;
    winLen_ = n;
    readBytes_ += (uint32_t)n;
    return true;
  }
  IFileReader& rd_;
  uint8_t* win_;
  size_t winCap_;
  uint32_t budget_;
  uint32_t raw_ = 0, end_ = 0, winPos_ = 0;
  size_t winLen_ = 0;
  uint32_t readBytes_ = 0;
  bool unsync_ = false, prevFF_ = false, cut_ = false;
};

inline uint32_t be32(const uint8_t* p) {
  return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

int slotOf(uint8_t ver, const uint8_t* id) {  // 0 виконавець, 1 назва, 2 альбом
  if (ver == 2) {
    if (!memcmp(id, "TP1", 3)) return 0;
    if (!memcmp(id, "TT2", 3)) return 1;
    if (!memcmp(id, "TAL", 3)) return 2;
  } else {
    if (!memcmp(id, "TPE1", 4)) return 0;
    if (!memcmp(id, "TIT2", 4)) return 1;
    if (!memcmp(id, "TALB", 4)) return 2;
  }
  return -1;
}

char* fieldOf(Tags& t, int s) { return s == 0 ? t.artist : s == 1 ? t.title : t.album; }

void note(Info& inf, int slot, uint8_t src, uint8_t enc, const uint8_t* raw, size_t n) {
  FieldInfo& f = inf.f[slot];
  f.src = src;
  f.enc = enc;
  f.rawN = (uint8_t)(n < kId3DbgRaw ? n : kId3DbgRaw);
  memcpy(f.raw, raw, f.rawN);
}

// ID3v2. scratch: [0, kId3FrameBuf) — буфер кадру, далі вікно читання. true = заголовок v2 коректний.
bool parseV2(IFileReader& rd, const uint8_t h[10], uint8_t* scratch, Tags& t, Info& inf) {
  if (memcmp(h, "ID3", 3) != 0) return false;
  uint8_t ver = h[3], fl = h[5];
  if (ver < 2 || ver > 4 || h[4] == 0xFF || ((h[6] | h[7] | h[8] | h[9]) & 0x80)) return false;
  uint32_t sz = syncsafe32(h + 6);
  uint32_t end = 10 + sz;
  uint32_t fsize = rd.size();
  if (end > fsize) end = fsize;  // обрізаний тег
  inf.ver = ver;
  inf.tagBytes = sz;
  if (ver == 2 && (fl & 0x40)) return true;  // стиснений тег v2.2 — не читаємо

  uint8_t* fb = scratch;
  Stream s(rd, scratch + kId3FrameBuf, kId3WinBytes, kId3MaxReadBytes);
  s.begin(10, end, ver <= 3 && (fl & 0x80));  // unsync на рівні тегу: v2.2/2.3; у 2.4 — на рівні кадру

  if (ver >= 3 && (fl & 0x40)) {  // extended header: пропуск
    uint8_t e[4];
    if (!s.read(e, 4)) return true;
    uint32_t es;
    if (ver == 3) es = be32(e);  // розмір без самого поля
    else {
      if ((e[0] | e[1] | e[2] | e[3]) & 0x80) return true;
      es = syncsafe32(e);
      if (es < 4) return true;
      es -= 4;  // у 2.4 розмір включає поле
    }
    if (es > sz || !s.skip(es)) return true;
  }

  const size_t idLen = ver == 2 ? 3 : 4, hdrLen = ver == 2 ? 6 : 10;
  bool seen[3] = {false, false, false};
  for (uint16_t k = 0; k < kId3MaxFrames; k++) {
    uint8_t fh[10];
    if (!s.read(fh, hdrLen) || fh[0] == 0) break;  // кінець даних або падінг
    bool idOk = true;
    for (size_t i = 0; i < idLen; i++)
      if (!((fh[i] >= 'A' && fh[i] <= 'Z') || (fh[i] >= '0' && fh[i] <= '9'))) idOk = false;
    if (!idOk) break;
    uint32_t size;
    uint8_t f1 = 0;
    if (ver == 2) size = ((uint32_t)fh[3] << 16) | ((uint32_t)fh[4] << 8) | fh[5];
    else if (ver == 3) { size = be32(fh + 4); f1 = fh[9]; }
    else {
      if ((fh[4] | fh[5] | fh[6] | fh[7]) & 0x80) break;
      size = syncsafe32(fh + 4);
      f1 = fh[9];
    }
    inf.frames++;
    if (!(ver <= 3 && (fl & 0x80)) && size > s.remainingRaw()) break;  // кадр виходить за тег (без unsync розмір сирий)

    int slot = slotOf(ver, fh);
    uint32_t extra = 0;
    bool fUnsync = false, want = slot >= 0 && !seen[slot];
    if (want) {
      if (ver == 3) { if (f1 & 0xC0) want = false; if (f1 & 0x20) extra += 1; }                       // стиснення/шифр.; групування
      else if (ver == 4) { if (f1 & 0x0C) want = false; if (f1 & 0x40) extra += 1; if (f1 & 0x01) extra += 4; fUnsync = (f1 & 0x02) != 0; }
      if (size < extra + 1) want = false;
    }
    if (!want) {
      if (!s.skip(size)) break;
      continue;
    }
    uint8_t ex[5];
    if (extra && !s.read(ex, extra)) break;
    uint32_t body = size - extra;
    size_t take = body < kId3FrameBuf ? body : kId3FrameBuf;
    if (!s.read(fb, take)) break;
    bool skipOk = body <= take || s.skip(body - (uint32_t)take);  // решту (APIC тощо) пропускаємо seek'ом
    size_t n = take;
    if (fUnsync) n = unsync(fb, n);
    seen[slot] = true;
    if (n >= 1) {
      char* out = fieldOf(t, slot);
      decodeText(fb[0], fb + 1, n - 1, out, kEvMetaFieldMax + 1, body > take);
      note(inf, slot, 2, fb[0], fb + 1, n - 1);
    }
    if (!skipOk || (seen[0] && seen[1] && seen[2])) break;
  }
  inf.readBytes += s.readBytes();
  inf.cut = s.cut();
  return true;
}

// ID3v1: останні 128 байтів. Заповнює лише порожні поля. true = «TAG» знайдено.
bool parseV1(IFileReader& rd, uint8_t* scratch, Tags& t, Info& inf) {
  uint32_t fsize = rd.size();
  if (fsize < 128 || !rd.seek(fsize - 128)) return false;
  if (rd.read(scratch, 128) != 128) return false;
  inf.readBytes += 128;
  if (memcmp(scratch, "TAG", 3) != 0) return false;
  static const uint8_t off[3] = {33, 3, 63};  // виконавець, назва, альбом
  for (int s = 0; s < 3; s++) {
    char* out = fieldOf(t, s);
    if (out[0]) continue;
    const uint8_t* p = scratch + off[s];
    size_t len = strEnd8(p, 30);
    decodeLegacy(p, len, out, kEvMetaFieldMax + 1, len == 30);
    if (out[0]) note(inf, s, 1, 0, p, len);
  }
  return true;
}

}  // namespace

// ============================================================ відкриті чисті функції

uint32_t syncsafe32(const uint8_t* p) {
  return ((uint32_t)(p[0] & 0x7F) << 21) | ((uint32_t)(p[1] & 0x7F) << 14) | ((uint32_t)(p[2] & 0x7F) << 7) | (p[3] & 0x7F);
}

size_t unsync(uint8_t* p, size_t n) {
  size_t w = 0;
  for (size_t i = 0; i < n; i++) {
    uint8_t b = p[i];
    p[w++] = b;
    if (b == 0xFF && i + 1 < n && p[i + 1] == 0) i++;
  }
  return w;
}

bool looksUtf8(const uint8_t* p, size_t n, bool tailCut) {
  size_t ok;
  bool multi;
  return validUtf8(p, n, tailCut, ok, multi) && multi;
}

bool looksCp1251(const uint8_t* p, size_t n) {
  size_t hi = 0, tot = 0;
  bool c1 = false;
  for (size_t i = 0; i < n; i++) {
    uint8_t b = p[i];
    if (b <= 0x20) continue;  // пробіли й керуючі не рахуємо
    tot++;
    if (b >= 0x80) {
      hi++;
      if (b <= 0x9F) c1 = true;
    }
  }
  if (!hi) return false;
  if (kId3C1ForcesCp1251 && c1) return true;
  return hi * 100 >= tot * (size_t)kId3Cp1251Pct;
}

size_t decodeLegacy(const uint8_t* p, size_t n, char* out, size_t cap, bool tailCut) {
  W w(out, cap);
  size_t ok;
  bool multi;
  if (validUtf8(p, n, tailCut, ok, multi) && multi) putUtf8(w, p, ok, true);          // а) UTF-8 у «неправильному» полі
  else if (looksCp1251(p, n)) { for (size_t i = 0; i < n && !w.full; i++) w.put(cp1251(p[i])); }  // б) cp1251
  else { for (size_t i = 0; i < n && !w.full; i++) w.put(p[i]); }                      // в) Latin1
  return w.finish();
}

size_t decodeText(uint8_t enc, const uint8_t* p, size_t n, char* out, size_t cap, bool cut) {
  W w(out, cap);
  if (enc == 1 || enc == 2) {
    putUtf16(w, p, n, enc == 2 ? 2 : 0);
  } else {
    n = strEnd8(p, n);
    size_t ok;
    bool multi;
    if (enc == 3 && validUtf8(p, n, cut, ok, multi)) putUtf8(w, p, ok, true);
    else return decodeLegacy(p, n, out, cap, cut);  // enc 0, невідомий, або «UTF-8» із некоректними байтами
  }
  return w.finish();
}

uint8_t parse(IFileReader& rd, uint8_t* scratch, size_t cap, Tags& out, Info* info) {
  out.clear();
  Info local;
  if (!info) info = &local;
  memset(info, 0, sizeof *info);
  if (!scratch || cap < kId3ScratchMin) return 0;
  uint8_t mask = 0;
  uint8_t h[10];
  if (rd.size() >= 10 && rd.seek(0) && rd.read(h, 10) == 10) {
    info->readBytes = 10;
    if (parseV2(rd, h, scratch, out, *info)) mask |= kFoundV2;
  }
  if (!out.title[0] && parseV1(rd, scratch, out, *info)) mask |= kFoundV1;
  return mask;
}

}  // namespace Id3

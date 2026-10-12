#include "proto/frame.h"

#include <stdio.h>
#include <string.h>

namespace proto {

namespace {

const char kHex[] = "0123456789ABCDEF";

int hexVal(char c, bool allowLower) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  if (allowLower && c >= 'a' && c <= 'f') return c - 'a' + 10;
  return -1;
}

// Довжина UTF-8-символу; при некоректній послідовності — 1 байт.
size_t utf8CharLen(const char* s, size_t remain) {
  uint8_t c = (uint8_t)s[0];
  size_t n = 1;
  if (c >= 0xC2 && c < 0xE0) n = 2;
  else if (c >= 0xE0 && c < 0xF0) n = 3;
  else if (c >= 0xF0 && c < 0xF5) n = 4;
  if (n > remain) return 1;
  for (size_t i = 1; i < n; i++)
    if (((uint8_t)s[i] & 0xC0) != 0x80) return 1;
  return n;
}

// Екранує in в out (out може бути nullptr — лише вимір). Не більше limit байтів,
// обрізання тільки по межі символу/escape-послідовності. Повертає записану довжину.
size_t escapeImpl(const char* in, char* out, size_t limit) {
  size_t inLen = strlen(in), i = 0, w = 0;
  while (i < inLen) {
    uint8_t c = (uint8_t)in[i];
    bool special = c < 0x20 || c == '%' || c == ',' || c == '*' || c == '$';
    size_t take = special ? 1 : utf8CharLen(in + i, inLen - i);
    size_t need = special ? 3 : take;
    if (w + need > limit) break;
    if (out) {
      if (special) {
        out[w] = '%';
        out[w + 1] = kHex[c >> 4];
        out[w + 2] = kHex[c & 15];
      } else {
        memcpy(out + w, in + i, take);
      }
    }
    w += need;
    i += take;
  }
  return w;
}

}  // namespace

uint8_t xorSum(const char* s, size_t n) {
  uint8_t x = 0;
  for (size_t i = 0; i < n; i++) x ^= (uint8_t)s[i];
  return x;
}

size_t encodeFrame(char* out, size_t cap, const char* name, const Arg* a, size_t n) {
  if (!out || !name || cap < kMaxFrame + 1 || n > kMaxArgs) return 0;
  size_t nameLen = strlen(name);
  if (nameLen == 0 || nameLen > kMaxBody) return 0;

  char num[kMaxArgs][14];
  size_t full[kMaxArgs] = {0}, lim[kMaxArgs] = {0};
  bool done[kMaxArgs] = {false};
  size_t fixed = nameLen, textCnt = 0;

  for (size_t i = 0; i < n; i++) {
    fixed += 1;  // кома
    if (a[i].kind == Arg::Num) {
      snprintf(num[i], sizeof num[i], "%ld", a[i].num);
      fixed += strlen(num[i]);
    } else if (a[i].kind == Arg::Str) {
      fixed += escapeImpl(a[i].str, nullptr, (size_t)-1);
    } else {
      full[i] = escapeImpl(a[i].str, nullptr, (size_t)-1);
      textCnt++;
    }
  }
  if (fixed > kMaxBody) return 0;

  // Розподіл бюджету між текстовими полями: короткі лишаються цілими, решта ділять залишок
  size_t remain = kMaxBody - fixed, left = textCnt;
  bool changed = true;
  while (left && changed) {
    changed = false;
    size_t share = remain / left;
    for (size_t i = 0; i < n; i++) {
      if (a[i].kind == Arg::Text && !done[i] && full[i] <= share) {
        lim[i] = full[i];
        remain -= full[i];
        done[i] = true;
        left--;
        changed = true;
        break;
      }
    }
  }
  if (left) {
    size_t share = remain / left;
    for (size_t i = 0; i < n; i++)
      if (a[i].kind == Arg::Text && !done[i]) lim[i] = share;
  }

  char* body = out + 1;
  char* p = body;
  memcpy(p, name, nameLen);
  p += nameLen;
  for (size_t i = 0; i < n; i++) {
    *p++ = ',';
    if (a[i].kind == Arg::Num) {
      size_t l = strlen(num[i]);
      memcpy(p, num[i], l);
      p += l;
    } else if (a[i].kind == Arg::Str) {
      p += escapeImpl(a[i].str, p, (size_t)-1);
    } else {
      p += escapeImpl(a[i].str, p, lim[i]);
    }
  }
  size_t bodyLen = (size_t)(p - body);
  uint8_t x = xorSum(body, bodyLen);
  out[0] = '$';
  *p++ = '*';
  *p++ = kHex[x >> 4];
  *p++ = kHex[x & 15];
  *p++ = '\n';
  *p = 0;
  return (size_t)(p - out);
}

// ---------------- Парсер ----------------

void FrameParser::feed(uint8_t b) {
  if (b == '$') {  // ресинхронізація: '$' усередині розбору починає новий кадр
    if (state_ != Idle && (len_ > 0 || state_ == Overflow)) bad_++;
    state_ = Body;
    len_ = 0;
    return;
  }
  if (state_ == Idle) return;
  if (b == '\r') return;
  if (b == '\n') {
    if (state_ == Body) finish();
    else bad_++;
    state_ = Idle;
    len_ = 0;
    return;
  }
  if (state_ == Overflow) return;
  if (b == 0 || len_ >= kMaxStore) {  // NUL або завеликий кадр
    state_ = Overflow;
    return;
  }
  buf_[len_++] = (char)b;
}

void FrameParser::finish() {
  if (len_ < 4 || buf_[len_ - 3] != '*') { bad_++; return; }
  int h = hexVal(buf_[len_ - 2], false), l = hexVal(buf_[len_ - 1], false);
  if (h < 0 || l < 0) { bad_++; return; }
  size_t bl = len_ - 3;
  if (xorSum(buf_, bl) != (uint8_t)((h << 4) | l)) { bad_++; return; }
  buf_[bl] = 0;
  good_++;
  if (cb_) cb_(buf_, bl, ctx_);
}

// ---------------- Розбір тіла ----------------

void percentDecode(char* s) {
  char* w = s;
  for (const char* r = s; *r; r++) {
    if (r[0] == '%' && r[1] && r[2]) {
      int h = hexVal(r[1], true), l = hexVal(r[2], true);
      if (h >= 0 && l >= 0) {
        *w++ = (char)((h << 4) | l);
        r += 2;
        continue;
      }
    }
    *w++ = *r;
  }
  *w = 0;
}

bool splitBody(char* body, Msg& m) {
  m.name = body;
  m.argc = 0;
  m.tooMany = false;
  for (size_t i = 0; i < kMaxArgs; i++) m.args[i] = nullptr;
  char* p = body;
  while (*p && *p != ',') p++;
  while (*p == ',') {
    *p++ = 0;
    char* start = p;
    while (*p && *p != ',') p++;
    if (m.argc < kMaxArgs) m.args[m.argc++] = start;
    else m.tooMany = true;
  }
  for (size_t i = 0; i < m.argc; i++) percentDecode(m.args[i]);
  return m.name[0] != 0;
}

}  // namespace proto
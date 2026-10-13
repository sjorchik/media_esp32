#pragma once
#include <stddef.h>
#include <stdint.h>
#include <string.h>

namespace Utf8 {

// Копіює src у dst (cap з урахуванням NUL): не більше cap-1 байтів, обрізання лише по межі
// символу, некоректні байти → '?'. Повертає довжину без NUL.
inline size_t copySafe(char* dst, size_t cap, const char* src) {
  if (!cap) return 0;
  size_t w = 0, i = 0;
  if (!src) { dst[0] = 0; return 0; }
  while (src[i]) {
    uint8_t c = (uint8_t)src[i];
    size_t n = 1;
    bool ok = true;
    if (c < 0x80) n = 1;
    else if (c >= 0xC2 && c < 0xE0) n = 2;
    else if (c >= 0xE0 && c < 0xF0) n = 3;
    else if (c >= 0xF0 && c < 0xF5) n = 4;
    else ok = false;
    if (ok && n > 1) {
      for (size_t k = 1; k < n; k++) {
        if (((uint8_t)src[i + k] & 0xC0) != 0x80) { ok = false; break; }  // NUL теж сюди
      }
    }
    if (!ok) {
      if (w + 1 >= cap) break;
      dst[w++] = '?';
      i++;
      continue;
    }
    if (w + n >= cap) break;
    memcpy(dst + w, src + i, n);
    w += n;
    i += n;
  }
  dst[w] = 0;
  return w;
}

}  // namespace Utf8
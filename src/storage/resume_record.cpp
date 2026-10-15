#include "storage/resume_record.h"

#include <string.h>

namespace Resume {

namespace {
inline void put32(uint8_t* p, uint32_t v) {
  p[0] = (uint8_t)v;
  p[1] = (uint8_t)(v >> 8);
  p[2] = (uint8_t)(v >> 16);
  p[3] = (uint8_t)(v >> 24);
}
inline uint32_t get32(const uint8_t* p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
}  // namespace

// CRC-32 (IEEE 802.3, поліном 0xEDB88320, без таблиці — блоби короткі)
uint32_t crc32(const uint8_t* p, size_t n) {
  uint32_t c = 0xFFFFFFFFu;
  for (size_t i = 0; i < n; i++) {
    c ^= p[i];
    for (int k = 0; k < 8; k++) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1u)));
  }
  return ~c;
}

size_t serialize(const char* path, size_t pathLen, uint32_t off, uint32_t ms, uint8_t* out, size_t cap) {
  if (!path || !out || pathLen == 0 || pathLen > kPathMax) return 0;
  const size_t total = kOverhead + pathLen;
  if (cap < total) return 0;
  out[0] = 'R';
  out[1] = 'S';
  out[2] = kVersion;
  out[3] = (uint8_t)pathLen;
  out[4] = (uint8_t)(pathLen >> 8);
  memcpy(out + 5, path, pathLen);
  put32(out + 5 + pathLen, off);
  put32(out + 9 + pathLen, ms);
  put32(out + 13 + pathLen, crc32(out, 13 + pathLen));
  return total;
}

ParseErr parse(const uint8_t* b, size_t n, Record& r) {
  if (!b || n < 5) return ParseErr::Short;
  if (b[0] != 'R' || b[1] != 'S') return ParseErr::Magic;
  if (b[2] != kVersion) return ParseErr::Version;  // інша версія — розкладка невідома, CRC не перевіряємо
  const size_t len = (size_t)b[3] | ((size_t)b[4] << 8);
  if (len > kPathMax) return ParseErr::Path;
  const size_t total = kOverhead + len;
  if (n < total) return ParseErr::Short;
  if (n > total) return ParseErr::Len;
  if (crc32(b, total - 4) != get32(b + total - 4)) return ParseErr::Crc;
  if (len == 0 || memchr(b + 5, 0, len)) return ParseErr::Path;
  r.version = b[2];
  memcpy(r.path, b + 5, len);
  r.path[len] = 0;
  r.off = get32(b + 5 + len);
  r.ms = get32(b + 9 + len);
  return ParseErr::Ok;
}

const char* errName(ParseErr e) {
  switch (e) {
    case ParseErr::Ok: return "ok";
    case ParseErr::Short: return "обрізаний";
    case ParseErr::Magic: return "чужий маркер";
    case ParseErr::Version: return "невідома версія";
    case ParseErr::Path: return "поганий шлях";
    case ParseErr::Len: return "зайві байти";
    default: return "поганий CRC";
  }
}

}  // namespace Resume

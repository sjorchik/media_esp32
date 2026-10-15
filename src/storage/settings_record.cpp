#include "storage/settings_record.h"

#include "storage/resume_record.h"

namespace {

constexpr uint8_t kMagic0 = 'S';
constexpr uint8_t kMagic1 = 'T';
constexpr size_t kBody = Settings::kBlobSize - 4;  // байти під CRC

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

size_t Settings::serialize(uint8_t lastSrc, uint8_t shuf, uint8_t rep, uint8_t* out, size_t cap) {
  if (!out || cap < kBlobSize || lastSrc > kSrcMax || shuf > kShufMax || rep > kRepMax) return 0;
  out[0] = kMagic0;
  out[1] = kMagic1;
  out[2] = kVersion;
  out[3] = lastSrc;
  out[4] = shuf;
  out[5] = rep;
  put32(out + kBody, Resume::crc32(out, kBody));
  return kBlobSize;
}

Settings::ParseErr Settings::parse(const uint8_t* b, size_t n, Record& r) {
  if (!b || n < kBlobSize) return ParseErr::Short;
  if (n > kBlobSize) return ParseErr::Len;
  if (b[0] != kMagic0 || b[1] != kMagic1) return ParseErr::Magic;
  if (b[2] != kVersion) return ParseErr::Version;
  if (get32(b + kBody) != Resume::crc32(b, kBody)) return ParseErr::Crc;
  if (b[3] > kSrcMax || b[4] > kShufMax || b[5] > kRepMax) return ParseErr::Range;
  r.version = b[2];
  r.lastSrc = b[3];
  r.shuf = b[4];
  r.rep = b[5];
  return ParseErr::Ok;
}

const char* Settings::errName(ParseErr e) {
  switch (e) {
    case ParseErr::Ok: return "ok";
    case ParseErr::Short: return "обрізаний";
    case ParseErr::Len: return "задовгий";
    case ParseErr::Magic: return "чужий маркер";
    case ParseErr::Version: return "невідома версія";
    case ParseErr::Crc: return "поганий CRC";
    default: return "значення поза діапазоном";
  }
}

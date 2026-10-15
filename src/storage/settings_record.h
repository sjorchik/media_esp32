#pragma once
#include <stddef.h>
#include <stdint.h>

// Запис налаштувань (M5b2): останнє джерело, shuffle, repeat. Чистий модуль: без NVS, Arduino і FreeRTOS
// (збирається на ПК). CRC32 — з Resume::crc32 (storage/resume_record).
//
// Блоб (10 Б): [0..1] маркер 'S','T' | [2] версія | [3] last_src | [4] shuf | [5] rep | [6..9] CRC32 (LE) по байтах 0..5
namespace Settings {

inline constexpr uint8_t kVersion = 1;
inline constexpr size_t kBlobSize = 2 + 1 + 3 + 4;
inline constexpr uint8_t kSrcMax = 3;    // 0=немає, 1=bt, 2=sd, 3=usb
inline constexpr uint8_t kShufMax = 1;
inline constexpr uint8_t kRepMax = 2;

struct Record {
  uint8_t version;   // версія формату (після parse)
  uint8_t lastSrc;   // 0..3
  uint8_t shuf;      // 0..1
  uint8_t rep;       // 0..2
};

enum class ParseErr : uint8_t { Ok, Short, Len, Magic, Version, Crc, Range };

// Записує блоб у out; повертає kBlobSize або 0 (значення поза діапазоном, мало місця в cap).
size_t serialize(uint8_t lastSrc, uint8_t shuf, uint8_t rep, uint8_t* out, size_t cap);
inline size_t serialize(const Record& r, uint8_t* out, size_t cap) { return serialize(r.lastSrc, r.shuf, r.rep, out, cap); }

// Відкидає: обрізаний (Short) / задовгий (Len), чужий маркер, невідому версію, поганий CRC, значення поза діапазоном.
ParseErr parse(const uint8_t* b, size_t n, Record& r);

const char* errName(ParseErr e);

}  // namespace Settings

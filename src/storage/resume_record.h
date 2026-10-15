#pragma once
#include <stddef.h>
#include <stdint.h>
#include <string.h>

// Запис резюме (M5b1): шлях треку + позиція. Чистий модуль: без NVS, Arduino і FreeRTOS (збирається на ПК).
//
// Блоб (little-endian):
//   [0..1] маркер 'R','S' | [2] версія формату | [3..4] довжина шляху u16 | шлях (UTF-8, без NUL)
//   | зсув u32 | мс u32 | CRC32 u32 (по всіх попередніх байтах)
// Повний розмір = kOverhead + довжина шляху; максимум kBlobMax.
namespace Resume {

inline constexpr uint8_t kVersion = 1;
inline constexpr size_t kPathMax = 255;                 // байтів шляху (UTF-8), без NUL
inline constexpr size_t kOverhead = 5 + 4 + 4 + 4;      // заголовок + зсув + мс + CRC
inline constexpr size_t kBlobMax = kPathMax + kOverhead;

struct Record {
  uint8_t version;               // версія формату (після parse)
  char path[kPathMax + 1];       // UTF-8, завершений NUL
  uint32_t off;                  // байтовий зсув початку наступного недекодованого кадру
  uint32_t ms;                   // пройдений час, мс
};

enum class ParseErr : uint8_t { Ok, Short, Magic, Version, Path, Len, Crc };

uint32_t crc32(const uint8_t* p, size_t n);

// Записує блоб у out; повертає розмір або 0 (порожній шлях, шлях > kPathMax, мало місця в cap).
size_t serialize(const char* path, size_t pathLen, uint32_t off, uint32_t ms, uint8_t* out, size_t cap);
inline size_t serialize(const Record& r, uint8_t* out, size_t cap) {
  return serialize(r.path, strlen(r.path), r.off, r.ms, out, cap);
}

// Розбирає блоб. Відкидає: обрізаний/задовгий, чужий маркер, невідому версію, шлях > kPathMax або порожній,
// NUL усередині шляху, поганий CRC. При помилці r не гарантовано змінений.
ParseErr parse(const uint8_t* b, size_t n, Record& r);

const char* errName(ParseErr e);

}  // namespace Resume


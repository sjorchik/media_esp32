#pragma once
#include <stddef.h>
#include <stdint.h>

#include "storage/resume_record.h"

// M4b4. Привʼязка резюме USB до флешки: у блобі Resume (формат без змін) шлях зберігається як "XXXXXXXX|<шлях>",
// де XXXXXXXX — серійний номер тому з завантажувального сектора (FAT16/FAT32/exFAT), 8 HEX у верхньому регістрі.
// Префікс і шлях — один блоб, тож записуються атомарно. Чистий модуль: без NVS/Arduino, збирається в native.
namespace ResumeVol {

inline constexpr size_t kPrefix = 9;                              // 8 HEX + '|'
inline constexpr size_t kPathMax = Resume::kPathMax - kPrefix;    // найдовший шлях треку, що влізає разом із префіксом

// Пише "XXXXXXXX|" у dst[0..kPrefix-1] (NUL не ставить: далі викликач кладе шлях з dst + kPrefix).
inline void putPrefix(char* dst, uint32_t serial) {
  static const char hx[] = "0123456789ABCDEF";
  for (int i = 0; i < 8; i++) dst[i] = hx[(serial >> (28 - 4 * i)) & 0xF];
  dst[8] = '|';
}

enum class Verdict : uint8_t {
  Match,        // та сама флешка: rest — шлях треку
  OtherVolume,  // серійний номер інший: резюме не застосовувати
  NoId,         // поточний серійний номер невідомий (0): резюме не застосовувати й не писати
  BadFormat,    // збережений шлях без префікса (напр. записаний не цією логікою)
};

inline bool hexVal(char c, uint32_t& v) {
  if (c >= '0' && c <= '9') v = (uint32_t)(c - '0');
  else if (c >= 'A' && c <= 'F') v = (uint32_t)(c - 'A' + 10);
  else return false;
  return true;
}

// Розбір збереженого шляху й порівняння з серійним номером поточної флешки.
inline Verdict check(const char* stored, uint32_t cur, const char*& rest, uint32_t* stSerial = nullptr) {
  rest = stored;
  if (!stored) return Verdict::BadFormat;
  uint32_t s = 0;
  for (int i = 0; i < 8; i++) {
    uint32_t v;
    if (!hexVal(stored[i], v)) return Verdict::BadFormat;
    s = (s << 4) | v;
  }
  if (stored[8] != '|' || stored[9] == 0) return Verdict::BadFormat;
  rest = stored + kPrefix;
  if (stSerial) *stSerial = s;
  if (cur == 0) return Verdict::NoId;
  return s == cur ? Verdict::Match : Verdict::OtherVolume;
}

}  // namespace ResumeVol

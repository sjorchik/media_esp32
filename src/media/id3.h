#pragma once
#include <stddef.h>
#include <stdint.h>

#include "config.h"
#include "media/ifile_reader.h"

// Розбір ID3 (v2.2/2.3/2.4, запасний v1). Без залежності від SD/Arduino: читання лише через IFileReader,
// памʼять — скретч від викликача (≥ kId3FrameBuf + kId3WinBytes), динамічних виділень немає.
namespace Id3 {

// UTF-8, завжди завершені нулем; вміст ≤ kEvMetaFieldMax байтів, обрізання по межі символу.
struct Tags {
  char artist[kEvMetaFieldMax + 1];
  char title[kEvMetaFieldMax + 1];
  char album[kEvMetaFieldMax + 1];
  void clear() { artist[0] = title[0] = album[0] = 0; }
};

enum : uint8_t { kFoundV2 = 1, kFoundV1 = 2 };  // біти результату parse()

// Діагностика для DBG,tag. Поля: 0 = виконавець, 1 = назва, 2 = альбом.
struct FieldInfo {
  uint8_t src;                // 0 немає, 2 = з v2, 1 = з v1
  uint8_t enc;                // байт кодування кадру (v1: 0)
  uint8_t rawN;
  uint8_t raw[kId3DbgRaw];    // перші байти тексту (без байта кодування)
};
struct Info {
  uint8_t ver;                // 0 або 2/3/4 (версія ID3v2)
  uint16_t frames;            // переглянуто кадрів
  uint32_t tagBytes;          // розмір тегу v2 за заголовком
  uint32_t readBytes;         // реально прочитано з носія
  bool cut;                   // розбір урізано лімітом kId3MaxReadBytes
  FieldInfo f[3];
};

// ---- чисті функції (тестуються на ПК) ----
// Текст кадру за байтом кодування (0 Latin1/cp1251/UTF-8-евристика, 1 UTF-16+BOM, 2 UTF-16BE, 3 UTF-8).
// p/n — байти ПІСЛЯ байта кодування. cut=true: вхід обрізано буфером (допускається неповний хвіст UTF-8).
// Бере перше значення (до NUL), керуючі → пробіл, хвостові пробіли знімає. out завжди завершено нулем; повертає довжину.
size_t decodeText(uint8_t enc, const uint8_t* p, size_t n, char* out, size_t cap, bool cut = false);
// Рядок, позначений Latin1 (і ID3v1): UTF-8 → cp1251 → Latin1 (порядок за завданням). tailCut — як cut.
size_t decodeLegacy(const uint8_t* p, size_t n, char* out, size_t cap, bool tailCut = false);
// true: усі байти ≥0x80 — коректний UTF-8 і є хоча б одна багатобайтова послідовність.
bool looksUtf8(const uint8_t* p, size_t n, bool tailCut = false);
// true: евристика «схоже на cp1251» (частка байтів ≥0x80 або наявність 0x80..0x9F).
bool looksCp1251(const uint8_t* p, size_t n);
size_t unsync(uint8_t* p, size_t n);          // на місці: FF 00 → FF; повертає нову довжину
uint32_t syncsafe32(const uint8_t* p);        // 4 × 7 біт

// Розбір файлу з поточною позицією довільною (викликач після цього робить seek). Повертає біти kFound*.
// scratch/cap: ≥ kId3ScratchMin. Некоректні/обрізані дані ігноруються (нічого не пишеться за межі).
inline constexpr size_t kId3ScratchMin = kId3FrameBuf + kId3WinBytes;
uint8_t parse(IFileReader& rd, uint8_t* scratch, size_t cap, Tags& out, Info* info = nullptr);

}  // namespace Id3

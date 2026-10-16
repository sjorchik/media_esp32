#pragma once

// Цифровий рівень виходу (M6a): послаблення PCM у цілих дБ ДО ЦАП, аналог output_trim на S3.
// Три слоти (bt, sd, usb); застосовується слот того джерела, чий шлях зараз пише PCM.
// Лише послаблення: 0 дБ = блок не чіпається (бітова ідентичність). Зміна коефіцієнта — лінійна рампа
// (повний хід 0→1 за kLevelRampFrames кадрів ≈ 10 мс), без клацань.
//
// Відмінності від output_trim (S3): коефіцієнт float, а не Q16 з таблицею (тут float-конвеєр і немає
// вимоги «без powf у гарячому шляху»: powf лише при зміні дБ); рампа в кадрах фіксована (441), а не від fs.
//
// Потоки: setDb()/getDb() — з будь-якої задачі (атомарні байти); advance()/apply()/process() — лише з задачі,
// що пише PCM (mp3 для SD, audio для BT); без логів, блокувань і millis().
// Одне насичення в кінці конвеєра: eq::process() множить на Gain у float і насичує один раз;
// якщо смуг EQ немає, Gain застосовує apply().

#include <stdint.h>

#include "config.h"

namespace level {

enum class Slot : uint8_t { Bt = 0, Sd = 1, Usb = 2 };
constexpr uint8_t kSlots = 3;

// Параметри посилення на один блок. Коефіцієнт кадру f (0-based):
//   f < nRamp  → g0 + dg·(f+1), а останній кадр рампи = gEnd точно;  f ≥ nRamp → gEnd.
struct Gain {
  float g0;       // коефіцієнт на початку блоку
  float dg;       // крок за кадр (±), 0 якщо рампи нема
  int32_t nRamp;  // кадрів рампи в цьому блоці (0 = усталений рівень)
  float gEnd;     // коефіцієнт після рампи
  bool unity;     // true: нічого не робити (усталений 0 дБ)
};

constexpr Gain kUnityGain = {1.0f, 0.0f, 0, 1.0f, true};

// Коефіцієнт кадру f за правилом вище. g — коефіцієнт попереднього кадру (початково g0).
inline float stepGain(const Gain& lg, int32_t f, float g) {
  if (f < lg.nRamp) return (f + 1 == lg.nRamp) ? lg.gEnd : g + lg.dg;
  return lg.gEnd;
}

// float → int16 з округленням і насиченням (без обертання).
inline int16_t toS16(float y) {
  if (y > 32767.0f) y = 32767.0f;
  if (y < -32768.0f) y = -32768.0f;
  return static_cast<int16_t>(y >= 0.0f ? y + 0.5f : y - 0.5f);
}

// 10^(db/20).
float dbToLinear(int db);

// Ціль слота, дБ. Значення поза [kLevelMinDb, kLevelMaxDb] обрізається. Лише RAM (після перезавантаження 0).
void setDb(Slot s, int8_t db);
int8_t getDb(Slot s);

// Слот, який востаннє використовував advance() (-1 = ще ніхто). Для DBG,lvl.
int lastSlot();

// Бере в рампи frames кадрів слота s і повертає параметри блоку. Зміна слота (інше джерело) —
// стрибок одразу до цілі без рампи (потік новий). Лише з задачі, що пише PCM.
Gain advance(Slot s, int32_t frames);

// Застосувати Gain до блоку int16 (ch = 1 або 2 каналів, frames кадрів) на місці. unity → не чіпає.
void apply(int16_t* buf, int32_t frames, uint8_t ch, const Gain& lg);

// advance + apply (шлях без EQ і для тестів).
void process(int16_t* buf, int32_t frames, uint8_t ch, Slot s);

}  // namespace level

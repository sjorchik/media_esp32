#pragma once

// 5-смуговий графічний еквалайзер PCM медіамодуля (M6a). Порт eq з S3: алгоритм без змін —
// peaking-biquad (RBJ Audio EQ Cookbook), float, форма DF2T, смуги каскадом на кожен канал.
//
// Відмінності від S3:
//   • формат блоку int16 (I2S на WROOM завжди 16 біт), інтерліврований L,R,L,R… (ch = 2) або моно (ch = 1);
//   • process() приймає level::Gain і виконує конвеєр EQ → рівень → ОДНЕ насичення (одне int16→float на кадр);
//   • reset() — скинути стани фільтрів при старті нового потоку (інше джерело/трек);
//   • bandActive() — для діагностики; без EQ_DEBUG-логів (ядро без Arduino.h, збирається в native).
//
// Порядок у шляху: декодування → eq::process (EQ → рівень → насичення) → AudioOut → I2S.
// Усі смуги 0 дБ (або вимкнені) і рівень 0 дБ: блок не чіпається (бітова ідентичність).
//
// Потоки: setBandDb()/setAllDb()/setSampleRate()/reset()/get*() — з будь-якої задачі, без мʼютексів (атомарні слова);
// process() — лише з задачі, що пише PCM (mp3 для SD, audio для BT; джерела ексклюзивні), без логів і блокувань.
// Коефіцієнти перераховуються всередині process() при зміні пресету або частоти дискретизації.
// Стан лише в RAM: після перезавантаження всі смуги 0 дБ.

#include <stdint.h>

#include "audio/level.h"
#include "config.h"

namespace eq {

// Підсилення однієї смуги, дБ. band >= kBandCount ігнорується; db обрізається до kGainMinDb..kGainMaxDb.
void setBandDb(uint8_t band, int8_t db);

// Поточне значення смуги (0 для неіснуючої).
int8_t getBandDb(uint8_t band);

// Усі смуги разом (один перерахунок).
void setAllDb(const int8_t gains[eq_cfg::kBandCount]);

// Копія поточного пресету в out.
void getAllDb(int8_t out[eq_cfg::kBandCount]);

// Частота дискретизації потоку, Гц. Значення поза kMinSampleRateHz..kMaxSampleRateHz ігноруються.
void setSampleRate(uint32_t hz);

// Остання прийнята частота дискретизації, Гц.
uint32_t sampleRate();

// Чи смуга реально працює при поточних пресеті й частоті (db != 0 і f0 ≤ kMaxFreqRatio·fs).
bool bandActive(uint8_t band);

// Попросити обнулити стани фільтрів (виконає наступний process()). Викликати при старті нового потоку.
void reset();

// EQ → рівень (lg) → насичення, на місці. frames — кадрів (для ch=2 — пар L,R). ch = 1 або 2.
void process(int16_t* buf, int32_t frames, uint8_t ch, const level::Gain& lg);

// Лише EQ (без рівня): те саме з одиничним посиленням.
inline void process(int16_t* buf, int32_t frames, uint8_t ch = 2) {
  process(buf, frames, ch, level::kUnityGain);
}

}  // namespace eq

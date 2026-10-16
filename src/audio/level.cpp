// Цифровий рівень виходу (M6a). Опис і правила потоків — у level.h. Файл не включає Arduino.h (збирається в native).

#include "audio/level.h"

#include <atomic>
#include <math.h>

namespace {

std::atomic<int8_t> s_db[level::kSlots] = {{kLevelDefaultDb}, {kLevelDefaultDb}, {kLevelDefaultDb}};
std::atomic<int8_t> s_last{-1};

// Стан хука (лише задача, що пише PCM; джерела ексклюзивні, тож одночасно пише одна)
float s_cur = 1.0f;       // поточний коефіцієнт
float s_tgt = 1.0f;       // коефіцієнт цілі
int8_t s_appliedDb = 0;   // дБ, з яких пораховано s_tgt
uint8_t s_slot = 0xFF;    // слот останнього advance (0xFF = ще не було)

constexpr float kStep = 1.0f / (float)kLevelRampFrames;  // зміна коефіцієнта за кадр

int clampDb(int db) {
  if (db < kLevelMinDb) db = kLevelMinDb;
  if (db > kLevelMaxDb) db = kLevelMaxDb;
  return db;
}

}  // namespace

float level::dbToLinear(int db) { return powf(10.0f, static_cast<float>(db) / 20.0f); }

void level::setDb(Slot s, int8_t db) {
  const uint8_t i = static_cast<uint8_t>(s);
  if (i >= kSlots) return;
  s_db[i].store(static_cast<int8_t>(clampDb(db)), std::memory_order_relaxed);
}

int8_t level::getDb(Slot s) {
  const uint8_t i = static_cast<uint8_t>(s);
  if (i >= kSlots) return 0;
  return s_db[i].load(std::memory_order_relaxed);
}

int level::lastSlot() { return s_last.load(std::memory_order_relaxed); }

level::Gain level::advance(Slot s, int32_t frames) {
  const uint8_t si = static_cast<uint8_t>(s);
  if (si >= kSlots) return kUnityGain;
  const int8_t db = s_db[si].load(std::memory_order_relaxed);
  const bool slotChanged = (si != s_slot);
  if (slotChanged || db != s_appliedDb) {
    s_tgt = (db == 0) ? 1.0f : dbToLinear(db);
    s_appliedDb = db;
  }
  if (slotChanged) {  // інше джерело = новий потік: одразу на ціль, без рампи
    s_cur = s_tgt;
    s_slot = si;
    s_last.store(static_cast<int8_t>(si), std::memory_order_relaxed);
  }

  Gain g;
  const float diff = s_tgt - s_cur;
  if (diff == 0.0f || frames <= 0) {  // усталений рівень
    g.g0 = s_cur;
    g.dg = 0.0f;
    g.nRamp = 0;
    g.gEnd = s_cur;
    g.unity = (s_cur >= 1.0f);
    return g;
  }
  const float ad = fabsf(diff);
  int32_t need = static_cast<int32_t>(ceilf(ad / kStep));
  if (need < 1) need = 1;
  const int32_t n = need < frames ? need : frames;
  g.g0 = s_cur;
  g.dg = diff > 0.0f ? kStep : -kStep;
  g.nRamp = n;
  g.gEnd = (n == need) ? s_tgt : s_cur + g.dg * static_cast<float>(n);
  g.unity = false;
  s_cur = g.gEnd;
  return g;
}

void level::apply(int16_t* buf, int32_t frames, uint8_t ch, const Gain& lg) {
  if (buf == nullptr || frames <= 0 || lg.unity) return;
  float g = lg.g0;
  int16_t* p = buf;
  if (ch == 1) {
    for (int32_t f = 0; f < frames; ++f) {
      g = stepGain(lg, f, g);
      p[f] = toS16(static_cast<float>(p[f]) * g);
    }
  } else if (ch == 2) {
    for (int32_t f = 0; f < frames; ++f) {
      g = stepGain(lg, f, g);
      p[0] = toS16(static_cast<float>(p[0]) * g);
      p[1] = toS16(static_cast<float>(p[1]) * g);
      p += 2;
    }
  }
}

void level::process(int16_t* buf, int32_t frames, uint8_t ch, Slot s) {
  if (buf == nullptr || frames <= 0) return;
  apply(buf, frames, ch, advance(s, frames));
}

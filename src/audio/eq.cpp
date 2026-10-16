// 5-смуговий еквалайзер PCM медіамодуля (M6a). Опис і правила потоків — у eq.h.
// Файл НЕ включає Arduino.h: чиста математика, збирається в native-тестах.

#include "audio/eq.h"

#include <atomic>
#include <math.h>
#include <string.h>

namespace {

constexpr uint8_t kN = eq_cfg::kBandCount;

// ---------------------------------------------------------------------------
// Спільний стан (пишуть інші задачі, читає process)
// ---------------------------------------------------------------------------
std::atomic<int8_t> s_gainDb[kN] = {};
// «Пресет змінився»: стартує з 1, застосований = 0, тож перший блок завжди рахує коефіцієнти.
std::atomic<uint32_t> s_version{1};
std::atomic<uint32_t> s_rateHz{eq_cfg::kDefaultSampleRateHz};
std::atomic<bool> s_resetReq{false};

// ---------------------------------------------------------------------------
// Стан process() (лише задача, що пише PCM)
// ---------------------------------------------------------------------------
struct Coef {
  float b0, b1, b2, a1, a2;  // нормовані на a0
};

Coef s_coef[kN];
float s_z[2][kN][2];  // [канал][смуга][z1, z2] (DF2T)
uint8_t s_active[kN];
uint8_t s_activeCount = 0;
uint32_t s_appliedVersion = 0;
uint32_t s_appliedRate = 0;

int clampDb(int db) {
  if (db < eq_cfg::kGainMinDb) db = eq_cfg::kGainMinDb;
  if (db > eq_cfg::kGainMaxDb) db = eq_cfg::kGainMaxDb;
  return db;
}

bool isOn(int db, uint8_t band, uint32_t rate) {
  return db != 0 && static_cast<double>(eq_cfg::kBandFreqHz[band]) <=
                        eq_cfg::kMaxFreqRatio * static_cast<double>(rate);
}

// Peaking EQ (RBJ Audio EQ Cookbook). Коефіцієнти — в double (рідкісна подія), зберігаємо в float.
Coef designPeaking(double f0, double dB, double fs) {
  const double kPi = 3.14159265358979323846;
  const double A = pow(10.0, dB / 40.0);
  const double w0 = 2.0 * kPi * f0 / fs;
  const double cw = cos(w0);
  const double alpha = sin(w0) / (2.0 * eq_cfg::kQ);
  const double a0 = 1.0 + alpha / A;
  Coef c;
  c.b0 = static_cast<float>((1.0 + alpha * A) / a0);
  c.b1 = static_cast<float>((-2.0 * cw) / a0);
  c.b2 = static_cast<float>((1.0 - alpha * A) / a0);
  c.a1 = static_cast<float>((-2.0 * cw) / a0);
  c.a2 = static_cast<float>((1.0 - alpha / A) / a0);
  return c;
}

// Перерахунок за потреби (лише з process()).
void refresh() {
  if (s_resetReq.exchange(false, std::memory_order_acq_rel)) {
    memset(s_z, 0, sizeof(s_z));  // новий потік — старий стан фільтрів не потрібен
  }
  const uint32_t ver = s_version.load(std::memory_order_acquire);
  const uint32_t rate = s_rateHz.load(std::memory_order_relaxed);
  if (ver == s_appliedVersion && rate == s_appliedRate) {
    return;
  }
  const bool rateChanged = (rate != s_appliedRate);
  s_appliedVersion = ver;
  s_appliedRate = rate;
  if (rateChanged) {
    memset(s_z, 0, sizeof(s_z));
  }
  s_activeCount = 0;
  for (uint8_t b = 0; b < kN; ++b) {
    const int db = s_gainDb[b].load(std::memory_order_relaxed);
    if (isOn(db, b, rate)) {
      s_coef[b] = designPeaking(static_cast<double>(eq_cfg::kBandFreqHz[b]), static_cast<double>(db),
                                static_cast<double>(rate));
      s_active[s_activeCount++] = b;
    } else {
      // Вимкнена смуга: стан обнуляємо, щоб при вмиканні не було «хвоста».
      s_z[0][b][0] = s_z[0][b][1] = 0.0f;
      s_z[1][b][0] = s_z[1][b][1] = 0.0f;
    }
  }
}

// Один біквад DF2T (z — два елементи стану).
inline float band(const Coef& k, float* z, float x) {
  const float y = k.b0 * x + z[0];
  z[0] = k.b1 * x - k.a1 * y + z[1];
  z[1] = k.b2 * x - k.a2 * y;
  return y;
}

}  // namespace

// ---------------------------------------------------------------------------
// Публічне API
// ---------------------------------------------------------------------------
void eq::setBandDb(uint8_t band, int8_t db) {
  if (band >= kN) {
    return;
  }
  const int8_t v = static_cast<int8_t>(clampDb(db));
  if (s_gainDb[band].exchange(v, std::memory_order_relaxed) != v) {
    s_version.fetch_add(1, std::memory_order_release);
  }
}

int8_t eq::getBandDb(uint8_t band) {
  if (band >= kN) {
    return 0;
  }
  return s_gainDb[band].load(std::memory_order_relaxed);
}

void eq::setAllDb(const int8_t gains[eq_cfg::kBandCount]) {
  if (gains == nullptr) {
    return;
  }
  bool changed = false;
  for (uint8_t b = 0; b < kN; ++b) {
    const int8_t v = static_cast<int8_t>(clampDb(gains[b]));
    if (s_gainDb[b].exchange(v, std::memory_order_relaxed) != v) {
      changed = true;
    }
  }
  if (changed) {
    s_version.fetch_add(1, std::memory_order_release);
  }
}

void eq::getAllDb(int8_t out[eq_cfg::kBandCount]) {
  if (out == nullptr) {
    return;
  }
  for (uint8_t b = 0; b < kN; ++b) {
    out[b] = s_gainDb[b].load(std::memory_order_relaxed);
  }
}

void eq::setSampleRate(uint32_t hz) {
  if (hz < eq_cfg::kMinSampleRateHz || hz > eq_cfg::kMaxSampleRateHz) {
    return;
  }
  if (s_rateHz.load(std::memory_order_relaxed) != hz) {  // викликається щокадру: без запису, якщо не змінилось
    s_rateHz.store(hz, std::memory_order_relaxed);
  }
}

uint32_t eq::sampleRate() { return s_rateHz.load(std::memory_order_relaxed); }

bool eq::bandActive(uint8_t band) {
  if (band >= kN) {
    return false;
  }
  return isOn(s_gainDb[band].load(std::memory_order_relaxed), band, s_rateHz.load(std::memory_order_relaxed));
}

void eq::reset() { s_resetReq.store(true, std::memory_order_release); }

void eq::process(int16_t* buf, int32_t frames, uint8_t ch, const level::Gain& lg) {
  if (buf == nullptr || frames <= 0 || (ch != 1 && ch != 2)) {
    return;
  }
  refresh();
  const uint8_t n = s_activeCount;
  if (n == 0) {
    level::apply(buf, frames, ch, lg);  // смуг нема: лише рівень (0 дБ → блок не чіпається)
    return;
  }

  const Coef* c[kN];
  float* zl[kN];
  float* zr[kN];
  for (uint8_t i = 0; i < n; ++i) {
    const uint8_t b = s_active[i];
    c[i] = &s_coef[b];
    zl[i] = s_z[0][b];
    zr[i] = s_z[1][b];
  }

  float g = lg.g0;
  int16_t* p = buf;
  if (ch == 2) {
    for (int32_t f = 0; f < frames; ++f) {
      g = level::stepGain(lg, f, g);
      float xl = static_cast<float>(p[0]);
      float xr = static_cast<float>(p[1]);
      for (uint8_t i = 0; i < n; ++i) {
        xl = band(*c[i], zl[i], xl);
        xr = band(*c[i], zr[i], xr);
      }
      p[0] = level::toS16(xl * g);
      p[1] = level::toS16(xr * g);
      p += 2;
    }
  } else {  // моно: лише лівий стан
    for (int32_t f = 0; f < frames; ++f) {
      g = level::stepGain(lg, f, g);
      float x = static_cast<float>(p[f]);
      for (uint8_t i = 0; i < n; ++i) {
        x = band(*c[i], zl[i], x);
      }
      p[f] = level::toS16(x * g);
    }
  }
}

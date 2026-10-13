#pragma once
#include <stddef.h>
#include <stdint.h>
#include <string.h>

// Кільцевий буфер PCM (16 біт стерео, кадр = 4 Б) «один продюсер → один споживач».
// Без блокувань і без динамічної памʼяті. Продюсер — BT-колбек, споживач — задача audio.
// Розмір — степінь двійки; індекси вільно біжать по uint32_t.
class PcmRing {
 public:
  PcmRing(uint8_t* buf, size_t cap) : buf_(buf), cap_((uint32_t)cap), mask_((uint32_t)cap - 1) {}

  // ---- керування (будь-яка задача) ----
  void setAccepting(bool on) { __atomic_store_n(&accepting_, on, __ATOMIC_RELEASE); }
  bool accepting() const { return __atomic_load_n(&accepting_, __ATOMIC_ACQUIRE); }
  void setPendingRate(uint32_t hz) { __atomic_store_n(&pendingRate_, hz, __ATOMIC_RELEASE); }
  uint32_t takePendingRate() { return __atomic_exchange_n(&pendingRate_, 0u, __ATOMIC_ACQ_REL); }

  // ---- продюсер ----
  size_t write(const uint8_t* d, size_t n) {
    n &= ~(size_t)3;                                  // лише цілі кадри
    uint32_t w = w_;
    uint32_t r = __atomic_load_n(&r_, __ATOMIC_ACQUIRE);
    uint32_t freeB = cap_ - (w - r);
    if (n > freeB) {                                  // переповнення: новий хвіст відкидаємо
      overrunBytes += (uint32_t)(n - freeB);
      overruns++;
      n = freeB;
    }
    if (!n) return 0;
    uint32_t pos = w & mask_;
    size_t first = cap_ - pos;
    if (first > n) first = n;
    memcpy(buf_ + pos, d, first);
    if (n > first) memcpy(buf_, d + first, n - first);
    __atomic_store_n(&w_, w + (uint32_t)n, __ATOMIC_RELEASE);
    return n;
  }

  // ---- споживач ----
  size_t read(uint8_t* d, size_t max) {
    uint32_t r = r_;
    uint32_t w = __atomic_load_n(&w_, __ATOMIC_ACQUIRE);
    size_t n = w - r;
    if (n > max) n = max;
    n &= ~(size_t)3;
    if (!n) return 0;
    uint32_t pos = r & mask_;
    size_t first = cap_ - pos;
    if (first > n) first = n;
    memcpy(d, buf_ + pos, first);
    if (n > first) memcpy(d + first, buf_, n - first);
    __atomic_store_n(&r_, r + (uint32_t)n, __ATOMIC_RELEASE);
    return n;
  }
  size_t available() const { return (size_t)(__atomic_load_n(&w_, __ATOMIC_ACQUIRE) - r_); }
  void flush() {
    uint32_t w = __atomic_load_n(&w_, __ATOMIC_ACQUIRE);
    flushed += w - r_;
    __atomic_store_n(&r_, w, __ATOMIC_RELEASE);
  }
  size_t capacity() const { return cap_; }

  void resetStats() {
    inBytes = outBytes = dropInactive = flushed = overrunBytes = 0;
    overruns = underruns = writeTimeouts = maxWriteUs = 0;
  }

  // ---- лічильники (кожен має одного «писаря») ----
  uint32_t inBytes = 0;        // продюсер: усі прийняті від BT байти (у т.ч. відкинуті)
  uint32_t dropInactive = 0;   // продюсер: відкинуто, бо джерело неактивне
  uint32_t overrunBytes = 0;   // продюсер: не влізло в кільце
  uint32_t overruns = 0;
  uint32_t outBytes = 0;       // споживач: віддано в AudioOut
  uint32_t flushed = 0;        // споживач: викинуто при мʼюті/зміні формату
  uint32_t underruns = 0;      // споживач: I2S-голодування (оцінка по годиннику)
  uint32_t writeTimeouts = 0;
  uint32_t maxWriteUs = 0;

 private:
  uint8_t* buf_;
  uint32_t cap_, mask_;
  volatile uint32_t w_ = 0, r_ = 0;
  uint32_t pendingRate_ = 0;
  bool accepting_ = false;
};
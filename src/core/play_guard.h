#pragma once
#include <stdint.h>

// M5c6. Чиста логіка захисту відтворення файлових джерел (SD і USB): без Arduino/FreeRTOS, тестується в native.
//  fixResume   — резюме у хвості треку або за кінцем файлу → з початку (інакше трек «закінчується» миттєво, і повтор його знову застосовує).
//  EndLoopGuard — захист від зациклення: кілька миттєвих завершень сесії підряд.
namespace PlayGuard {

struct ResumeFix {
  uint32_t off, ms;
  bool reset;  // true: резюме скинуто в (0, 0) — старт з початку треку
};

// off/ms — збережене резюме; size — розмір файлу, Б (0 = невідомий → не чіпаємо); tailMs — kResumeTailMs; guardBytes — kResumeTailGuard.
// Залишок за часом оцінюється зі швидкості off/ms (резюме збережено в точці off, що відповідає ms). Без ms (ms=0, off>0) швидкість невідома:
// беремо найгіршу, 320 кбіт/с = 40 Б/мс, тож скидаємо, коли залишок може бути у хвості.
inline ResumeFix fixResume(uint32_t off, uint32_t ms, uint32_t size, uint32_t tailMs, uint32_t guardBytes) {
  if (off == 0) return ResumeFix{0, 0, false};          // з початку і так
  if (size == 0) return ResumeFix{off, ms, false};
  if (off >= size) return ResumeFix{0, 0, true};         // за кінцем (або рівно в кінці) файлу
  const uint32_t left = size - off;
  if (left < guardBytes) return ResumeFix{0, 0, true};
  if (ms > 0) {
    const uint64_t leftMs = (uint64_t)left * ms / off;
    if (leftMs < tailMs) return ResumeFix{0, 0, true};
  } else if (left < (uint64_t)tailMs * 40u) {
    return ResumeFix{0, 0, true};
  }
  return ResumeFix{off, ms, false};
}

class EndLoopGuard {
 public:
  void start(uint32_t nowMs) {  // сесію запущено
    t0_ = nowMs;
    armed_ = true;
  }
  void reset() {  // ручна команда, stop, новий скан: лічильник скинуто
    run_ = 0;
    armed_ = false;
  }
  // Сесія завершилась природно (Eof). true = зациклення: runMax «миттєвих» завершень (< shortMs) підряд.
  bool onEnd(uint32_t nowMs, uint32_t shortMs, uint8_t runMax) {
    if (!armed_) return false;
    armed_ = false;
    if ((uint32_t)(nowMs - t0_) < shortMs) {
      if (run_ < 255) run_++;
    } else {
      run_ = 0;
    }
    return run_ >= runMax;
  }
  uint8_t run() const { return run_; }

 private:
  uint32_t t0_ = 0;
  uint8_t run_ = 0;
  bool armed_ = false;
};

}  // namespace PlayGuard

#pragma once
#include <stdint.h>

// Логіка захисту від петлі аварій (M5b2). Чистий модуль: без NVS, Arduino і esp_* (збирається на ПК).
// Клас причини перезапуску визначає шар BootGuard (storage/boot_guard_store) і передає параметром.
namespace BootLogic {

enum class Reason : uint8_t { Normal, Crash };   // Crash: PANIC, INT_WDT, TASK_WDT, WDT

struct Decision {
  uint8_t count;    // новий лічильник підряд аварійних перезапусків
  bool protect;     // режим захисту на цей запуск
  bool write;       // true: count відрізняється від старого → писати в NVS
};

// Рішення на старті. Crash: count+1 (з насиченням на 255); Normal: 0.
// protect = (новий count ≥ threshold), threshold 0 — захист вимкнено.
Decision onBoot(Reason r, uint8_t count, uint8_t threshold);

// Рішення після kBootStableMs безперервної роботи: лічильник → 0. Поле protect тут не використовується
// (режим захисту визначається лише на старті й діє до перезапуску).
Decision onStable(uint8_t count);

}  // namespace BootLogic

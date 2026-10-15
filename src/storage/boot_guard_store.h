#pragma once
#include <stdint.h>

// Захист від петлі аварій (M5b2) поверх NvsStore: ключ boot_crash (u8, підряд аварійних перезапусків).
// init() і tick() — лише з loopTask. Помилка NVS лише логується.
// Режим захисту (лічильник ≥ kCrashLoopMax на старті): ResumeStore::load повертає «нема», rsm_sd/rsm_usb стерто,
// AUTO_RESUME_SOURCE ігнорується. Діє до наступного перезапуску.
namespace BootGuard {

void init();                    // рано, одразу після NvsStore::begin() і до відновлення резюме
void tick(uint32_t nowMs);      // з loop(): через kBootStableMs безперервної роботи лічильник → 0
bool protect();                 // режим захисту на цей запуск (читається з будь-якої задачі)

// Для DBG
const char* reasonName();       // причина перезапуску (esp_reset_reason)
bool wasCrash();                // перезапуск був аварійним
uint8_t count();                // поточне значення лічильника (дзеркало NVS)
bool stableDone();              // kBootStableMs уже минув
bool clear();                   // лічильник → 0 (режим захисту цього запуску не змінюється)
bool setCount(uint8_t v);       // лише для тестів (DBG,boot,set)

}  // namespace BootGuard

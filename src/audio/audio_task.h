#pragma once
#include <stdint.h>

#include "audio/audio_out.h"

// Задача аудіо (ядро 1, prio 6). На етапі 1 — лише тестовий тон для DBG,tone;
// на етапах 3–4 тут працюватимуть декодер і подача A2DP через AudioOut.
namespace AudioTask {
void begin(AudioOut& out);
bool requestTone(uint16_t hz, uint16_t ms);  // false, якщо зайнято
bool busy();
}  // namespace AudioTask
// Керований фальшивий час для native-тестів (M4b4). У збірці під ARDUINO файл порожній і в бінарник нічого не додає.
#ifndef ARDUINO
#include "util/clock.h"

namespace {
uint32_t s_now = 0, s_step = 0;
}

uint32_t nowMs() {
  const uint32_t t = s_now;
  s_now += s_step;
  return t;
}

namespace NativeClock {
void set(uint32_t ms) { s_now = ms; }
void advance(uint32_t ms) { s_now += ms; }
void setAutoStep(uint32_t ms) { s_step = ms; }
}  // namespace NativeClock
#endif  // !ARDUINO

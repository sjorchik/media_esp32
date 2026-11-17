#pragma once
#include <stdint.h>

// Єдине джерело часу в спільному коді (скан тощо). На прошивці nowMs() = millis() (інлайн, без зміни коду й розміру);
// у native (юніт-тести) nowMs() визначено в util/clock_native.cpp, де час керований тестом: NativeClock::set/advance/setAutoStep.
#ifdef ARDUINO
#include <Arduino.h>
inline uint32_t nowMs() { return millis(); }
#else
uint32_t nowMs();
namespace NativeClock {
void set(uint32_t ms);        // встановити час
void advance(uint32_t ms);    // посунути час
void setAutoStep(uint32_t ms);  // кожен виклик nowMs() додатково зсуває час на ms (типово 0: час стоїть, поки тест його не посуне)
}  // namespace NativeClock
#endif

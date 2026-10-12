#pragma once
#include <stddef.h>

#include "config.h"

namespace Log {
void begin();   // Serial (UART0) + мʼютекс
void logf(char level, const char* tag, const char* fmt, ...) __attribute__((format(printf, 3, 4)));
void line(const char* prefix, const char* text, size_t len);  // сирий рядок + LF (дзеркало подій)
}  // namespace Log

#define LOGE(tag, ...) Log::logf('E', tag, __VA_ARGS__)
#define LOGW(tag, ...) do { if (LOG_LEVEL >= 2) Log::logf('W', tag, __VA_ARGS__); } while (0)
#define LOGI(tag, ...) do { if (LOG_LEVEL >= 3) Log::logf('I', tag, __VA_ARGS__); } while (0)
#define LOGD(tag, ...) do { if (LOG_LEVEL >= 4) Log::logf('D', tag, __VA_ARGS__); } while (0)
#include "util/log.h"

#include <Arduino.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

namespace {
SemaphoreHandle_t s_mtx = nullptr;
void lock() { if (s_mtx) xSemaphoreTake(s_mtx, portMAX_DELAY); }
void unlock() { if (s_mtx) xSemaphoreGive(s_mtx); }
}  // namespace

void Log::begin() {
  s_mtx = xSemaphoreCreateMutex();
  Serial.setRxBufferSize(kConsoleRxBuf);
  Serial.begin(kConsoleBaud);
}

void Log::logf(char level, const char* tag, const char* fmt, ...) {
  char buf[192];
  int n = snprintf(buf, sizeof buf, "[%lu][%c][%s] ", (unsigned long)millis(), level, tag);
  if (n < 0) return;
  if (n >= (int)sizeof buf) n = (int)sizeof buf - 1;
  va_list ap;
  va_start(ap, fmt);
  int m = vsnprintf(buf + n, sizeof buf - (size_t)n, fmt, ap);
  va_end(ap);
  size_t len = (size_t)n + (m < 0 ? 0 : (size_t)m);
  if (len > sizeof buf - 2) len = sizeof buf - 2;
  buf[len++] = '\n';
  lock();
  Serial.write((const uint8_t*)buf, len);
  unlock();
}

void Log::line(const char* prefix, const char* text, size_t len) {
  lock();
  Serial.write((const uint8_t*)prefix, strlen(prefix));
  Serial.write((const uint8_t*)text, len);
  Serial.write((uint8_t)'\n');
  unlock();
}
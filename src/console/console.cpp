#include "console/console.h"

#include <Arduino.h>
#include <ctype.h>
#include <string.h>

#include "config.h"
#include "core/router.h"
#include "proto/frame.h"
#include "util/diag.h"
#include "util/log.h"

namespace {

constexpr const char* TAG = "console";

void onFrame(char* body, size_t, void*) { Router::dispatch(body, Origin::Console); }
proto::FrameParser s_fp(onFrame, nullptr);
char s_line[kConsoleLineMax];

// M5c5: лічильники (28 Б). Пише лише задача console; читання з інших задач — знімок без синхронізації (діагностика).
volatile uint32_t s_peak = 0, s_high = 0, s_bytes = 0, s_lines = 0, s_long = 0, s_longB = 0, s_maxLen = 0;

// Заповнення RX-буфера: пік і «майже переповнення». Точного лічильника переповнень драйвер Arduino 2.0.17 тут не дає,
// тож ознака переповнення — пік біля ємності + втрата рядків на стороні хоста (tools/m5c5_src_stress.py --burst).
void sampleRx() {
  int a = Serial.available();
  if (a <= 0) return;
  if ((uint32_t)a > s_peak) s_peak = (uint32_t)a;
  if ((uint32_t)a * 100u >= (uint32_t)kConsoleRxBuf * kConsoleRxHighPct) s_high = s_high + 1;
}

void handleLine(char* line, size_t n) {
  while (n && (line[n - 1] == ' ' || line[n - 1] == '\t')) line[--n] = 0;
  char* p = line;
  while (*p == ' ' || *p == '\t') p++;
  if (!*p) return;
  if (*p == '$') {                         // кадр із сумою
    uint32_t badBefore = s_fp.bad();
    s_fp.feed((const uint8_t*)p, strlen(p));
    s_fp.feed((uint8_t)'\n');
    if (s_fp.bad() != badBefore) LOGW(TAG, "кадр відкинуто (сума/довжина/формат)");
  } else {                                 // простий рядок: імʼя регістронезалежне
    for (char* q = p; *q && *q != ','; ++q) *q = (char)toupper((unsigned char)*q);
    Router::dispatch(p, Origin::Console);
  }
}

void taskFn(void*) {
  size_t n = 0;
  uint32_t dropped = 0;  // байтів поточного задовгого рядка, що не вмістились
  bool overflow = false;
  for (;;) {
    sampleRx();
    while (Serial.available() > 0) {  // читаємо до спорожнення
      int c = Serial.read();
      s_bytes = s_bytes + 1;
      if (c == '\n' || c == '\r') {
        if (n && !overflow) {
          s_line[n] = 0;
          if (n > s_maxLen) s_maxLen = (uint32_t)n;
          s_lines = s_lines + 1;
          handleLine(s_line, n);
          sampleRx();  // найгірший момент: handleLine блокується на TX, RX тим часом наповнюється
        } else if (overflow) {
          s_long = s_long + 1;
          LOGW(TAG, "рядок задовгий (> %u Б), відкинуто хвіст %u Б", (unsigned)(sizeof s_line - 1), (unsigned)dropped);
        }
        n = 0;
        dropped = 0;
        overflow = false;
      } else if (n < sizeof s_line - 1) {
        s_line[n++] = (char)c;
      } else {
        overflow = true;
        dropped++;
        s_longB = s_longB + 1;
      }
    }
    vTaskDelay(pdMS_TO_TICKS(5));
  }
}

}  // namespace

Console::Stats Console::stats() {
  Stats s;
  s.rxCap = (uint32_t)kConsoleRxBuf;
  s.rxPeak = s_peak;
  s.rxHigh = s_high;
  s.bytes = s_bytes;
  s.lines = s_lines;
  s.longLines = s_long;
  s.longBytes = s_longB;
  s.maxLen = s_maxLen;
  return s;
}

void Console::resetStats() {
  s_peak = s_high = s_bytes = s_lines = s_long = s_longB = s_maxLen = 0;
}

void Console::begin() {
  TaskHandle_t h = nullptr;
  xTaskCreatePinnedToCore(taskFn, "console", kStackConsole, nullptr, kPrioConsole, &h, kCoreProto);
  Diag::registerTask("console", h);
  LOGI(TAG, "UART0 %u: приймаються $BODY*XX і NAME,арг,…; DBG,help", (unsigned)kConsoleBaud);
}

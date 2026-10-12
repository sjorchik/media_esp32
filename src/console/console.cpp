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
  bool overflow = false;
  for (;;) {
    while (Serial.available() > 0) {
      int c = Serial.read();
      if (c == '\n' || c == '\r') {
        if (n && !overflow) {
          s_line[n] = 0;
          handleLine(s_line, n);
        } else if (overflow) {
          LOGW(TAG, "рядок задовгий, відкинуто");
        }
        n = 0;
        overflow = false;
      } else if (n < sizeof s_line - 1) {
        s_line[n++] = (char)c;
      } else {
        overflow = true;
      }
    }
    vTaskDelay(pdMS_TO_TICKS(5));
  }
}

}  // namespace

void Console::begin() {
  TaskHandle_t h = nullptr;
  xTaskCreatePinnedToCore(taskFn, "console", kStackConsole, nullptr, kPrioConsole, &h, kCoreProto);
  Diag::registerTask("console", h);
  LOGI(TAG, "UART0 %u: приймаються $BODY*XX і NAME,арг,…; DBG,help", (unsigned)kConsoleBaud);
}
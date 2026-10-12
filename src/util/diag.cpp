#include "util/diag.h"

#include "esp_heap_caps.h"
#include "esp_system.h"

#include "config.h"
#include "util/log.h"

namespace {
struct Rec { const char* name; TaskHandle_t h; };
Rec s_tasks[10];
size_t s_n = 0;
uint32_t s_last = 0;
}  // namespace

void Diag::registerTask(const char* name, TaskHandle_t h) {
  if (h && s_n < sizeof(s_tasks) / sizeof(s_tasks[0])) s_tasks[s_n++] = Rec{name, h};
}

void Diag::logHeap(const char* tag) {
  LOGI("heap", "[%s] free=%u largest=%u min_free=%u", tag,
       (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT),
       (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT),
       (unsigned)esp_get_minimum_free_heap_size());
}

void Diag::logTasks() {
  for (size_t i = 0; i < s_n; i++) {
    LOGI("stack", "%-8s вільно (мін.) %u Б", s_tasks[i].name,
         (unsigned)uxTaskGetStackHighWaterMark(s_tasks[i].h));
  }
}

void Diag::tick(uint32_t nowMs) {
  if (kDiagPeriodMs == 0 || (nowMs - s_last) < kDiagPeriodMs) return;
  s_last = nowMs;
  logHeap("periodic");
  logTasks();
}
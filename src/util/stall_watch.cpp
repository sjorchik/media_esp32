#include "util/stall_watch.h"

#ifdef ARDUINO
#include "esp_timer.h"

namespace {
esp_timer_handle_t s_tmr = nullptr;
StallWatch::DumpFn s_dump = nullptr;
void* s_ctx = nullptr;

void onFire(void*) {  // задача esp_timer: app може бути заблокована, тут це не важливо
  StallWatch::DumpFn f = s_dump;
  void* c = s_ctx;
  if (f) f(c);
}
}  // namespace

void StallWatch::arm(DumpFn dump, void* ctx, uint32_t afterMs) {
  if (!s_tmr) {
    esp_timer_create_args_t a = {};
    a.callback = &onFire;
    a.name = "stall";
    if (esp_timer_create(&a, &s_tmr) != ESP_OK) {
      s_tmr = nullptr;
      return;
    }
  }
  esp_timer_stop(s_tmr);  // повторний arm перезапускає відлік
  s_dump = dump;
  s_ctx = ctx;
  esp_timer_start_once(s_tmr, (uint64_t)afterMs * 1000ULL);
}

void StallWatch::disarm() {
  if (s_tmr) esp_timer_stop(s_tmr);  // не запущений — ESP_ERR_INVALID_STATE, ігноруємо
}
#endif

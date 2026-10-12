#include "audio/audio_task.h"

#include <Arduino.h>
#include <math.h>

#include "util/diag.h"
#include "util/log.h"

namespace {
constexpr const char* TAG = "atask";
AudioOut* s_out = nullptr;
TaskHandle_t s_task = nullptr;
volatile bool s_busy = false;
volatile uint16_t s_hz = 440, s_ms = 1000;
int16_t s_buf[kChunkFrames * 2];

void playTone(uint16_t hz, uint16_t ms) {
  const uint32_t rate = kDefaultRate;
  if (!s_out->setFormat(rate, 16, 2)) return;
  s_out->unmute();                          // fade-in робить AudioOut
  const float step = 2.0f * (float)M_PI * (float)hz / (float)rate;
  float ph = 0;
  const uint32_t total = (uint32_t)((uint64_t)rate * ms / 1000);
  for (uint32_t done = 0; done < total;) {
    size_t n = total - done < kChunkFrames ? (size_t)(total - done) : kChunkFrames;
    for (size_t i = 0; i < n; i++) {
      int16_t v = (int16_t)(sinf(ph) * kToneAmpl * 32767.0f);
      ph += step;
      if (ph > 2.0f * (float)M_PI) ph -= 2.0f * (float)M_PI;
      s_buf[2 * i] = s_buf[2 * i + 1] = v;
    }
    size_t w = s_out->write(s_buf, n * 4, 200);
    if (w == 0) break;
    done += w / 4;
  }
  s_out->mute();                            // хвіст fade-out без клацання
}

void taskFn(void*) {
  for (;;) {
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    LOGI(TAG, "тон %u Гц %u мс", (unsigned)s_hz, (unsigned)s_ms);
    playTone(s_hz, s_ms);
    Diag::logHeap("tone end");
    s_busy = false;
  }
}
}  // namespace

void AudioTask::begin(AudioOut& out) {
  s_out = &out;
  xTaskCreatePinnedToCore(taskFn, "audio", kStackAudio, nullptr, kPrioAudio, &s_task, kCoreAudio);
  Diag::registerTask("audio", s_task);
}

bool AudioTask::busy() { return s_busy; }

bool AudioTask::requestTone(uint16_t hz, uint16_t ms) {
  if (!s_task || s_busy) return false;
  s_hz = hz;
  s_ms = ms;
  s_busy = true;
  xTaskNotifyGive(s_task);
  return true;
}
#include "audio/audio_task.h"

#include <Arduino.h>
#include <math.h>

#include "esp_timer.h"

#include "audio/pcm_ring.h"
#include "config.h"
#include "util/diag.h"
#include "util/log.h"

namespace {
constexpr const char* TAG = "atask";
AudioOut* s_out = nullptr;
TaskHandle_t s_task = nullptr;
volatile bool s_busy = false;
volatile uint16_t s_hz = 440, s_ms = 1000;
int16_t s_buf[kChunkFrames * 2];

// ---- помпа PCM ----
PcmRing* s_ring = nullptr;
uint8_t s_pcm[kPumpBytes];
bool s_buffering = true;       // чекаємо пребуфер
uint64_t s_frames = 0;         // кадрів віддано від початку відтворення
int64_t s_t0 = 0;              // мкс, початок відтворення
uint32_t s_rate = kDefaultRate;
constexpr uint64_t kSlackFrames = (uint64_t)kI2sDmaCount * kI2sDmaLen;  // затримка DMA

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

void pump() {
  PcmRing* r = s_ring;
  if (!r) return;
  if (!r->accepting()) {                    // джерело неактивне
    r->flush();
    s_buffering = true;
    return;
  }
  uint32_t hz = r->takePendingRate();       // зміна частоти: з мʼютом (setFormat), кільце скидаємо
  if (hz) {
    r->flush();
    if (s_out->setFormat(hz, 16, 2)) s_rate = hz;
    s_buffering = true;
  }
  if (s_out->isMuted()) {                   // DAC замʼючений — дані не потрібні
    r->flush();
    s_buffering = true;
    return;
  }
  size_t avail = r->available();
  int64_t now = esp_timer_get_time();
  if (s_buffering) {
    if (avail < kBtPrebufferBytes) return;
    s_buffering = false;
    s_frames = 0;
    s_t0 = now;
  } else {
    // I2S-голодування: «програно» за годинником більше, ніж віддано (з запасом на DMA)
    uint64_t played = (uint64_t)(now - s_t0) * s_rate / 1000000ULL;
    if (played > s_frames + kSlackFrames) {
      r->underruns++;
      s_buffering = true;
      return;
    }
  }
  for (;;) {
    size_t n = r->read(s_pcm, sizeof s_pcm);
    if (!n) break;
    int64_t t = esp_timer_get_time();
    size_t w = s_out->write(s_pcm, n, kPcmWriteTimeoutMs);
    uint32_t dt = (uint32_t)(esp_timer_get_time() - t);
    if (dt > r->maxWriteUs) r->maxWriteUs = dt;
    r->outBytes += (uint32_t)w;
    s_frames += w / 4;
    if (w < n) {                            // I2S не прийняв за ліміт: залишок втрачено
      r->writeTimeouts++;
      break;
    }
  }
}

void taskFn(void*) {
  for (;;) {
    ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(kPumpIdleMs));
    if (s_busy) {
      LOGI(TAG, "тон %u Гц %u мс", (unsigned)s_hz, (unsigned)s_ms);
      playTone(s_hz, s_ms);
      Diag::logHeap("tone end");
      s_busy = false;
      continue;
    }
    pump();
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

void AudioTask::attachStream(PcmRing* ring) { s_ring = ring; }

void AudioTask::wake() {
  if (s_task) xTaskNotifyGive(s_task);
}
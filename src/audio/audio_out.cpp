#include "audio/audio_out.h"

#include <Arduino.h>
#include "driver/i2s.h"

#include "pins.h"
#include "util/log.h"

namespace {
constexpr const char* TAG = "audio";
constexpr i2s_port_t kPort = I2S_NUM_0;

struct Lock {
  SemaphoreHandle_t m;
  bool ok;
  Lock(SemaphoreHandle_t mtx, TickType_t t) : m(mtx), ok(mtx && xSemaphoreTakeRecursive(mtx, t) == pdTRUE) {}
  ~Lock() { if (ok) xSemaphoreGiveRecursive(m); }
};
}  // namespace

void AudioOut::earlyMute() {
  pinMode(PIN_XSMT, OUTPUT);
  digitalWrite(PIN_XSMT, LOW);
}

void AudioOut::setXsmt(bool unmuted) { digitalWrite(PIN_XSMT, unmuted ? HIGH : LOW); }

void AudioOut::computeStep() {
  uint32_t n = rate_ * kFadeMs / 1000;
  step_ = n ? (int32_t)(32768 / n) : 32768;
  if (step_ < 1) step_ = 1;
}

bool AudioOut::begin() {
  mtx_ = xSemaphoreCreateRecursiveMutex();
  i2s_config_t cfg = {};
  cfg.mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX);
  cfg.sample_rate = rate_;
  cfg.bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT;
  cfg.channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT;
  cfg.communication_format = I2S_COMM_FORMAT_STAND_I2S;
  cfg.intr_alloc_flags = ESP_INTR_FLAG_LEVEL1;
  cfg.dma_buf_count = kI2sDmaCount;
  cfg.dma_buf_len = kI2sDmaLen;
  cfg.use_apll = false;
  cfg.tx_desc_auto_clear = true;   // при нестачі даних DMA віддає тишу
  cfg.fixed_mclk = 0;
  if (i2s_driver_install(kPort, &cfg, 0, nullptr) != ESP_OK) {
    LOGE(TAG, "i2s_driver_install не вдалося");
    return false;
  }
  i2s_pin_config_t pins = {};
  pins.mck_io_num = I2S_PIN_NO_CHANGE;   // PCM5102: SCK на GND (внутрішній PLL)
  pins.bck_io_num = PIN_I2S_BCK;
  pins.ws_io_num = PIN_I2S_WS;
  pins.data_out_num = PIN_I2S_DOUT;
  pins.data_in_num = I2S_PIN_NO_CHANGE;
  i2s_set_pin(kPort, &pins);
  i2s_zero_dma_buffer(kPort);
  setXsmt(false);
  computeStep();
  ready_ = true;
  muted_ = true;
  LOGI(TAG, "I2S готовий (%u Гц 16 біт стерео), DAC замʼючено", (unsigned)rate_);
  return true;
}

void AudioOut::mute() {
  Lock l(mtx_, portMAX_DELAY);
  if (!ready_) return;
  if (!muted_) {
    // Хвіст fade-out: від останнього кадру лінійно до нуля, без участі продюсера
    uint32_t n = rate_ * kFadeMs / 1000;
    if (n < 1) n = 1;
    for (uint32_t done = 0; done < n;) {
      size_t m = n - done < kChunkFrames ? (size_t)(n - done) : kChunkFrames;
      for (size_t i = 0; i < m; i++) {
        int32_t k = (int32_t)(n - (done + i));
        buf_[2 * i] = (int16_t)((int32_t)lastL_ * k / (int32_t)n);
        buf_[2 * i + 1] = (int16_t)((int32_t)lastR_ * k / (int32_t)n);
      }
      size_t wr = 0;
      i2s_write(kPort, buf_, m * 4, &wr, pdMS_TO_TICKS(50));
      if (wr < m * 4) break;
      done += m;
    }
    setXsmt(false);
    muted_ = true;
    vTaskDelay(pdMS_TO_TICKS(kMuteSettleMs));
    i2s_zero_dma_buffer(kPort);
    lastL_ = lastR_ = 0;
  } else {
    setXsmt(false);
  }
}

void AudioOut::unmute() {
  Lock l(mtx_, portMAX_DELAY);
  if (!ready_ || !muted_) return;
  i2s_zero_dma_buffer(kPort);
  gain_ = 0;                          // fade-in на перших кадрах write()
  setXsmt(true);
  vTaskDelay(pdMS_TO_TICKS(kUnmuteSettleMs));
  muted_ = false;
}

bool AudioOut::setFormat(uint32_t rate, uint8_t bits, uint8_t channels) {
  if (bits != 16 || (channels != 1 && channels != 2) || rate < 8000 || rate > 96000) {
    LOGW(TAG, "формат не підтримується: %u Гц %u біт %u кан", (unsigned)rate, bits, channels);
    return false;
  }
  Lock l(mtx_, portMAX_DELAY);
  if (!ready_) return false;
  bool wasOn = !muted_;
  if (wasOn) mute();
  if (rate != rate_) i2s_set_clk(kPort, rate, I2S_BITS_PER_SAMPLE_16BIT, I2S_CHANNEL_STEREO);
  rate_ = rate;
  ch_ = channels;
  computeStep();
  if (wasOn) unmute();
  return true;
}

size_t AudioOut::write(const void* data, size_t bytes, uint32_t timeoutMs) {
  if (!ready_ || bytes == 0) return 0;
  if (muted_) return bytes;           // замʼючено — відкидаємо
  Lock l(mtx_, pdMS_TO_TICKS(timeoutMs));
  if (!l.ok) return 0;
  if (muted_) return bytes;

  const int16_t* in = (const int16_t*)data;
  size_t fb = (size_t)ch_ * 2, frames = bytes / fb, consumed = 0;
  while (frames) {
    size_t n = frames < kChunkFrames ? frames : kChunkFrames;
    for (size_t i = 0; i < n; i++) {
      int32_t lv, rv;
      if (ch_ == 1) { lv = rv = in[i]; }
      else { lv = in[2 * i]; rv = in[2 * i + 1]; }
      if (gain_ < 32768) {            // fade-in після unmute
        lv = (lv * gain_) >> 15;
        rv = (rv * gain_) >> 15;
        gain_ += step_;
        if (gain_ > 32768) gain_ = 32768;
      }
      buf_[2 * i] = (int16_t)lv;
      buf_[2 * i + 1] = (int16_t)rv;
    }
    size_t wr = 0;
    esp_err_t e = i2s_write(kPort, buf_, n * 4, &wr, pdMS_TO_TICKS(timeoutMs));
    size_t wf = wr / 4;
    if (wf) {
      lastL_ = buf_[2 * (wf - 1)];
      lastR_ = buf_[2 * (wf - 1) + 1];
    }
    consumed += wf * fb;
    if (e != ESP_OK || wf < n) break;
    frames -= n;
    in += n * ch_;
  }
  return consumed;
}

void AudioOut::stop() {
  mute();
  Lock l(mtx_, portMAX_DELAY);
  if (ready_) i2s_zero_dma_buffer(kPort);
}
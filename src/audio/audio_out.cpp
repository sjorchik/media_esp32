#include "audio/audio_out.h"

#include <Arduino.h>
#include "driver/i2s.h"
#include "freertos/task.h"

#include "pins.h"
#include "util/log.h"

namespace {
constexpr const char* TAG = "audio";
constexpr i2s_port_t kPort = I2S_NUM_0;

// Рівень на GPIO27: PCM5102 XSMT (HIGH=звук) або UDA1334A MUTE (HIGH=мʼют)
constexpr int levelFor(bool unmuted) {
#if DAC_MUTE_ACTIVE_HIGH
  return unmuted ? LOW : HIGH;
#else
  return unmuted ? HIGH : LOW;
#endif
}

// Запит на мʼют: write() негайно віддає мʼютекс. Без цього задача audio (вищий пріоритет) знову
// захоплює мʼютекс у кожному write(), і mute() з app-задачі чекав секундами.
// M5c2/H1: лічильник глибини — вкладений mute() (setFormat у задачі mp3) не скидає abort чужого mute() з app.
volatile uint32_t s_abortDepth = 0;
inline bool abortOn() { return s_abortDepth != 0; }
struct AbortGuard {
  AbortGuard() { __atomic_fetch_add(&s_abortDepth, 1u, __ATOMIC_SEQ_CST); }
  ~AbortGuard() { __atomic_fetch_sub(&s_abortDepth, 1u, __ATOMIC_SEQ_CST); }
};

// Діагностика мʼютекса (M5c1): хто тримає й хто чекає. s_owner/s_depth/s_op пише лише власник мʼютекса,
// s_waitMs/s_waitOp — той, хто чекає без таймауту; DBG,aout читає без блокувань.
volatile TaskHandle_t s_owner = nullptr;
volatile uint8_t s_depth = 0;
volatile char s_op = 0;
volatile uint32_t s_waitMs = 0;   // millis()|1 від початку очікування (0 = ніхто не чекає; при кількох чекальниках — останній)
volatile char s_waitOp = 0;
uint32_t s_waitMaxMs = 0;

struct Lock {
  SemaphoreHandle_t m;
  bool ok;
  Lock(SemaphoreHandle_t mtx, TickType_t t, char op) : m(mtx), ok(false) {
    if (!mtx) return;
    const bool block = (t == portMAX_DELAY);
    uint32_t t0 = 0;
    if (block) {
      t0 = millis();
      s_waitOp = op;
      s_waitMs = t0 | 1u;
    }
    ok = xSemaphoreTakeRecursive(mtx, t) == pdTRUE;
    if (block) {
      s_waitMs = 0;
      uint32_t w = millis() - t0;
      if (ok && w > s_waitMaxMs) s_waitMaxMs = w;
    }
    if (ok) {
      if (s_depth == 0) s_owner = xTaskGetCurrentTaskHandle();
      s_depth = (uint8_t)(s_depth + 1);
      s_op = op;
    }
  }
  ~Lock() {
    if (!ok) return;
    s_depth = (uint8_t)(s_depth - 1);
    if (s_depth == 0) s_owner = nullptr;
    xSemaphoreGiveRecursive(m);
  }
};
}  // namespace

void AudioOut::earlyMute() {
  digitalWrite(PIN_XSMT, levelFor(false));  // рівень мʼюту до вмикання виходу
  pinMode(PIN_XSMT, OUTPUT);
  digitalWrite(PIN_XSMT, levelFor(false));
}

bool AudioOut::aborting() const { return abortOn(); }

void AudioOut::mark(char kind) {
  cnt_.lastTrans = kind;
  cnt_.lastTransMs = millis();
}

void AudioOut::dbgInfo(DbgInfo& o) const {
  const uint32_t now = millis();
  o.ready = ready_;
  o.muted = muted_;
  o.aborting = abortOn();
  o.channels = ch_;
  o.rate = rate_;
  o.gainPct = (uint8_t)((int64_t)gain_ * 100 / 32768);
  o.muteCalls = cnt_.muteCalls;
  o.muteDone = cnt_.muteDone;
  o.unmuteCalls = cnt_.unmuteCalls;
  o.unmuteDone = cnt_.unmuteDone;
  o.fmtCalls = cnt_.fmtCalls;
  o.stopCalls = cnt_.stopCalls;
  o.wDropped = cnt_.wDropped;
  o.wLockFail = cnt_.wLockFail;
  o.wPartial = cnt_.wPartial;
  o.lastTrans = cnt_.lastTrans;
  o.lastTransAgoMs = cnt_.lastTrans ? now - cnt_.lastTransMs : 0xFFFFFFFFu;
  const uint32_t ws = s_waitMs;
  o.waitingMs = ws ? now - (ws & ~1u) : 0;
  o.waitingOp = ws ? s_waitOp : 0;
  o.lockWaitMaxMs = s_waitMaxMs;
  TaskHandle_t h = s_owner;
  o.owner = h ? pcTaskGetName(h) : nullptr;
  o.ownerOp = s_op;
  o.dmaFrames = kDmaFrames;
}

void AudioOut::setXsmt(bool unmuted) { digitalWrite(PIN_XSMT, levelFor(unmuted)); }

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
  pins.mck_io_num = I2S_PIN_NO_CHANGE;   // PCM5102/UDA1334A: MCLK не потрібен
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
  LOGI(TAG, "I2S готовий (%u Гц 16 біт стерео), DAC замʼючено; мʼют: %s%s", (unsigned)rate_,
       DAC_MUTE_ACTIVE_HIGH ? "HIGH=мʼют (UDA1334A)" : "LOW=мʼют (PCM5102)",
       HAVE_DAC ? "" : "; DAC відсутній");
  return true;
}

void AudioOut::mute() {
  cnt_.muteCalls++;
  AbortGuard ag;                        // оголошено до Lock: скидається після відпускання мʼютекса
  Lock l(mtx_, portMAX_DELAY, 'm');
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
    cnt_.muteDone++;
    mark('M');
    vTaskDelay(pdMS_TO_TICKS(kMuteSettleMs));
    i2s_zero_dma_buffer(kPort);
    lastL_ = lastR_ = 0;
  } else {
    setXsmt(false);
  }
}

void AudioOut::unmute() {
  cnt_.unmuteCalls++;
  Lock l(mtx_, portMAX_DELAY, 'u');
  if (!ready_ || !muted_) return;
  i2s_zero_dma_buffer(kPort);
  gain_ = 0;                          // fade-in на перших кадрах write()
  setXsmt(true);
  vTaskDelay(pdMS_TO_TICKS(kUnmuteSettleMs));
  muted_ = false;
  cnt_.unmuteDone++;
  mark('U');
}

bool AudioOut::setFormat(uint32_t rate, uint8_t bits, uint8_t channels) {
  if (bits != 16 || (channels != 1 && channels != 2) || rate < 8000 || rate > 96000) {
    LOGW(TAG, "формат не підтримується: %u Гц %u біт %u кан", (unsigned)rate, bits, channels);
    return false;
  }
  cnt_.fmtCalls++;
  Lock l(mtx_, portMAX_DELAY, 'f');
  if (!ready_) return false;
  bool wasOn = !muted_;
  if (wasOn) mute();
  if (rate != rate_) i2s_set_clk(kPort, rate, I2S_BITS_PER_SAMPLE_16BIT, I2S_CHANNEL_STEREO);
  rate_ = rate;
  ch_ = channels;
  computeStep();
  if (wasOn) unmute();
  mark('F');
  return true;
}

size_t AudioOut::write(const void* data, size_t bytes, uint32_t timeoutMs) {
  if (!ready_ || bytes == 0) return 0;
  if (muted_ || abortOn()) {               // замʼючено або йде мʼют — відкидаємо
    cnt_.wDropped++;
    return bytes;
  }
  Lock l(mtx_, pdMS_TO_TICKS(timeoutMs), 'w');
  if (!l.ok) {
    cnt_.wLockFail++;
    return 0;
  }
  if (muted_ || abortOn()) {
    cnt_.wDropped++;
    return bytes;
  }

  const int16_t* in = (const int16_t*)data;
  size_t fb = (size_t)ch_ * 2, frames = bytes / fb, consumed = 0;
  while (frames) {
    if (abortOn()) {                       // між порціями віддаємо мʼютекс для mute()
      cnt_.wDropped++;
      return bytes;
    }
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
    if (e != ESP_OK || wf < n) {
      cnt_.wPartial++;
      break;
    }
    frames -= n;
    in += n * ch_;
  }
  return consumed;
}

void AudioOut::stop() {
  cnt_.stopCalls++;
  mute();
  Lock l(mtx_, portMAX_DELAY, 's');
  if (ready_) i2s_zero_dma_buffer(kPort);
}


#pragma once
#include <stddef.h>
#include <stdint.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "config.h"

// Єдиний власник I2S і XSMT. Потокобезпечно (рекурсивний мʼютекс).
// Формат: I2S завжди 16 біт стерео; setFormat приймає bits=16, channels=1|2 (моно дублюється програмно).
// Продюсер write(): задача audio (BT) або задача mp3 (SD) — джерела ексклюзивні, тож одночасно лише один.
// Правило: один продюсер write(); setFormat/stop — з контексту продюсера або коли він простоює
// (setFormat сам мʼютить DAC, якщо той був увімкнений).
class AudioOut {
 public:
  static void earlyMute();            // XSMT=LOW, викликати першим рядком setup()

  bool begin();                       // ставить драйвер I2S, DAC лишається замʼючений
  void mute();                        // хвіст fade-out → XSMT LOW → затримка → очищення DMA
  void unmute();                      // XSMT HIGH → затримка → fade-in на перших кадрах write()
  bool isMuted() const { return muted_; }
  bool aborting() const;                       // true, поки йде mute(): продюсер має почекати (відпустити CPU)
  uint32_t rate() const { return rate_; }      // поточний формат I2S (для рішення «чи потрібен setFormat»)
  uint8_t channels() const { return ch_; }
  bool setFormat(uint32_t rate, uint8_t bits, uint8_t channels);
  // Повертає кількість спожитих байтів вхідних даних. Якщо DAC замʼючений — дані відкидаються
  // (повертає bytes). Блокується максимум timeoutMs.
  size_t write(const void* data, size_t bytes, uint32_t timeoutMs = 100);
  void stop();                        // mute + скидання буферів; тактування I2S продовжується

 private:
  void setXsmt(bool unmuted);
  void computeStep();

  SemaphoreHandle_t mtx_ = nullptr;
  volatile bool muted_ = true;
  bool ready_ = false;
  uint32_t rate_ = kDefaultRate;
  uint8_t ch_ = 2;
  int32_t gain_ = 0;                  // Q15, 0..32768
  int32_t step_ = 1;
  int16_t lastL_ = 0, lastR_ = 0;
  int16_t buf_[kChunkFrames * 2];
};

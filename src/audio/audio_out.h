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

  // M5c1: діагностика для DBG,aout. Без блокувань, з будь-якої задачі (значення «на льоту», лише для лога).
  struct DbgInfo {
    bool ready, muted, aborting;
    uint8_t channels, gainPct;
    uint32_t rate;
    uint32_t muteCalls, muteDone, unmuteCalls, unmuteDone, fmtCalls, stopCalls;  // виклики / реальні переходи
    uint32_t wDropped, wLockFail, wPartial;   // write(): відкинуто (мʼют/abort); не взято мʼютекс; неповний запис
    uint32_t lastTransAgoMs;                  // скільки мс тому була остання зміна (0xFFFFFFFF = ще не було)
    char lastTrans;                           // 'M' мʼют, 'U' анмʼют, 'F' setFormat, 0 = не було
    uint32_t waitingMs;                       // >0: хтось уже стільки мс чекає мʼютекс без таймауту (ознака завислого власника)
    char waitingOp;
    uint32_t lockWaitMaxMs;                   // макс. очікування мʼютекса в mute()/unmute()/setFormat()/stop()
    const char* owner;                        // задача, що тримає мʼютекс зараз (nullptr = вільний)
    char ownerOp;                             // остання операція під мʼютексом: w write, m mute, u unmute, f setFormat, s stop
    uint32_t dmaFrames;                       // глибина DMA, кадрів
  };
  void dbgInfo(DbgInfo& o) const;

 private:
  void setXsmt(bool unmuted);
  void computeStep();
  void mark(char kind);               // запис «остання зміна» для dbgInfo

  SemaphoreHandle_t mtx_ = nullptr;
  volatile bool muted_ = true;
  bool ready_ = false;
  uint32_t rate_ = kDefaultRate;
  uint8_t ch_ = 2;
  int32_t gain_ = 0;                  // Q15, 0..32768
  int32_t step_ = 1;
  int16_t lastL_ = 0, lastR_ = 0;
  int16_t buf_[kChunkFrames * 2];
  struct Cnt {                        // лічильники діагностики (M5c1), ≈ 44 Б
    uint32_t muteCalls, muteDone, unmuteCalls, unmuteDone, fmtCalls, stopCalls;
    uint32_t wDropped, wLockFail, wPartial, lastTransMs;
    char lastTrans;
  } cnt_{};
};

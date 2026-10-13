#pragma once
#include <stddef.h>
#include <stdint.h>

#include "config.h"
#include "sources/isource.h"

// BT-джерело: A2DP sink (SBC) + AVRCP CT. Бібліотека pschatzmann/ESP32-A2DP без власного I2S:
// PCM йде в кільце → задача audio → AudioOut. Усі методи — з задачі app; BT-колбеки лише
// кладуть події в Inbox. Стек піднімається один раз (startStack) і далі не переініціалізується.
class BtSource : public ISource {
 public:
  SrcId id() const override { return SrcId::BT; }
  bool begin() override;          // кільце, NVS (збережений пристрій); стек ще не стартує
  void startStack();              // після Link::begin(): піднімає BT і лишає ініціалізованим
  Res activate() override;
  void deactivate() override;
  Res play() override;
  Res pause() override;
  Res stop() override;
  Res next() override;
  Res prev() override;
  Res fnext() override { return Res::BadState; }
  Res fprev() override { return Res::BadState; }
  void setRepeat(uint8_t) override {}
  void setShuffle(bool) override {}   // SHUF для bt зберігається менеджером, на відтворення не впливає
  SourceStatus status() const override;
  void loop(uint32_t nowMs) override;
  void idleLoop(uint32_t nowMs) override { service(nowMs); }
  void emitInfo() override;

  // ---- налагодження (DBG,bt…) — потокобезпечні прапорці/знімок ----
  void dump() const;
  void requestPair() { pairReq_ = true; }
  void requestForget() { forgetReq_ = true; }
  void resetStats() { resetReq_ = true; }

 private:
  void service(uint32_t now);
  PlayState derive() const;
  uint32_t posMsAt(uint32_t now) const;
  void sendTime(uint32_t now);
  void onConnected(uint32_t now);
  void onDisconnected(uint32_t now);
  void applyScanMode(uint32_t now, bool force);
  void reportConn(bool connected);
  void savePeer();
  void saveName();
  void doForget(uint32_t now);
  void clearMeta();

  bool started_ = false, active_ = false, conn_ = false, connecting_ = false;
  bool havePeer_ = false, giveUp_ = false, suppress_ = false;
  bool nameReported_ = false, nameWatch_ = false;
  bool metaPending_ = false, metaChanged_ = false, metaForce_ = false, timeOnce_ = false;
  bool volFix_ = false, pairing_ = false, scanDisc_ = false, scanApplied_ = false;
  bool audioStarted_ = false;
  bool optPause_ = false;                  // оптимістична пауза: показуємо pause до підтвердження телефона
  volatile bool pairReq_ = false, forgetReq_ = false, resetReq_ = false;

  uint8_t peer_[6] = {0};
  uint8_t tries_ = 0, inactivePauses_ = 0;
  uint8_t avrc_ = 0xFE;                    // 0xFE = невідомо
  PlayState st_ = PlayState::Idle;
  uint16_t rate_ = 0;

  uint32_t nextTryAt_ = 0, connectAt_ = 0, nameDeadline_ = 0, nameWatchEnd_ = 0;
  uint32_t pairUntil_ = 0, scanReassertAt_ = 0, volFixAt_ = 0, lastVolSet_ = 0;
  uint32_t lastInactivePauseAt_ = 0, metaAt_ = 0, lastTimeAt_ = 0, optUntil_ = 0;
  uint32_t posBase_ = 0, posAt_ = 0, durS_ = 0, pendDurMs_ = 0;
  uint32_t statAt_ = 0, lastIn_ = 0, lastOut_ = 0, inRate_ = 0, outRate_ = 0;

  char name_[33] = {0}, savedName_[33] = {0};
  char title_[kBtMetaMax + 1] = {0}, artist_[kBtMetaMax + 1] = {0}, album_[kBtMetaMax + 1] = {0};
  char pT_[kBtMetaMax + 1] = {0}, pA_[kBtMetaMax + 1] = {0}, pAl_[kBtMetaMax + 1] = {0};
};
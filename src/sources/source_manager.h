#pragma once
#include "audio/audio_out.h"
#include "core/commands.h"
#include "core/types.h"
#include "sources/isource.h"

// Гарантує: активне лише одне джерело; deactivate() попереднього завершується до activate()
// нового; перемикання ідемпотентне. Викликається лише з задачі app.
class SourceManager {
 public:
  void begin(AudioOut* audio, ISource* bt, ISource* sd, ISource* usb);
  void handle(const Cmd& c);
  void loop(uint32_t nowMs);
  Res select(SrcId id);
  void resumeLast();                 // для AUTO_RESUME_SOURCE
  void onMediaChanged();
  void emitFullState();              // STATE, MEDIA, BT, [TRACK, FOLDER, META, TIME]
  void publishNow();                 // M4c2: надіслати STATE зараз (джерело змінило стан у loop() і хоче STATE раніше за свій ERR)

  SrcId current() const { return cur_; }
  PlayState state() const;
  bool shuffle() const { return shuf_; }
  uint8_t repeat() const { return rep_; }
#if ENABLE_USB_CH376
  // M4c1: app під час скану USB чекає чергу команд kScanFastWaitTicks замість ≈10 мс (main.cpp: див. report_M4c1.md, «що лишилось»).
  bool wantsFastLoop() const { return src_[(int)cur_] && src_[(int)cur_]->wantsFastLoop(); }
#endif

 private:
  void publish(bool force);          // STATE при зміні + політика мʼюту DAC
  void emitErr(Res r, const char* detail);
#if ENABLE_USB_CH376
  void presenceTick(uint32_t nowMs);  // M4c2: фонова присутність USB-флешки і MEDIA usb при зміні
#endif
  void saveLast(SrcId id);
  void persist();                    // set_v1: last_src + shuf + rep (лише якщо змінилось)

  AudioOut* audio_ = nullptr;
  ISource* src_[4] = {nullptr, nullptr, nullptr, nullptr};
  SrcId cur_ = SrcId::Off;
  bool shuf_ = false;
  uint8_t rep_ = 0;
  SrcId pubSrc_ = SrcId::Off;
  PlayState pubState_ = PlayState::Idle;
  bool pubShuf_ = false;
  uint8_t pubRep_ = 0;
  uint8_t lastSaved_ = 0;
};



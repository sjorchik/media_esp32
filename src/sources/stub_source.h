#pragma once
#include "sources/isource.h"

// Заглушка для BT/SD/USB: лише лог і правдоподібні статуси та події.
class StubSource : public ISource {
 public:
  explicit StubSource(SrcId id) : id_(id) {}
  SrcId id() const override { return id_; }
  bool begin() override;
  Res activate() override;
  void deactivate() override;
  Res play() override;
  Res pause() override;
  Res stop() override;
  Res next() override { return move(+1, false); }
  Res prev() override { return move(-1, false); }
  Res fnext() override { return move(+1, true); }
  Res fprev() override { return move(-1, true); }
  void setRepeat(uint8_t mode) override { rep_ = mode; }
  void setShuffle(bool on) override { shuf_ = on; }
  SourceStatus status() const override;
  void loop(uint32_t nowMs) override;
  void emitInfo() override;
  Res refreshMedia() override;

 private:
  bool isMedia() const { return id_ != SrcId::BT; }
  bool mediaPresent() const;
  Res move(int dir, bool folder);
  void sendInfo();
  void restartTrack();

  SrcId id_;
  PlayState st_ = PlayState::Idle;
  bool active_ = false, shuf_ = false, tagsPending_ = false;
  uint8_t rep_ = 0, track_ = 0;
  uint32_t pos_ = 0, dur_ = 180;
  uint32_t scanEnd_ = 0, nextTick_ = 0, tagsAt_ = 0, btAt_ = 0;
};
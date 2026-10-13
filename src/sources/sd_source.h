#pragma once
#include "media/imedia.h"
#include "media/playlist.h"
#include "media/scanner.h"
#include "sources/isource.h"

// SD-джерело (M3a): hot-plug, скан, плейлист. Відтворення — M3c (play/next/… → NotImpl).
// Працює через IMedia&, тому про SD не знає. Усе (скан, DBG-завдання) виконується в задачі app порціями.
class SdSource : public ISource {
 public:
  enum class Dbg : uint8_t { None, Info, Scan, List, Read };

  explicit SdSource(IMedia& media) : media_(media) {}
  SrcId id() const override { return SrcId::SD; }
  bool begin() override;
  Res activate() override;
  void deactivate() override;
  Res play() override;
  Res pause() override;
  Res stop() override;
  Res next() override { return nav(); }
  Res prev() override { return nav(); }
  Res fnext() override { return nav(); }
  Res fprev() override { return nav(); }
  void setRepeat(uint8_t) override {}
  void setShuffle(bool) override {}
  SourceStatus status() const override { return SourceStatus{st_, 0, 0}; }
  void loop(uint32_t nowMs) override;
  void idleLoop(uint32_t nowMs) override { service(nowMs); }
  void emitInfo() override;
  Res refreshMedia() override;

  // DBG (з будь-якої задачі): лише ставить запит; виконує app. false = попереднє ще виконується.
  bool dbg(Dbg k, int arg = 0);

 private:
  void service(uint32_t now);
  void onMedia(MediaEvent ev);
  void startScan();
  void scanStep();
  void finishScan(Scanner::St r);
  void stopJobs();
  void runDbg();
  void dbgInfo();
  void dbgScan();
  void dbgList();
  void dbgRead();
  void emitTrackFolder();
  Res nav();

  IMedia& media_;
  Playlist pl_;
  Scanner scanner_;
  PlayState st_ = PlayState::Idle;
  bool active_ = false, scanned_ = false;
  uint8_t scanRetry_ = 0;
  uint16_t cur_ = 0;

  // DBG-завдання
  volatile Dbg pend_ = Dbg::None;
  volatile int pendArg_ = 0;
  Dbg job_ = Dbg::None;
  int arg_ = 0;
  uint16_t listPos_ = 0, listEnd_ = 0;
  bool rdOpen_ = false;
  uint32_t rdBytes_ = 0, rdBusyUs_ = 0, rdT0_ = 0;
};

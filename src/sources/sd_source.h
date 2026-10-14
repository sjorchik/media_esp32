#pragma once
#include "audio/mp3_player.h"
#include "config.h"
#include "media/imedia.h"
#include "media/playlist.h"
#include "media/scanner.h"
#include "sources/isource.h"

// SD-джерело: hot-plug, скан, плейлист (M3a); відтворення MP3 (M3b); навігація, repeat, події треку,
// обробка помилок відтворення (M3c). Shuffle лише зберігається (логіка — M5a).
// Працює через IMedia&, тому про SD не знає. Усе (скан, DBG-завдання) виконується в задачі app порціями.
class SdSource : public ISource {
 public:
  enum class Dbg : uint8_t { None, Info, Scan, List, Read, Play, Decode, Nav };

  explicit SdSource(IMedia& media) : media_(media) {}
  SrcId id() const override { return SrcId::SD; }
  bool begin() override;
  Res activate() override;
  void deactivate() override;
  Res play() override;
  Res pause() override;
  Res stop() override;
  Res next() override { return nav(Nav::Next); }
  Res prev() override { return nav(Nav::Prev); }
  Res fnext() override { return nav(Nav::FNext); }
  Res fprev() override { return nav(Nav::FPrev); }
  void setRepeat(uint8_t mode) override { rep_ = mode <= 2 ? mode : 0; }  // застосовується одразу (читає onTrackEnd)
  void setShuffle(bool on) override { shuf_ = on; }                        // лише зберігається
  SourceStatus status() const override;
  void loop(uint32_t nowMs) override;
  void idleLoop(uint32_t nowMs) override { service(nowMs); }
  void emitInfo() override;
  Res refreshMedia() override;

  // DBG (з будь-якої задачі): лише ставить запит; виконує app. false = попереднє ще виконується.
  bool dbg(Dbg k, int arg = 0);

 private:
  enum class Nav : uint8_t { Next, Prev, FNext, FPrev };
  struct Hist {
    uint16_t from, to;  // індекси з 0
    char why;           // причина переходу (легенда — у dbgNav)
  };

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
  void dbgPlay();
  void dbgDecode();
  void dbgNav();
  // відтворення
  Res startTrack(uint16_t n);              // відкриває файл, запускає сесію; state=play; повертає ReadFail, якщо не відкрито
  Res playTrack(uint16_t n, char why);     // startTrack з автопропуском битих (ERR + наступний; 3 підряд → stop)
  bool countFail(Res r);                   // ERR + лічильник; false = ліміт, state=stop
  void onPlayError(Res r);                 // помилка під час гри: ERR і перехід до наступного
  void endSession();                       // мʼют → зупинка сесії → закриття файлу (ідемпотентно)
  void pollPlayer(uint32_t now);
  void onTrackEnd();                       // природний кінець (state вже stop): REP 0/1/2
  // навігація
  Res nav(Nav k);
  Res ready() const;                       // Ok або BadState (scan) / NoMedia / BadState (idle)
  uint16_t nextIdx(uint16_t i) const;      // циклічно
  uint16_t prevIdx(uint16_t i) const;      // циклічно
  void setCur(uint16_t n);                 // нова поточна позиція без запуску; зміна → події треку
  void note(char why, uint16_t to);        // запис в історію переходів
  // події
  void sendTime();                         // TIME за status()
  void emitTrackInfo();                    // TRACK, FOLDER, META, TIME
  void emitFolder();
  void emitMeta();

  IMedia& media_;
  Playlist pl_;
  Scanner scanner_;
  PlayState st_ = PlayState::Idle;
  bool active_ = false, scanned_ = false;
  uint8_t scanRetry_ = 0;
  uint16_t cur_ = 0;
  uint8_t rep_ = 0;
  bool shuf_ = false;
  uint8_t errRun_ = 0;                     // помилок відтворення підряд
  Mp3Player dec_;
  uint32_t nextTimeAt_ = 0, infoSeq_ = 0;

  Hist hist_[kNavHist] = {};
  uint8_t histHead_ = 0, histCnt_ = 0;

  // DBG-завдання
  volatile Dbg pend_ = Dbg::None;
  volatile int pendArg_ = 0;
  Dbg job_ = Dbg::None;
  int arg_ = 0;
  uint16_t listPos_ = 0, listEnd_ = 0;
  bool rdOpen_ = false;
  uint32_t rdBytes_ = 0, rdBusyUs_ = 0, rdT0_ = 0;
};

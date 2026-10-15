#pragma once
#include "audio/mp3_player.h"
#include "config.h"
#include "media/imedia.h"
#include "media/playlist.h"
#include "media/scanner.h"
#include "media/shuffle.h"
#include "sources/isource.h"

// SD-джерело: hot-plug, скан, плейлист (M3a); відтворення MP3 (M3b); навігація, repeat, події треку,
// обробка помилок відтворення (M3c); ID3-теги в META (M3d); shuffle без повторів та історія для PREV (M5a, media/shuffle);
// резюме треку й позиції в NVS (M5b1, storage/resume_store); hot-plug SD: виймання/вставлення/дубль, відкладений вердикт
// збою читання, сторожа «play без сесії» (M5c3).
// Працює через IMedia&, тому про SD не знає. Усе (скан, DBG-завдання) виконується в задачі app порціями.
class SdSource : public ISource {
 public:
  enum class Dbg : uint8_t { None, Info, Scan, List, Read, Play, Decode, Nav, Tag, Shuf, ShufTest, Resume, ResumeClear, FailNext };

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
  void setShuffle(bool on) override;                                       // SHUF: звук не переривається
  SourceStatus status() const override;
  void loop(uint32_t nowMs) override;
  void idleLoop(uint32_t nowMs) override { service(nowMs); }
  void emitInfo() override;
  Res refreshMedia() override;

  // DBG (з будь-якої задачі): лише ставить запит; виконує app. false = попереднє ще виконується.
  // FailNext: arg = n (0..255) | 0x100 для decode_fail (інакше read_fail); потрібен SKIP_TEST_CMDS=1.
  bool dbg(Dbg k, int arg = 0);

  // Читання стану для DBG,aout (без блокувань; з консольної задачі, навіть якщо app завис)
  const Mp3Player& dec() const { return dec_; }
  PlayState playState() const { return st_; }
  uint8_t errRun() const { return errRun_; }
  uint8_t failLeft() const { return failN_; }
  bool failIsDecode() const { return failMode_ != 0; }

  // M5c3: лічильники hot-plug для DBG,media (без блокувань, з консольної задачі)
  struct MediaCnt { uint16_t insOk, insDup, remOk, remDup, guard; };
  MediaCnt mediaCnt() const { return MediaCnt{insOk_, insDup_, remOk_, remDup_, guardN_}; }
  bool active() const { return active_; }

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
  void scanFailed();                       // M5c3: повтор скану або idle (після відкладеного вердикту)
  void susStep(uint32_t now);              // M5c3: кінець очікування Removed → звичайна обробка збою
  void stopJobs();
  void runDbg();
  void dbgInfo();
  void dbgScan();
  void dbgList();
  void dbgRead();
  void dbgPlay();
  void dbgDecode();
  void dbgNav();
  void dbgTag();
  void dbgShuf();
  void dbgShufTest();
  void dbgResume(bool clear);
  void dbgFailNext();
  // відтворення
  Res startTrack(uint16_t n, uint32_t off = 0, uint32_t ms = 0);  // відкриває файл, запускає сесію (з зсуву off, мс); state=play; ReadFail, якщо не відкрито
  Res playTrack(uint16_t n, char why, HAct act = HAct::Push, uint32_t off = 0, uint32_t ms = 0);  // startTrack з автопропуском битих (ERR + наступний з кола/каталогу; 3 підряд → stop); act — запис в історію після успіху; off/ms — лише для першої спроби
  bool countFail(Res r);                   // ERR + лічильник; false = ліміт, state=stop
  void onPlayError(Res r);                 // помилка під час гри: ERR і перехід до наступного
  void endSession();                       // мʼют → зупинка сесії → закриття файлу (ідемпотентно)
  void pollPlayer(uint32_t now);
  void onTrackEnd();                       // природний кінець (state вже stop): REP 0/1/2
  // навігація
  Res nav(Nav k);
  Res ready() const;                       // Ok або BadState (scan) / NoMedia / BadState (idle)
  void setCur(uint16_t n);                 // нова поточна позиція без запуску; зміна → події треку
  void note(char why, uint16_t to);        // запис в історію переходів
  // події
  void sendTime();                         // TIME за status()
  void emitTrackInfo(int32_t timeS = -1);  // TRACK, FOLDER, META, TIME (timeS ≥ 0 — показати збережену позицію замість 0)
  void emitFolder();
  void emitMeta();
  // резюме (M5b1)
  uint16_t loadResume(uint32_t& off, uint32_t& ms);  // індекс збереженого треку в плейлисті або kNone (тоді перший трек із нуля)
  bool rsmWrite(uint32_t off, uint32_t ms);           // запис (cur_, off, ms) у NVS; шлях > 255 Б → запис стирається
  void saveResume(bool periodic);                     // поточна позиція декодера (play/pause); periodic — лише якщо зсув ≥ кадру
  // теги (M3d)
  void takeTags(bool emit);                // забрати результат розбору сесії; emit — надіслати повторний META, якщо змінився
  void buildMeta(char* a, char* t, char* al);  // найкраще відоме: теги (якщо для cur_) або імʼя файлу; буфери kEvMetaFieldMax+1
  void pushMeta();                         // META лише якщо відрізняється від уже показаного

  IMedia& media_;
  Playlist pl_;
  Scanner scanner_;
  PlayState st_ = PlayState::Idle;
  bool active_ = false, scanned_ = false;
  uint8_t scanRetry_ = 0;
  // M5c3: збій читання (play) або скану може бути наслідком виймання картки; Removed приходить із затримкою антидребезгу.
  // Вердикт відкладаємо на kCardLossGraceMs: Removed → це не збій треку; інакше звичайна обробка.
  enum class Sus : uint8_t { None, Play, Scan };
  Sus sus_ = Sus::None;
  uint32_t susAt_ = 0;
  uint16_t insOk_ = 0, insDup_ = 0, remOk_ = 0, remDup_ = 0;  // події носія: прийнято / дубль
  uint8_t guardN_ = 0;                     // скільки разів спрацювала сторожа «play без сесії»
  uint16_t cur_ = 0;
  uint8_t rep_ = 0;
  ShufNav nav_;                            // shuffle-коло (перестановка в арені плейлиста), історія, політика вибору
  uint16_t histBuf_[kHistLen] = {};        // кільце історії (2 Б на запис)
  uint8_t errRun_ = 0;                     // помилок відтворення підряд
  uint8_t failN_ = 0;                      // M5c1: скільки наступних стартів треку штучно завершити помилкою (SKIP_TEST_CMDS)
  uint8_t failMode_ = 0;                   // 0 = read_fail, 1 = decode_fail
  Mp3Player dec_;
  uint32_t nextTimeAt_ = 0, infoSeq_ = 0;
  uint16_t tagTrack_ = 0xFFFF;             // для якого треку чинні dec_.tags() (0xFFFF = ні)
  uint32_t tagSeqSeen_ = 0;                // останній забраний dec_.tagSeq()
  uint32_t shownHash_ = 0;                 // хеш META, надісланого останнім (для порівняння «змінилось»)
  uint32_t rsmAt_ = 0;                     // коли наступний періодичний запис резюме
  uint32_t rsmLastOff_ = 0;                // зсув останнього запису (умова «≥ одного кадру»)
  uint16_t rsmWarn_ = 0xFFFF;              // для якого треку вже попередили «шлях задовгий»

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

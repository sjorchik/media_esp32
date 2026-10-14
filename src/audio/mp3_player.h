#pragma once
#include <stddef.h>
#include <stdint.h>

#include "media/id3.h"
#include "media/ifile_reader.h"

class AudioOut;

// Лічильники декодера (пише задача «mp3», читає app для DBG,decode). Усі поля 32-бітні.
struct Mp3Stats {
  uint32_t frames, skipped, resyncs;     // декодовано кадрів; проходи без PCM; втрата/віднова синхронізації
  uint32_t underruns, writeTimeouts, paced;   // paced: кадри, після яких довелось віддати CPU (write не блокував)
  uint32_t decAvgUs, decMaxUs, frameAvgUs;
  uint32_t loadAvgPm, loadMaxPm;         // час декодування / тривалість кадру, проміле
  uint32_t hz, ch, kbps;                 // останній кадр
  uint32_t pcmBytes;                     // розмір останнього PCM-кадру
  uint32_t inFill, inCap;                // заповнення вхідного буфера
  uint32_t rdBytes, rdBusyUs, rdMaxUs;   // читання файлу
};

// MP3-декодер SD (minimp3) в окремій задачі (ядро 1, prio kPrioDecoder). Конвеєр:
//   файл → вхідний буфер → minimp3 → PCM-кадр → AudioOut::write() (блокується на DMA = темп відтворення).
// Окремого PCM-кільця немає: задача «mp3» — єдиний продюсер write(), поки активне SD.
// Памʼять — з PcmArena (власник Sd). Методи керування — лише з задачі app; задача блокується на notify
// (стан Idle/Paused) або на i2s_write (Run), busy-loop немає.
class Mp3Player {
 public:
  enum class Ev : uint8_t { None, Eof, ReadFail, DecodeFail };
  enum class St : uint8_t { Idle, Run, Paused };

  void init(AudioOut& out, IFileReader& rd) { out_ = &out; rd_ = &rd; }
  bool start();                       // створює задачу (один раз); вимагає PcmArena=Sd
  void startSession();                // файл уже відкрито (pos 0); асинхронно
  bool pause();                       // синхронно; true = задача на паузі
  void resume();                      // асинхронно (задача чекає розмʼюту DAC сама)
  void stopSession();                 // синхронно; потім файл можна закривати
  bool sessionActive() const { return st_ != St::Idle; }
  St state() const { return st_; }
  Ev takeEvent();                     // подія кінця сесії (раз), None якщо нема

  uint32_t posMs() const { return posMs_; }
  uint32_t durMs() const { return durMs_; }       // 0 = невідома
  bool durEstimated() const { return durEst_; }   // true: за бітрейтом, не з Xing/VBRI
  uint32_t infoSeq() const { return infoSeq_; }   // змінюється, коли зʼявилась тривалість
  void stats(Mp3Stats& o) const;
  uint8_t* scratch(size_t& cap);      // вхідний буфер області для DBG,sdread/DBG,tag (лише коли немає сесії)

  // Теги (M3d): задача mp3 розбирає їх на старті сесії (до першого кадру) і піднімає tagSeq.
  // Після зміни tagSeq app читає tags() доти, доки не стартує наступна сесія.
  uint32_t tagSeq() const { return tagSeq_; }
  const Id3::Tags& tags() const { return tags_; }
  uint32_t tagUs() const { return tagUs_; }       // час розбору, мкс
  uint8_t tagMask() const { return tagMask_; }    // Id3::kFoundV2 | kFoundV1
  uint8_t tagVer() const { return tagVer_; }      // версія ID3v2 або 0

 private:
  static void taskEntry(void* self);
  void taskMain();
  void runSession();
  Ev decodeLoop();
  bool fill();
  bool skipId3();
  void parseTags();
  bool waitUnmuted();
  bool parkPaused();
  void writePcm(size_t bytes);
  void drainSilence();
  uint8_t takeReq();
  void finish(Ev e);

  AudioOut* out_ = nullptr;
  IFileReader* rd_ = nullptr;
  volatile St st_ = St::Idle;
  volatile uint8_t req_ = 0;
  volatile Ev ev_ = Ev::None;
  volatile uint32_t posMs_ = 0, durMs_ = 0, infoSeq_ = 0;
  volatile bool durEst_ = false;
  Mp3Stats stats_{};
  Id3::Tags tags_{};                         // ≈ 291 Б; копіюється з локальної структури в стеку задачі
  volatile uint32_t tagSeq_ = 0, tagUs_ = 0;
  volatile uint8_t tagMask_ = 0, tagVer_ = 0;
};


#pragma once
#include <stdint.h>

#include "config.h"
#include "media/imedia.h"
#include "media/playlist.h"

struct ScanStats {
  uint32_t wallMs = 0, busyMs = 0;   // загальний час / час, витрачений у step()
  uint32_t entries = 0;              // прочитано записів каталогів
  uint16_t tracks = 0, foldersTotal = 0, foldersWithTracks = 0;
  uint32_t hidden = 0;               // приховані та імена з "."
  uint32_t longName = 0;             // імʼя/шлях не вмістилися в буфер
  uint32_t deepDirs = 0;             // каталоги глибше kScanMaxDepth
  uint32_t dirErr = 0;               // каталоги, які не відкрилися
  uint32_t dropped = 0;              // mp3, що не влізли в арену (скан зупиняється на першому)
#if ENABLE_USB_CH376
  uint32_t advMaxMs = 0;             // M4c1: найдовший один крок (один виклик nextEntry/openDir), мс — верхня межа затримки реакції на команду
  uint32_t reread = 0;               // M4c1: каталоги, перечитані другим проходом (режим одного проходу: пул імен переповнено/не виділено)
#endif
  bool overflow = false;
};

// Обхід дерева порціями: step() робить роботу не довше budgetMs і повертає керування.
// Порядок: файли каталогу (у порядку записів FAT) → підкаталоги (у порядку записів), глибина ≤ kScanMaxDepth.
// Рівень 0 = корінь. Лише *.mp3 (регістр байдужий); імена на "." і hidden ігноруються.
// M4c1: режим одного проходу (setOnePass(true), лише USB): імена підкаталогів запамʼятовуються під час проходу по файлах
// у тимчасовому пулі на heap (kUsbScanSubPoolBytes, стек по рівнях; звільняється в finish()/abort()), а другий прохід по
// каталогу не робиться. Не влізло в пул (або heap не виділено) — цей каталог перечитується другим проходом, як раніше.
// Результат (треки, порядок, статистика, переповнення арени) той самий, що в двопрохідному режимі. За замовчуванням — два проходи (SD).
class Scanner {
 public:
  enum class St : uint8_t { Idle, Running, Done, Overflow, Failed };

  void start(IMedia& m, Playlist& pl);
  St step(uint32_t budgetMs);
  void abort();                      // закрити всі каталоги, звільнити пул, стан Idle
#if ENABLE_USB_CH376
  void setOnePass(bool on) { onePass_ = on; }  // M4c1: діє з наступного start()
  ~Scanner() { freePool(); }
#endif
  St state() const { return st_; }
  const ScanStats& stats() const { return stats_; }

 private:
  struct Level {
    int h;
    uint8_t pass;       // 0 = файли, 1 = підкаталоги
    uint16_t folder;    // індекс у Playlist або kNone (ще не зареєстрована)
    uint16_t pathLen;   // довжина path_ для цього каталогу
    uint16_t nameAt;    // початок останньої компоненти в path_
#if ENABLE_USB_CH376
    uint16_t subBeg;    // M4c1: діапазон імен підкаталогів у пулі [subBeg, subEnd); subPos — наступне до обробки
    uint16_t subEnd;
    uint16_t subPos;
    bool poolFull;      // пул не вмістив усіх імен цього каталогу → другий прохід перечитує каталог
#endif
  };
  Level mkLevel(int h, uint16_t pathLen, uint16_t nameAt) const;
  void advance();
  void popLevel();
  void closeAll();
  void finish();
  bool ensureFolders();
  bool descend(const char* name, size_t n);  // зайти в підкаталог name (перевірки глибини/довжини шляху, openDir)
#if ENABLE_USB_CH376
  bool poolPush(Level& L, const char* name, size_t n);
  void freePool();
#endif
  const char* dirPath() const { return pathLen_ ? path_ : "/"; }

  IMedia* m_ = nullptr;
  Playlist* pl_ = nullptr;
  St st_ = St::Idle;
  ScanStats stats_;
  uint32_t t0_ = 0;
  int depth_ = 0;
  uint16_t pathLen_ = 0;
  Level lv_[kScanMaxDepth + 1];
  char path_[kScanPathMax] = {0};
  MediaEntry e_;
#if ENABLE_USB_CH376
  bool onePass_ = false;
  uint8_t* pool_ = nullptr;   // heap, лише на час скану
  uint16_t poolTop_ = 0;
#endif
};


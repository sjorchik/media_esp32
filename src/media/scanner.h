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
  bool overflow = false;
};

// Обхід дерева порціями: step() робить роботу не довше budgetMs і повертає керування.
// Порядок: файли каталогу (у порядку записів FAT) → підкаталоги (у порядку записів), глибина ≤ kScanMaxDepth.
// Рівень 0 = корінь. Лише *.mp3 (регістр байдужий); імена на "." і hidden ігноруються.
class Scanner {
 public:
  enum class St : uint8_t { Idle, Running, Done, Overflow, Failed };

  void start(IMedia& m, Playlist& pl);
  St step(uint32_t budgetMs);
  void abort();                      // закрити всі каталоги, стан Idle
  St state() const { return st_; }
  const ScanStats& stats() const { return stats_; }

 private:
  struct Level {
    int h;
    uint8_t pass;       // 0 = файли, 1 = підкаталоги
    uint16_t folder;    // індекс у Playlist або kNone (ще не зареєстрована)
    uint16_t pathLen;   // довжина path_ для цього каталогу
    uint16_t nameAt;    // початок останньої компоненти в path_
  };
  void advance();
  void popLevel();
  void closeAll();
  void finish();
  bool ensureFolders();
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
};

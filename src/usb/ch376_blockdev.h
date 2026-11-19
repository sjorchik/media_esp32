#pragma once
#include <stddef.h>
#include <stdint.h>

#include "config.h"
#include "usb/ch376_disk.h"
#include "usb/sector_cache.h"  // M4c1: порожній, якщо ENABLE_USB_CH376=0

// Ядро блочного пристрою над Ch376Disk (M4b3). Чистий C++: без Arduino.h і без SdFat, тестується в native.
// Сектор 0 пристрою = перший сектор розділу (зсув lbaOffset враховано тут), тож SdFat монтує його як «без таблиці розділів».
// MBR і GPT обробляються однаково: зсув і розмір розділу задає викликач (з disk_probe). Лише читання, запис не підтримується.
// Читання розбивається на DISK_READ по kCh376SectorsPerCmd секторів (хвіст — окремою командою).
// M4c1: необовʼязковий LRU-кеш секторів (SectorCache) на heap лише на час скану: cacheOpen() / cacheClose(). Запит одного сектора
// іде через кеш (попадання без чіпа; промах при послідовному доступі читає до readAhead секторів однією командою); запити n > 1
// (читання файлу) кеш обходять. Без кеша поведінка й лічильники як у M4b3. Кеш недійсний після setPartition() і markGone().
//
// Стан «пристрій відсутній» (Gone): будь-який збій чіпа (Disconnected, NoDisk, ReadError, Timeout, NoReply, BadReply,
// Aborted, ChipStatus) переводить у Gone, і він ЛИПКИЙ: усі наступні читання одразу повертають false без звернення до чіпа
// (чиста помилка для SdFat, без очікувань). Вихід — лише setPartition() (новий open). Помилки аргументів (за межами
// розділу, 0 секторів, nullptr) — false, але стан не змінюють. Усі виклики — з однієї задачі.

struct BlockDevStats {
  uint32_t calls = 0;        // викликів readSector/readSectors з коректними аргументами
  uint32_t sectors = 0;      // секторів прочитано успішно
  uint32_t cmds = 0;         // DISK_READ-команд, надісланих у Ch376Disk (разом із невдалими)
  uint32_t errors = 0;       // збої чіпа (рівно один, далі стан Gone)
  uint32_t rangeErrors = 0;  // виклики за межами розділу / 0 секторів / nullptr
  uint32_t rejected = 0;     // виклики, відхилені, бо пристрій уже Gone
};

class Ch376BlockDev {
 public:
  explicit Ch376BlockDev(Ch376Disk& d) : d_(d) {}
#if ENABLE_USB_CH376
  ~Ch376BlockDev() { cacheClose(); }
  Ch376BlockDev(const Ch376BlockDev&) = delete;
  Ch376BlockDev& operator=(const Ch376BlockDev&) = delete;
#endif

  // Розділ: перший сектор LBA (на диску) і довжина в секторах. Сектори поза [0, 2^32) обрізаються. Скидає стан Gone, помилку
  // й статистику; ставить Ch376Disk::setSecPerCmd(kCh376SectorsPerCmd).
  void setPartition(uint32_t lbaOffset, uint32_t sectors);

  bool readSector(uint32_t sector, uint8_t* dst);
  bool readSectors(uint32_t sector, uint8_t* dst, size_t n);

  uint32_t sectorCount() const { return sectors_; }
  uint32_t lbaOffset() const { return off_; }

  bool present() const { return !gone_; }     // false — пристрій відсутній (липко)
  DiskErr lastError() const { return err_; }  // збій, що перевів у Gone (Ok — не було)
  void markGone(DiskErr e);                   // зовнішнє визнання «відсутній» (e != Ok)

  const BlockDevStats& stats() const { return st_; }
  Ch376Disk& disk() { return d_; }

#if ENABLE_USB_CH376
  // M4c1. Кеш секторів: slots 0 = лише лічильники («тінь»), 1..8 — LRU-кеш; readAhead — секторів за промах при послідовному доступі.
  // false — не вистачило heap (читання йдуть без кеша). Повторний виклик скидає кеш і статистику.
  bool cacheOpen(uint8_t slots, uint8_t readAhead);
  void cacheClose();  // звільняє heap (ідемпотентно)
  bool cacheOn() const { return cache_ != nullptr; }
  // Розширення/зміна розміру кешу без втрати лічильників: новий кеш виділяється ДО звільнення старого; false — лишається старий.
  bool cacheResize(uint8_t slots, uint8_t readAhead);
  uint8_t cacheSlots() const { return cache_ ? cache_->slots() : 0; }
  const SectorCacheStats* cacheStats() const { return cache_ ? &cache_->stats() : nullptr; }
#endif

  // Чи переводить цей код у Gone (усе, крім Ok і BadArg).
  static bool isDeviceFailure(DiskErr e) { return e != DiskErr::Ok && e != DiskErr::BadArg; }

 private:
  bool fail(DiskErr e);
#if ENABLE_USB_CH376
  bool fetch(uint32_t sector, uint8_t* dst, size_t n);  // читання з чіпа (розбиття по kCh376SectorsPerCmd), лічильники, Gone
  bool cachedRead(uint32_t sector, uint8_t* dst);
#endif

  Ch376Disk& d_;
  uint32_t off_ = 0;
  uint32_t sectors_ = 0;
  bool gone_ = false;
  DiskErr err_ = DiskErr::Ok;
  BlockDevStats st_;
#if ENABLE_USB_CH376
  SectorCache* cache_ = nullptr;
#endif
};


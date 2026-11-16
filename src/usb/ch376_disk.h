#pragma once
#include <stddef.h>
#include <stdint.h>

#include "config.h"
#include "usb/ch376_link.h"

// Шар секторів CH376S (M4b2): DISK_CAPACITY і DISK_READ блоками по 64 Б. Залежить ЛИШЕ від Ch376Link:
// без Ch376 / Ch376Fs і без UART-специфіки (її лишено в ch376_uart.cpp). Можлива SPI-реалізація Ch376Link
// не потребує змін цього файлу. Чистий C++, без Arduino.h. Усі виклики — з однієї задачі.
// Значення протоколу — в config.h (таблиця «Припущення з даташита» у звіті, перевірка на залізі).

enum class DiskErr : uint8_t {
  Ok,
  NoReply,       // GET_STATUS або відповідь RD_USB_DATA0 не прийшли
  BadReply,      // довжина блоку ≠ 64 або відповідь DISK_CAPACITY неможлива
  Timeout,       // подія або блок даних не прийшли вчасно (у т.ч. обрізана відповідь)
  Aborted,       // link попросив перервати
  Disconnected,  // флешку витягнуто (USB_INT_DISCONNECT)
  NoDisk,        // диска немає (ERR_DISK_DISCON)
  ReadError,     // помилка читання диска (USB_INT_DISK_ERR / переповнення буфера)
  BadArg,        // аргументи поза межами: 0 секторів, LBA+n за межами відомої ємності
  ChipStatus,    // чіп повернув невідомий статус (код — lastStatus())
};

const char* diskErrName(DiskErr e);

// Лічильники для DBG,usb,rawread|rawspeed|rawseq|rawpre. Час блоку — від очікування 0x1D до прийому даних блоку.
struct DiskStats {
  uint32_t cmds = 0;                              // успішних DISK_READ-команд
  uint32_t sectors = 0;                           // секторів прочитано успішно
  uint32_t blocks = 0;                            // блоків 64 Б прочитано
  uint32_t blkMinUs = 0xFFFFFFFFu, blkMaxUs = 0, blkSumUs = 0;
  uint32_t cmdMaxUs = 0, cmdSumUs = 0;            // час команди: від DISK_READ до статусу 0x14
  uint32_t waitUs = 0;                            // сума очікування події по блоках
  uint32_t statUs = 0;                            // сума GET_STATUS (обмін + пауза)
  uint32_t xferUs = 0;                            // сума RD_USB_DATA0 (дані блоку)
  uint32_t polls = 0;                             // GET_STATUS без події (повтори)
  uint32_t intWaits = 0;                          // очікувань, завершених за INT#
  uint32_t intMisses = 0;                         // INT не з'явився за зріз: запасний GET_STATUS
  uint32_t errors = 0;                            // команди, що завершились помилкою
};

class Ch376Disk {
 public:
  explicit Ch376Disk(Ch376Link& link) : link_(link) {}

  // Лічильники, кеш ємності й налаштування — до типових значень.
  void reset();

  void setUseInt(bool on) { useInt_ = on; }
  bool useInt() const { return useInt_; }

  // Секторів в одній DISK_READ (1..kCh376DiskMaxSectors). Поза межами — ігнорується.
  void setSecPerCmd(uint32_t n);
  uint32_t secPerCmd() const { return secPerCmd_; }

  // Пауза після GET_STATUS, байтових інтервалів (вимір варіанта в).
  void setSettleBytes(uint32_t n) { settle_ = n; }
  uint32_t settleBytes() const { return settle_; }

  // Експеримент (варіант г): не надсилати GET_STATUS між блоками однієї команди. Ризик — у звіті.
  void setSkipStatus(bool on) { skipSt_ = on; }
  bool skipStatus() const { return skipSt_; }

  // DISK_CAPACITY: кількість секторів (запам'ятовується). Ok → sectors дійсний.
  DiskErr capacity(uint32_t& sectors);
  bool capacityKnown() const { return capKnown_; }
  uint32_t cachedCapacity() const { return cap_; }

  // Читання n секторів з lba. dst == nullptr — дані відкидаються (вимірювання); інакше dst має вмістити n·512 Б.
  // Розбиває на команди по secPerCmd(). Якщо ємність відома і lba+n за її межами — BadArg без звернення до чіпа.
  DiskErr readSectors(uint32_t lba, uint32_t n, uint8_t* dst);

  uint8_t lastStatus() const { return lastSt_; }
  const DiskStats& stats() const { return st_; }
  void resetStats() { st_ = DiskStats{}; }

  // Статус чіпа → помилка диска. 0x14 → Ok.
  static DiskErr mapStatus(uint8_t st);

 private:
  DiskErr send(uint8_t cmd, const uint8_t* a, size_t na);
  DiskErr getStatus(uint8_t& st);
  DiskErr waitStatus(uint8_t& st, uint32_t tmoMs);
  DiskErr readBlock(uint8_t* dst);  // RD_USB_DATA0: довжина має бути 64; dst == nullptr — відкинути
  DiskErr oneCmd(uint32_t lba, uint32_t n, uint8_t* dst);
  DiskErr failCmd(DiskErr e);

  Ch376Link& link_;
  bool useInt_ = kCh376FsUseInt;
  uint32_t secPerCmd_ = kCh376DiskSecPerCmdDef;
  uint32_t settle_ = kCh376DiskSettleDef;
  bool skipSt_ = false;
  bool capKnown_ = false;
  uint32_t cap_ = 0;
  uint8_t lastSt_ = 0;
  uint32_t tWait_ = 0, tStat_ = 0;  // розбивка останнього waitStatus()
  DiskStats st_;
};

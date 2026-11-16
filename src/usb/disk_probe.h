#pragma once
#include <stddef.h>
#include <stdint.h>

#include "config.h"

// Діагностичний розбір розкладки тому (M4b2): MBR, захисний MBR GPT, BPB FAT12/16/32, exFAT. Чистий C++ без Arduino.h.
// Це НЕ читач FAT: лише структури й придатність до майбутнього читача. Сектори — через SectorReader
// (на залізі адаптер до Ch376Disk у ch376_diag_disk.cpp; у тестах — образ у памʼяті).

class SectorReader {
 public:
  virtual ~SectorReader() {}
  // true — сектор lba прочитано у dst (512 Б).
  virtual bool readSector(uint32_t lba, uint8_t* dst) = 0;
};

enum class VolKind : uint8_t { None, Fat12, Fat16, Fat32, ExFat, Unknown };
const char* volKindName(VolKind k);
// Тип FAT за кількістю кластерів (правило Microsoft: <4085 — FAT12, <65525 — FAT16, інакше FAT32).
VolKind fatKindFromClusters(uint32_t clusters);

struct PartEntry {
  uint8_t type = 0;
  uint32_t startLba = 0;
  uint32_t sectors = 0;
};

struct BpbInfo {
  bool valid = false;
  uint16_t bytesPerSec = 0;
  uint16_t reserved = 0;     // повне 16-бітне поле (чіп бачив лише молодший байт)
  uint8_t secPerClus = 0;
  uint8_t numFats = 0;
  uint16_t rootEntries = 0;
  uint32_t totSec = 0;
  uint32_t fatSz = 0;
  uint32_t rootClus = 0;     // лише FAT32
  uint32_t dataStart = 0;    // сектор початку даних відносно початку тому
  uint32_t clusters = 0;
  char label[12] = {0};      // ASCII, без хвостових пробілів; непечатні → '?'
};

struct ExFatInfo {
  bool valid = false;
  uint64_t partOffset = 0;
  uint64_t volLength = 0;
  uint32_t fatOffset = 0, fatLength = 0, heapOffset = 0, clusterCount = 0, rootCluster = 0;
  uint8_t bytesPerSecShift = 0, secPerClusShift = 0, numFats = 0;
};

struct ProbeReport {
  bool mbr = false;             // сигнатура 55AA і таблиця розділів розібрані
  bool protectiveGpt = false;   // у таблиці MBR є тип 0xEE
  bool gptHeader = false;       // «EFI PART» на LBA 1
  bool gptFirstFound = false;   // у GPT знайдено перший непорожній запис (він і розібраний)
  uint8_t partCount = 0;        // ненульових записів MBR (0..4)
  PartEntry parts[4];
  uint32_t volStart = 0;        // LBA тому, який розібрано (0 — суцільна флешка без таблиці)
  VolKind kind = VolKind::None;
  BpbInfo bpb;
  ExFatInfo ex;
  bool readable = false;        // придатно для майбутнього читача: FAT12/16/32 з 512 Б/сектор
};

// Розбір розкладки. scratch — буфер 512 Б від викликача (не на стеку). false — сектора 0 не прочитано.
bool probeDisk(SectorReader& rd, uint8_t* scratch, ProbeReport& out);

// Чисті розбори (для тестів і повторного використання). Усі читають лише з переданого буфера.
bool parseMbr(const uint8_t* s, PartEntry* out, uint8_t& count);               // false — немає 55AA
bool parseBpb(const uint8_t* s, BpbInfo& b);                                    // false — розкладка неможлива
bool parseExFat(const uint8_t* s, ExFatInfo& e);                                // false — немає "EXFAT   " або збій полів
bool parseGptHeader(const uint8_t* s, uint32_t& entriesLba, uint32_t& count, uint32_t& entrySize);  // false — немає "EFI PART"
bool parseGptFirst(const uint8_t* entries, uint32_t entrySize, uint32_t& startLba, uint32_t& sectors);  // false — порожньо

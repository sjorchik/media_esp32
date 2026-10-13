#pragma once
#include <stddef.h>
#include <stdint.h>

#include "config.h"
#include "media/ifile_reader.h"

// Запис каталогу (повертається nextEntry)
struct MediaEntry {
  char name[kMediaNameMax];  // UTF-8; порожнє, якщо nameOk=false
  bool nameOk;               // false: імʼя не вдалося прочитати або воно задовге для буфера
  bool isDir;
  bool hidden;               // атрибут hidden
};

enum class MediaEvent : uint8_t { None, Inserted, Removed };

struct MediaInfo {
  bool present, mounted;
  int cdPin, cdRaw;          // cdPin<0: присутність визначається опитуванням
  uint8_t mhz;
  const char* cardType;
  const char* fs;
  uint32_t sizeMb;
  uint8_t errStreak;
  uint32_t errTotal;
};

// Носій (SD, пізніше USB). Усі методи — лише з задачі app. Решта коду не знає про тип носія.
class IMedia {
 public:
  virtual ~IMedia() {}
  virtual bool begin() = 0;                       // одноразово: шина, піни
  virtual MediaEvent poll(uint32_t nowMs) = 0;    // hot-plug: антидребезг, авто-mount/unmount; викликати часто
  virtual bool mount() = 0;
  virtual void unmount() = 0;
  virtual bool mounted() const = 0;
  virtual bool present() const = 0;               // присутність за останнім poll()
  // Перелік каталогу: handle ≥ 0 або -1. nextEntry: 1 = запис, 0 = кінець, -1 = помилка.
  virtual int openDir(const char* path) = 0;      // path: "/" або "/A/B", UTF-8
  virtual int nextEntry(int h, MediaEntry& e) = 0;
  virtual void closeDir(int h) = 0;
  virtual IFileReader& reader() = 0;
  virtual void info(MediaInfo& out) = 0;
};

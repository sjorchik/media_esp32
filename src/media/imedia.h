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
  // M4b4. Ідентифікатор тому (серійний номер ФС) для резюме USB; 0 = немає/не використовується (SD).
  virtual uint32_t volumeId() const { return 0; }
  // M4b4. Короткий ексклюзивний доступ до чіпа (запис NVS зупиняє кеш флешу — у цей час передача по UART недопустима).
  // Типово no-op (SD). Викликається з app; може блокувати не довше kUsbLockWaitMs.
  // M4b4. Профіль скану (DBG,usb,vscan): what 0 = почати облік, 1 = зупинити, 2 = вивести в лог (wallMs/busyMs — час скану зі статистики Scanner). SD: no-op.
  virtual void scanProfile(int what, uint32_t wallMs = 0, uint32_t busyMs = 0) { (void)what; (void)wallMs; (void)busyMs; }
  virtual void ioLock() {}
  virtual void ioUnlock() {}
#if ENABLE_USB_CH376
  // M4c1. Тимчасові ресурси скану (USB: кеш секторів на heap, пропуск getName() для не-MP3). scanBegin() — перед Scanner::start,
  // scanEnd() — після завершення АБО переривання скану (ідемпотентно); у відтворенні ресурсів скану немає. SD: no-op.
  virtual void scanBegin() {}
  virtual void scanEnd() {}
  // M4c2. Неблокуюче монтування (USB: відкриття по кроках у loop). mountBegin() стартує (false — відмова одразу, див. lastFail у USB),
  // mountStep() — один короткий крок: Pending / Done (mounted()==true) / Failed; mountAbort() — перервати й звільнити (ідемпотентно).
  // Типово (SD): mount() блокуючий, тож Done/Failed одразу за mounted().
  enum class MountPoll : uint8_t { Pending, Done, Failed };
  virtual bool mountBegin() { return true; }
  virtual MountPoll mountStep(uint32_t /*nowMs*/) { return mounted() ? MountPoll::Done : MountPoll::Failed; }
  virtual void mountAbort() {}
  virtual bool mounting() const { return false; }
#endif
};




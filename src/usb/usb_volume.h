#pragma once
#include "config.h"

#if ENABLE_USB_CH376

#include <stddef.h>
#include <stdint.h>

#include "media/ifile_reader.h"
#include "usb/usb_volume_core.h"

// USB-том на SdFat над сирими секторами CH376S (M4b3). Увесь стан SdFat (FsVolume, FsFile) — на heap від open() до close();
// статично лише вказівники й UsbOpenInfo (≈0,15 КБ). Один відкритий файл одночасно. Мʼютекса немає: усі виклики з однієї
// задачі (консоль); для плеєра в наступних завданнях потрібне серіалізування, як у SdMedia.
class FsVolume;
class FsFile;
class Ch376SdDevice;
class UsbVolume;

struct UsbVolInfo {
  UsbOpenFail fail = UsbOpenFail::None;
  uint32_t baud = 0;
  SelfTest selfTest = SelfTest::NotRun;
  uint32_t partStart = 0, partSectors = 0;
  uint8_t partIndex = 0;
  uint8_t fsType = 0;          // 16, 32, 64 (exFAT) — з SdFat
  uint32_t clusterBytes = 0;
  char label[12] = {0};
  uint32_t openMs = 0;
};

struct UsbDirEntry {
  const char* name;  // UTF-8; "" якщо імʼя не вдалося прочитати
  bool nameOk, isDir, hidden;
  uint32_t size;
};
typedef void (*UsbLsFn)(const UsbDirEntry& e, void* ctx);
enum class UsbLsRes : uint8_t { Ok, NoVolume, OpenFail, NotDir, Gone, NoMemory };

class UsbFileReader final : public IFileReader {
 public:
  explicit UsbFileReader(UsbVolume& v) : v_(v) {}
  bool open(const char* path) override;
  size_t read(void* dst, size_t n) override;
  bool seek(uint32_t pos) override;
  uint32_t size() const override { return size_; }
  uint32_t pos() const override { return pos_; }
  bool isOpen() const override;
  bool failed() const override { return failed_; }
  void close() override;

 private:
  UsbVolume& v_;
  uint32_t pos_ = 0, size_ = 0;
  bool failed_ = false;
};

class UsbVolume {
 public:
  static UsbVolume& get();

  // Запуск драйвера (2 Мбод з самоперевіркою, інакше 1 Мбод), розбір розділу, монтування. false — причина в out.fail
  // (і окремим логом). Після відмови все звільнено, порт закрито.
  bool open(UsbVolInfo& out);
  // Закрити файл, розмонтувати, звільнити обʼєкти, скинути чіп на 9600 і Serial2.end(). Ідемпотентно.
  void close();

  bool isOpen() const { return vol_ != nullptr; }
  bool present() const;  // том відкрито і пристрій не «відсутній»
  const UsbOpenInfo* openInfo() const { return vol_ ? &ses_.info() : nullptr; }
  Ch376BlockDev* blockDev() { return ses_.dev(); }
  Ch376SdDevice* sdDevice() { return dev_; }

  // Перелік каталогу (до maxN записів, без «.» і «..»). count — передано в fn; more — є ще записи.
  UsbLsRes listDir(const char* path, uint32_t maxN, UsbLsFn fn, void* ctx, uint32_t& count, bool& more);

  // Один файл: відкрити (лише звичайний файл), читати, seek, закрити. fileRead: <0 — помилка, 0 — кінець.
  bool fileOpen(const char* path, uint32_t& size);
  int fileRead(void* dst, size_t n);
  bool fileSeek(uint32_t pos);
  bool fileIsOpen() const;
  void fileClose();

  UsbFileReader& reader() { return rd_; }

 private:
  UsbVolume() : rd_(*this) {}
  void release();

  UsbSession ses_;
  Ch376SdDevice* dev_ = nullptr;
  FsVolume* vol_ = nullptr;
  FsFile* file_ = nullptr;
  UsbFileReader rd_;
};

// Статична памʼять (поза FsVolume/FsFile на heap): має лишатись у межах завдання (≤ 0,3 КБ).
static_assert(sizeof(UsbOpenInfo) <= 160, "UsbOpenInfo завеликий для статичної памʼяті");

#endif  // ENABLE_USB_CH376

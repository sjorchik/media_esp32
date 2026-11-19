#pragma once
#include "config.h"

#if ENABLE_USB_CH376

#include <stddef.h>
#include <stdint.h>

#include "media/ifile_reader.h"
#include "usb/usb_volume_core.h"

// USB-том на SdFat над сирими секторами CH376S (M4b3). Увесь стан SdFat (FsVolume, FsFile) — на heap від open() до close();
// статично лише вказівники й UsbOpenInfo (≈0,15 КБ). Один відкритий файл одночасно.
// M4b4: усі публічні виклики серіалізуються одним рекурсивним мʼютексом (як у SdMedia): app (відкриття, скан, ID3 у DBG),
// задача mp3 (читання файлу під час відтворення), консоль (DBG,usb,*). Очікування — до kUsbLockWaitMs.
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
  uint32_t serial = 0;         // M4b4: серійний номер тому (BPB/exFAT); 0 = невідомий
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

// M4b4: профіль перелічення каталогів під час скану (DBG,usb,vscan). Облік увімкнено лише між profBegin() і profEnd().
struct UsbScanProf {
  bool on = false;
  uint32_t openDirN = 0, openDirUs = 0, openDirMaxUs = 0;
  uint32_t nextN = 0, nextUs = 0, nextMaxUs = 0;   // виклики dirNext = FsFile::openNext
  uint32_t entries = 0, endN = 0, errN = 0;        // dirNext: 1 = запис, 0 = кінець каталогу, -1 = помилка
  uint32_t closeN = 0, closeUs = 0;
  uint32_t lockN = 0, lockWaitUs = 0, lockWaitMaxUs = 0;  // очікування мʼютекса (лише всередині openDir/openNext/closeDir і читань під час скану)
  uint32_t calls = 0, sectors = 0, cmds = 0;       // блочний пристрій: різниця за час скану
  // M4c1: звернення до секторів і кеш (знімок із SectorCache у profEnd/scanEnd)
  bool cacheOn = false;
  uint8_t cSlots = 0;
  uint32_t cReqs = 0, cUnique = 0, cWould = 0, cSeq = 0, cSame = 0, cJump = 0;
  uint32_t cHits = 0, cMiss = 0, cEvict = 0, cRa = 0;
  uint32_t nameSkips = 0;                          // файли, для яких getName() пропущено (суфікс 8.3 не MP3)
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
  bool present() const;  // том відкрито і пристрій не «відсутній» (бере мʼютекс; не вдалось — false)
  // M4b4: те саме без очікування — для poll() у app: якщо чіп зайнятий читанням (мʼютекс чужий), вважаємо «присутній».
  bool presentNoWait() const;
  uint32_t serial() const { return serial_; }  // серійний номер відкритого тому (0 — немає/невідомий)
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

  // M4b4: перелік каталогу по одному запису (для IMedia::openDir/nextEntry). Хендли — FsFile на heap від dirOpen до dirClose,
  // до kUsbDirHandles одночасно. dirNext: 1 = запис, 0 = кінець, -1 = помилка/носій зник. name — буфер kMediaNameMax.
  int dirOpen(const char* path);
  int dirNext(int h, char* name, size_t cap, bool& nameOk, bool& isDir, bool& hidden);
  void dirClose(int h);

  void profBegin();
  void profEnd();
  const UsbScanProf& prof() const { return prof_; }

  // M4c1: тимчасові ресурси скану. scanBegin(): кеш секторів на heap (kUsbScanCacheSlots, 0 = лише лічильники) і фільтр getName();
  // scanEnd(): знімок лічильників у профіль, звільнення кеша, зняття фільтра (ідемпотентно; і при перериванні скану).
  void scanBegin();
  void scanEnd();
  void scanTune();  // M4c1: під час скану розширює кеш секторів до kUsbScanCacheSlots, коли heap дозволяє (після BT→USB)
  // Чи тримає щось (том, файл, каталоги, кеш, порт чіпа). Для перевірки ексклюзивності після deactivate (бере мʼютекс).
  bool held() const;

  // M4b4: ексклюзивний доступ до чіпа ззовні (запис NVS зупиняє кеш флешу: UART-обмін у цей час недопустимий).
  bool lockFor(uint32_t ms);
  void unlock();

  UsbFileReader& reader() { return rd_; }

 private:
  UsbVolume();
  void release();
  void closeDirs();

  UsbSession ses_;
  Ch376SdDevice* dev_ = nullptr;
  FsVolume* vol_ = nullptr;
  FsFile* file_ = nullptr;
  FsFile* dirs_[kUsbDirHandles] = {};
  uint32_t serial_ = 0;
  UsbScanProf prof_;
  uint32_t base0_[3] = {0, 0, 0};  // BlockDevStats (calls, sectors, cmds) на початок скану
  bool mp3Only_ = false;           // M4c1: dirNext не читає довге імʼя файлів із суфіксом 8.3 ≠ MP3 (лише під час скану)
  UsbFileReader rd_;
};

// Статична памʼять (поза FsVolume/FsFile на heap): має лишатись у межах завдання (≤ 0,3 КБ).
static_assert(sizeof(UsbOpenInfo) <= 160, "UsbOpenInfo завеликий для статичної памʼяті");

#endif  // ENABLE_USB_CH376



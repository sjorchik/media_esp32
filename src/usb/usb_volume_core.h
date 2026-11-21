#pragma once
#include <stddef.h>
#include <stdint.h>

#include "config.h"
#include "usb/ch376.h"
#include "usb/ch376_blockdev.h"
#include "usb/ch376_disk.h"
#include "usb/ch376_link.h"
#include "usb/disk_probe.h"

// Логіка відкриття USB-тому без SdFat і без Arduino (M4b3): політика швидкості, самоперевірка 2 Мбод, вибір розділу,
// причини відмови. Чіп і порт — через IUsbOpenHw (на залізі його реалізує usb_volume.cpp, у тестах — підміна).
// Чистий C++, тестується в native. Усі виклики — з однієї задачі.

// Причина відмови open() — окремим логом.
enum class UsbOpenFail : uint8_t {
  None,
  AlreadyOpen,    // том уже відкрито
  NoMemory,       // не вистачило heap
  NoPort,         // порт/драйвер не запустився
  NoChip,         // чіп не відповів на жодній швидкості
  NoMedia,        // чіп є, флешки немає (тайм-аут підключення, відʼєднання, немає диска)
  SlowLink,       // чіп відповідає лише нижче kCh376MinPlayBaud
  ReadError,      // ємність або службові сектори не прочитано
  NoPartition,    // розділу/ФС не знайдено (порожній диск, таблиця без записів)
  UnsupportedFs,  // розділ є, але ФС не FAT16/FAT32/exFAT (або сектор ≠ 512 Б)
  MountFailed,    // SdFat не змонтував том (виставляє верхній шар)
};
const char* usbOpenFailName(UsbOpenFail f);  // коротка ASCII-назва
const char* usbOpenFailText(UsbOpenFail f);  // пояснення українською для логу

// Чіп і порт для open(). Реалізація на залізі — Ch376/ch376_uart/Ch376Disk; у тестах — підміна.
class IUsbOpenHw {
 public:
  virtual ~IUsbOpenHw() {}
  // Сеанс створив Ch376Disk (heap) для цього open(); на залізі реалізація через нього читає сектори й ємність.
  virtual void attach(Ch376Disk& /*disk*/) {}
  virtual bool portBegin() = 0;
  // Повна послідовність на швидкості baud: RESET_ALL → CHECK_EXIST → швидкість → host → підключення → DISK_CONNECT →
  // DISK_MOUNT (BadFs ігнорується: чіп не монтує GPT 30 ГБ, а сирі сектори читаються). baud == 0 — список
  // 1000000 → 460800 → 115200 (лише діагностика «чіп живий, але повільно»). Кожен виклик починає з RESET_ALL.
  virtual Ch376Err bringUp(uint32_t baud) = 0;
  virtual uint32_t baud() const = 0;  // фактична швидкість після останнього успішного bringUp
  virtual DiskErr capacity(uint32_t& sectors) = 0;
  virtual DiskErr readSector(uint32_t lba, uint8_t* dst) = 0;  // сирий сектор диска (512 Б)
  virtual void portEnd() = 0;

  // M4c2: неблокуючий варіант bringUp для кроків у loop. Типово — один блокуючий bringUp() у першому ж bringStep().
  // bringStep повертає true, коли послідовність завершено (res — результат як у bringUp), інакше false (викликати ще).
  virtual void bringBegin(uint32_t baud) { asyncBaud_ = baud; }
  virtual bool bringStep(uint32_t /*nowMs*/, Ch376Err& res) {
    res = bringUp(asyncBaud_);
    return true;
  }
  virtual void bringAbort() {}

 protected:
  uint32_t asyncBaud_ = 0;
};

// Результат самоперевірки 2 Мбод.
enum class SelfTest : uint8_t {
  NotRun,      // не виконувалась (відмова до неї)
  Passed,      // хеш сектора 0 на 2 Мбод збігся з 1 Мбод — лишаємось на 2 Мбод
  Mismatch,    // не збігся (дроти) — повернулись на 1 Мбод
  FastFailed,  // 2 Мбод не узгодились або не читають — повернулись на 1 Мбод
  RefFailed,   // еталон на 1 Мбод недоступний, 2 Мбод працюють — без перевірки
};
const char* selfTestName(SelfTest s);

struct UsbStep {
  uint32_t baud;  // 0 — auto-список
  Ch376Err err;
};

inline constexpr uint8_t kUsbStepsMax = 6;

struct UsbOpenInfo {
  uint32_t baud = 0;  // фінальна швидкість
  SelfTest selfTest = SelfTest::NotRun;
  uint32_t refHash = 0, fastHash = 0;  // FNV-1a сектора 0 на 1 і 2 Мбод
  uint32_t capacity = 0;               // секторів на диску
  uint32_t partStart = 0;              // LBA початку тому
  uint32_t partSectors = 0;            // довжина тому, секторів
  uint8_t partIndex = 0;               // 1..4 — запис MBR; 0 — без таблиці або перший запис GPT
  char label[12] = {0};                // мітка тому з BPB (FAT); для exFAT порожня
  VolKind kind = VolKind::None;
  uint32_t serial = 0;                 // M4b4: серійний номер тому (BPB/exFAT) — привʼязка резюме до флешки; 0 = невідомий
  DiskErr diskErr = DiskErr::Ok;       // остання помилка диска
  Ch376Err chipErr = Ch376Err::Ok;     // остання помилка чіпа
  UsbStep steps[kUsbStepsMax];
  uint8_t nSteps = 0;
};

uint32_t usbFnv1a(const uint8_t* d, size_t n);

// M4c2. Результат кроку неблокуючого відкриття.
enum class OpenPoll : uint8_t { Pending, Done, Failed };

// Автомат usbOpenCore по кроках (те саме дерево рішень: еталон 1 Мбод → 2 Мбод із хешем → відкат → ємність → розділ), але
// кожен step() — одна коротка дія; очікування чіпа йдуть через hw.bringBegin/bringStep. Чистий C++, тестується в native
// (диференціально проти usbOpenCore). scratch (512 Б) і pr — від викликача, мають жити до кінця.
class UsbOpenSteps {
 public:
  void begin(IUsbOpenHw& hw, uint8_t* scratch, ProbeReport* pr, UsbOpenInfo& info);
  OpenPoll step(uint32_t nowMs, UsbOpenFail& fail);
  void abort();  // hw.bringAbort(); порт закриває викликач
  bool active() const { return ph_ != Ph::Idle && ph_ != Ph::End; }

 private:
  enum class Ph : uint8_t { Idle, Port, Ref, RefRead, Fast, FastRead, Back, AltFast, AltRead, Auto, Cap, Pick, End };
  OpenPoll bad(UsbOpenFail f, UsbOpenFail& out);
  bool bring(uint32_t baud, uint32_t now, Ch376Err& e);  // стартує при першому виклику фази; true — bringUp завершено

  IUsbOpenHw* hw_ = nullptr;
  uint8_t* scratch_ = nullptr;
  ProbeReport* pr_ = nullptr;
  UsbOpenInfo* info_ = nullptr;
  Ph ph_ = Ph::Idle;
  bool started_ = false;  // bringBegin для поточної фази виконано
  Ch376Err refErr_ = Ch376Err::Ok;
};

// Швидкість: еталон 1 Мбод (хеш сектора 0) → 2 Мбод (хеш) → збіг: лишаємось; ні: повернення на 1 Мбод. Кожна швидкість
// нижча за kCh376MinPlayBaud — відмова SlowLink. scratch — 512 Б від викликача. Повертає None і фінальну швидкість у info.baud.
UsbOpenFail usbNegotiate(IUsbOpenHw& hw, uint8_t* scratch, UsbOpenInfo& info);

// Вибір розділу. MBR: перший запис, що дає придатну ФС (перебір усіх 4, розширені 0x05/0x0F пропускаються).
// GPT: лише перший непорожній запис (так повертає probeDisk). Без таблиці: сектор 0. pr — від викликача (не на стеку).
// Заповнює info.partStart/partSectors/partIndex/kind. Розмір: запис MBR → розмір ФС (BPB/exFAT) → решта диска; обрізається до ємності.
UsbOpenFail usbPickPartition(IUsbOpenHw& hw, uint8_t* scratch, ProbeReport& pr, UsbOpenInfo& info);

// Увесь open без SdFat: portBegin → usbNegotiate → capacity → usbPickPartition. При відмові порт НЕ закривається
// (закриває викликач через hw.portEnd()). scratch — 512 Б, pr — від викликача.
UsbOpenFail usbOpenCore(IUsbOpenHw& hw, uint8_t* scratch, ProbeReport& pr, UsbOpenInfo& info);

// Сеанс USB без SdFat (M4b3): обʼєкти на heap від open() до close(). SdFat-шар (UsbVolume) надбудовується зверху.
// open(): scratch 512 Б і ProbeReport — тимчасово (звільняються до повернення), Ch376Disk і Ch376BlockDev — до close().
// Відмова: усе звільняється, hw.portEnd() викликано, info() лишається для логу. Усі виклики — з однієї задачі.
class UsbSession {
 public:
  UsbSession() = default;
  UsbSession(const UsbSession&) = delete;
  UsbSession& operator=(const UsbSession&) = delete;

  UsbOpenFail open(Ch376Link& link, IUsbOpenHw& hw);
  // M4c2: неблокуюче відкриття. openBegin виділяє обʼєкти (як open); openStep — один короткий крок; openAbort звільняє все
  // (hw.bringAbort + hw.portEnd). Після Done/Failed обʼєкти вже у стані як після open().
  UsbOpenFail openBegin(Ch376Link& link, IUsbOpenHw& hw);  // None — автомат запущено
  OpenPoll openStep(uint32_t nowMs, UsbOpenFail& fail);
  void openAbort();
  bool opening() const { return steps_.active() || pending_; }
  void close();  // звільняє обʼєкти; порт чіпа закриває викликач; ідемпотентно

  bool isOpen() const { return bd_ != nullptr; }
  Ch376BlockDev* dev() const { return bd_; }
  Ch376Disk* disk() const { return disk_; }
  const UsbOpenInfo& info() const { return info_; }

 private:
  void freeTemp();

  Ch376Disk* disk_ = nullptr;
  Ch376BlockDev* bd_ = nullptr;
  UsbOpenInfo info_;
  uint8_t* scratch_ = nullptr;  // M4c2: тимчасові буфери відкриття по кроках
  ProbeReport* pr_ = nullptr;
  IUsbOpenHw* hw_ = nullptr;
  UsbOpenSteps steps_;
  bool pending_ = false;
};



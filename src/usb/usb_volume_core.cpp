#include "usb/usb_volume_core.h"

#include "config.h"

#if ENABLE_USB_CH376

#include <string.h>

#include <new>

namespace {

void rec(UsbOpenInfo& i, uint32_t baud, Ch376Err e) {
  if (i.nSteps < kUsbStepsMax) i.steps[i.nSteps++] = UsbStep{baud, e};
  i.chipErr = e;
}

// Помилка чіпа без флешки: інші швидкості не допоможуть, тож їх не пробуємо (кожна спроба чекає підключення до 3 с).
bool noMediaErr(Ch376Err e) { return e == Ch376Err::Timeout || e == Ch376Err::Disconnected || e == Ch376Err::NoDisk; }

UsbOpenFail fromDisk(DiskErr d) {
  return (d == DiskErr::Disconnected || d == DiskErr::NoDisk) ? UsbOpenFail::NoMedia : UsbOpenFail::ReadError;
}

// Адаптер SectorReader → IUsbOpenHw; запамʼятовує помилку (probeDisk сам її не віддає).
class HwReader : public SectorReader {
 public:
  explicit HwReader(IUsbOpenHw& h) : h_(h) {}
  bool readSector(uint32_t lba, uint8_t* dst) override {
    const DiskErr e = h_.readSector(lba, dst);
    if (e != DiskErr::Ok) err_ = e;
    return e == DiskErr::Ok;
  }
  DiskErr err() const { return err_; }

 private:
  IUsbOpenHw& h_;
  DiskErr err_ = DiskErr::Ok;
};

// Придатність до SdFat: FAT16/FAT32 з 512 Б/сектор (FAT12 SdFat не підтримує) або exFAT з 512 Б/сектор.
bool usable(const ProbeReport& r) {
  if (r.kind == VolKind::Fat16 || r.kind == VolKind::Fat32) return r.bpb.bytesPerSec == 512;
  if (r.kind == VolKind::ExFat) return r.ex.valid && r.ex.bytesPerSecShift == 9;
  return false;
}

// Розбір завантажувального сектора на lba у pr (як classifyVolume у disk_probe; той — у анонімному просторі імен).
bool classifyAt(SectorReader& rd, uint8_t* s, uint32_t lba, ProbeReport& pr) {
  pr.volStart = lba;
  pr.bpb = BpbInfo{};
  pr.ex = ExFatInfo{};
  pr.readable = false;
  if (!rd.readSector(lba, s)) {
    pr.kind = VolKind::Unknown;
    return false;
  }
  if (parseExFat(s, pr.ex)) {
    pr.kind = VolKind::ExFat;
    return true;
  }
  if (parseBpb(s, pr.bpb)) {
    pr.kind = fatKindFromClusters(pr.bpb.clusters);
    pr.readable = pr.bpb.bytesPerSec == 512;
    return true;
  }
  pr.kind = VolKind::Unknown;
  return true;
}

}  // namespace

const char* usbOpenFailName(UsbOpenFail f) {
  switch (f) {
    case UsbOpenFail::None: return "ok";
    case UsbOpenFail::AlreadyOpen: return "already";
    case UsbOpenFail::NoMemory: return "nomem";
    case UsbOpenFail::NoPort: return "noport";
    case UsbOpenFail::NoChip: return "nochip";
    case UsbOpenFail::NoMedia: return "nomedia";
    case UsbOpenFail::SlowLink: return "slowlink";
    case UsbOpenFail::ReadError: return "readerr";
    case UsbOpenFail::NoPartition: return "nopart";
    case UsbOpenFail::UnsupportedFs: return "unsupfs";
    case UsbOpenFail::MountFailed: return "mountfail";
  }
  return "?";
}

const char* usbOpenFailText(UsbOpenFail f) {
  switch (f) {
    case UsbOpenFail::None: return "успіх";
    case UsbOpenFail::AlreadyOpen: return "том уже відкрито (спершу DBG,usb,umount)";
    case UsbOpenFail::NoMemory: return "не вистачило памʼяті (heap)";
    case UsbOpenFail::NoPort: return "порт CH376S не запустився";
    case UsbOpenFail::NoChip: return "чіп CH376S не відповідає на жодній швидкості (проводка/живлення)";
    case UsbOpenFail::NoMedia: return "флешки немає або вона не підключилась";
    case UsbOpenFail::SlowLink: return "чіп відповідає лише нижче мінімальної швидкості для відтворення";
    case UsbOpenFail::ReadError: return "сектори диска не читаються (ємність/службові сектори)";
    case UsbOpenFail::NoPartition: return "розділу або файлової системи не знайдено";
    case UsbOpenFail::UnsupportedFs: return "непідтримувана ФС (потрібні FAT16, FAT32 або exFAT з 512 Б/сектор)";
    case UsbOpenFail::MountFailed: return "SdFat не змонтував том (не знайдено ФС або збій читання)";
  }
  return "?";
}

const char* selfTestName(SelfTest s) {
  switch (s) {
    case SelfTest::NotRun: return "не виконувалась";
    case SelfTest::Passed: return "збіг, лишаємось на 2 Мбод";
    case SelfTest::Mismatch: return "хеш не збігся (дроти?), повернення на 1 Мбод";
    case SelfTest::FastFailed: return "2 Мбод не працюють, повернення на 1 Мбод";
    case SelfTest::RefFailed: return "еталон 1 Мбод недоступний, 2 Мбод без перевірки";
  }
  return "?";
}

uint32_t usbFnv1a(const uint8_t* d, size_t n) {
  uint32_t h = 2166136261u;
  for (size_t i = 0; i < n; i++) {
    h ^= d[i];
    h *= 16777619u;
  }
  return h;
}

UsbOpenFail usbNegotiate(IUsbOpenHw& hw, uint8_t* scratch, UsbOpenInfo& info) {
  info.nSteps = 0;
  info.selfTest = SelfTest::NotRun;

  // 1. Еталон: 1 Мбод.
  Ch376Err e = hw.bringUp(kUsbBaudRef);
  rec(info, kUsbBaudRef, e);
  if (e == Ch376Err::Ok && hw.baud() >= kCh376MinPlayBaud) {
    DiskErr d = hw.readSector(0, scratch);
    if (d != DiskErr::Ok) {
      info.diskErr = d;
      return fromDisk(d);
    }
    info.refHash = usbFnv1a(scratch, kCh376DiskSecBytes);

    // 2. Швидка: 2 Мбод і порівняння хешу сектора 0.
    e = hw.bringUp(kUsbBaudFast);
    rec(info, kUsbBaudFast, e);
    info.selfTest = SelfTest::FastFailed;
    if (e == Ch376Err::Ok && hw.baud() >= kCh376MinPlayBaud) {
      d = hw.readSector(0, scratch);
      if (d == DiskErr::Ok) {
        info.fastHash = usbFnv1a(scratch, kCh376DiskSecBytes);
        if (info.fastHash == info.refHash) {
          info.selfTest = SelfTest::Passed;
          info.baud = hw.baud();
          return UsbOpenFail::None;
        }
        info.selfTest = SelfTest::Mismatch;
      } else {
        info.diskErr = d;
      }
    }
    // 3. Назад на 1 Мбод (скид чіпа очищає сліди невдалої спроби).
    e = hw.bringUp(kUsbBaudRef);
    rec(info, kUsbBaudRef, e);
    if (e != Ch376Err::Ok) return noMediaErr(e) ? UsbOpenFail::NoMedia : UsbOpenFail::NoChip;
    info.baud = hw.baud();
    return UsbOpenFail::None;
  }

  // Еталон недоступний. Немає флешки — далі нічого пробувати.
  if (e != Ch376Err::Ok) {
    if (noMediaErr(e)) return UsbOpenFail::NoMedia;
  }
  // 4. Лише 2 Мбод (без самоперевірки).
  const Ch376Err e2 = hw.bringUp(kUsbBaudFast);
  rec(info, kUsbBaudFast, e2);
  if (e2 == Ch376Err::Ok && hw.baud() >= kCh376MinPlayBaud) {
    const DiskErr d = hw.readSector(0, scratch);
    if (d != DiskErr::Ok) {
      info.diskErr = d;
      return fromDisk(d);
    }
    info.refHash = info.fastHash = usbFnv1a(scratch, kCh376DiskSecBytes);
    info.selfTest = SelfTest::RefFailed;
    info.baud = hw.baud();
    return UsbOpenFail::None;
  }
  if (noMediaErr(e2)) return UsbOpenFail::NoMedia;
  // 5. Діагностика: чи живий чіп на нижчих швидкостях.
  const Ch376Err e3 = hw.bringUp(0);
  rec(info, 0, e3);
  if (e3 == Ch376Err::Ok) {
    info.baud = hw.baud();
    return UsbOpenFail::SlowLink;  // нижче kCh376MinPlayBaud
  }
  return noMediaErr(e3) ? UsbOpenFail::NoMedia : UsbOpenFail::NoChip;
}

UsbOpenFail usbPickPartition(IUsbOpenHw& hw, uint8_t* scratch, ProbeReport& pr, UsbOpenInfo& info) {
  HwReader rd(hw);
  if (!probeDisk(rd, scratch, pr)) {
    info.diskErr = rd.err();
    return fromDisk(rd.err());
  }
  uint8_t idx = 0;
  if (pr.mbr && !pr.protectiveGpt) {
    // Перший запис, що дає придатну ФС. Запис, який probeDisk уже розібрав, не перечитуємо.
    int first = -1;
    for (int i = 0; i < 4; i++)
      if (pr.parts[i].type != 0 && pr.parts[i].startLba == pr.volStart) {
        first = i;
        break;
      }
    if (usable(pr)) {
      idx = (uint8_t)(first + 1);
    } else {
      for (int i = 0; i < 4; i++) {
        const PartEntry& p = pr.parts[i];
        if (p.type == 0 || p.type == 0x05 || p.type == 0x0F || i == first || p.startLba == 0) continue;
        if (!classifyAt(rd, scratch, p.startLba, pr)) continue;
        if (usable(pr)) {
          idx = (uint8_t)(i + 1);
          break;
        }
      }
    }
  }
  if (!usable(pr)) {
    // Розрізняємо: збій читання, відсутність розділу й чужа ФС.
    if (rd.err() != DiskErr::Ok) {
      info.diskErr = rd.err();
      return fromDisk(rd.err());
    }
    info.kind = pr.kind;
    if (pr.kind == VolKind::None) return UsbOpenFail::NoPartition;
    return UsbOpenFail::UnsupportedFs;
  }

  uint64_t room = info.capacity > pr.volStart ? (uint64_t)info.capacity - pr.volStart : 0;
  uint64_t sec = 0;
  if (idx > 0 && pr.parts[idx - 1].sectors > 0) sec = pr.parts[idx - 1].sectors;
  if (sec == 0) sec = pr.kind == VolKind::ExFat ? pr.ex.volLength : pr.bpb.totSec;
  if (sec == 0 || sec > room) sec = room;
  if (sec == 0) return UsbOpenFail::ReadError;  // початок розділу за межами ємності
  info.partStart = pr.volStart;
  info.partSectors = (uint32_t)sec;
  info.partIndex = idx;
  info.kind = pr.kind;
  memset(info.label, 0, sizeof info.label);
  if (pr.kind != VolKind::ExFat) memcpy(info.label, pr.bpb.label, sizeof info.label - 1);
  return UsbOpenFail::None;
}

UsbOpenFail usbOpenCore(IUsbOpenHw& hw, uint8_t* scratch, ProbeReport& pr, UsbOpenInfo& info) {
  info = UsbOpenInfo{};
  if (!hw.portBegin()) return UsbOpenFail::NoPort;
  UsbOpenFail f = usbNegotiate(hw, scratch, info);
  if (f != UsbOpenFail::None) return f;
  uint32_t cap = 0;
  const DiskErr d = hw.capacity(cap);
  if (d != DiskErr::Ok || cap == 0) {
    info.diskErr = d;
    return d == DiskErr::Ok ? UsbOpenFail::ReadError : fromDisk(d);
  }
  info.capacity = cap;
  return usbPickPartition(hw, scratch, pr, info);
}

UsbOpenFail UsbSession::open(Ch376Link& link, IUsbOpenHw& hw) {
  info_ = UsbOpenInfo{};
  if (bd_) return UsbOpenFail::AlreadyOpen;
  uint8_t* scratch = new (std::nothrow) uint8_t[kCh376DiskSecBytes];
  ProbeReport* pr = new (std::nothrow) ProbeReport;
  Ch376Disk* disk = new (std::nothrow) Ch376Disk(link);
  Ch376BlockDev* bd = disk ? new (std::nothrow) Ch376BlockDev(*disk) : nullptr;
  if (!scratch || !pr || !disk || !bd) {
    delete bd;
    delete disk;
    delete pr;
    delete[] scratch;
    return UsbOpenFail::NoMemory;
  }
  hw.attach(*disk);
  const UsbOpenFail f = usbOpenCore(hw, scratch, *pr, info_);
  delete pr;
  delete[] scratch;
  if (f != UsbOpenFail::None) {
    hw.portEnd();
    delete bd;
    delete disk;
    return f;
  }
  bd->setPartition(info_.partStart, info_.partSectors);
  disk_ = disk;
  bd_ = bd;
  return UsbOpenFail::None;
}

void UsbSession::close() {
  delete bd_;
  delete disk_;
  bd_ = nullptr;
  disk_ = nullptr;
}

#endif  // ENABLE_USB_CH376

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
    case SelfTest::Passed: return "збіг, лишаємось на робочій швидкості";
    case SelfTest::Mismatch: return "хеш не збігся (дроти?), повернення на еталонну швидкість";
    case SelfTest::FastFailed: return "робоча швидкість не працює, повернення на еталонну";
    case SelfTest::RefFailed: return "еталонна швидкість недоступна, робоча без перевірки";
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
#if USB_FORCE_BAUD_REF
    info.baud = hw.baud();  // стенд (M4b4, вимір резерву): лишаємось на еталонній швидкості, самоперевірка не виконується
    return UsbOpenFail::None;
#endif

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
  info.serial = pr.kind == VolKind::ExFat ? pr.ex.serial : pr.bpb.serial;
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

// ============================================================ M4c2: відкриття по кроках

void UsbOpenSteps::begin(IUsbOpenHw& hw, uint8_t* scratch, ProbeReport* pr, UsbOpenInfo& info) {
  hw_ = &hw;
  scratch_ = scratch;
  pr_ = pr;
  info_ = &info;
  info = UsbOpenInfo{};
  ph_ = Ph::Port;
  started_ = false;
  refErr_ = Ch376Err::Ok;
}

void UsbOpenSteps::abort() {
  if (!active()) return;
  hw_->bringAbort();
  ph_ = Ph::Idle;
}

OpenPoll UsbOpenSteps::bad(UsbOpenFail f, UsbOpenFail& out) {
  out = f;
  ph_ = Ph::End;
  return OpenPoll::Failed;
}

bool UsbOpenSteps::bring(uint32_t baud, uint32_t now, Ch376Err& e) {
  if (!started_) {
    hw_->bringBegin(baud);
    started_ = true;
  }
  if (!hw_->bringStep(now, e)) return false;
  started_ = false;
  return true;
}

OpenPoll UsbOpenSteps::step(uint32_t now, UsbOpenFail& fail) {
  UsbOpenInfo& info = *info_;
  Ch376Err e = Ch376Err::Ok;
  DiskErr d = DiskErr::Ok;
  switch (ph_) {
    case Ph::Idle:
    case Ph::End:
      return OpenPoll::Pending;

    case Ph::Port:
      if (!hw_->portBegin()) return bad(UsbOpenFail::NoPort, fail);
      info.nSteps = 0;
      info.selfTest = SelfTest::NotRun;
      ph_ = Ph::Ref;
      return OpenPoll::Pending;

    case Ph::Ref:  // 1. Еталон: 1 Мбод
      if (!bring(kUsbBaudRef, now, e)) return OpenPoll::Pending;
      rec(info, kUsbBaudRef, e);
      refErr_ = e;
      if (e == Ch376Err::Ok && hw_->baud() >= kCh376MinPlayBaud) {
        ph_ = Ph::RefRead;
      } else if (e != Ch376Err::Ok && noMediaErr(e)) {
        return bad(UsbOpenFail::NoMedia, fail);  // немає флешки — далі нічого пробувати
      } else {
        ph_ = Ph::AltFast;  // еталон недоступний
      }
      return OpenPoll::Pending;

    case Ph::RefRead:
      d = hw_->readSector(0, scratch_);
      if (d != DiskErr::Ok) {
        info.diskErr = d;
        return bad(fromDisk(d), fail);
      }
      info.refHash = usbFnv1a(scratch_, kCh376DiskSecBytes);
#if USB_FORCE_BAUD_REF
      info.baud = hw_->baud();  // стенд: лишаємось на еталонній швидкості, самоперевірки немає
      ph_ = Ph::Cap;
#else
      ph_ = Ph::Fast;
#endif
      return OpenPoll::Pending;

    case Ph::Fast:  // 2. Швидка: 2 Мбод і порівняння хешу сектора 0
      if (!bring(kUsbBaudFast, now, e)) return OpenPoll::Pending;
      rec(info, kUsbBaudFast, e);
      info.selfTest = SelfTest::FastFailed;
      ph_ = (e == Ch376Err::Ok && hw_->baud() >= kCh376MinPlayBaud) ? Ph::FastRead : Ph::Back;
      return OpenPoll::Pending;

    case Ph::FastRead:
      d = hw_->readSector(0, scratch_);
      if (d == DiskErr::Ok) {
        info.fastHash = usbFnv1a(scratch_, kCh376DiskSecBytes);
        if (info.fastHash == info.refHash) {
          info.selfTest = SelfTest::Passed;
          info.baud = hw_->baud();
          ph_ = Ph::Cap;
          return OpenPoll::Pending;
        }
        info.selfTest = SelfTest::Mismatch;
      } else {
        info.diskErr = d;
      }
      ph_ = Ph::Back;
      return OpenPoll::Pending;

    case Ph::Back:  // 3. Назад на 1 Мбод
      if (!bring(kUsbBaudRef, now, e)) return OpenPoll::Pending;
      rec(info, kUsbBaudRef, e);
      if (e != Ch376Err::Ok) return bad(noMediaErr(e) ? UsbOpenFail::NoMedia : UsbOpenFail::NoChip, fail);
      info.baud = hw_->baud();
      ph_ = Ph::Cap;
      return OpenPoll::Pending;

    case Ph::AltFast:  // 4. Лише 2 Мбод (без самоперевірки)
      if (!bring(kUsbBaudFast, now, e)) return OpenPoll::Pending;
      rec(info, kUsbBaudFast, e);
      if (e == Ch376Err::Ok && hw_->baud() >= kCh376MinPlayBaud) {
        ph_ = Ph::AltRead;
      } else if (noMediaErr(e)) {
        return bad(UsbOpenFail::NoMedia, fail);
      } else {
        ph_ = Ph::Auto;
      }
      return OpenPoll::Pending;

    case Ph::AltRead:
      d = hw_->readSector(0, scratch_);
      if (d != DiskErr::Ok) {
        info.diskErr = d;
        return bad(fromDisk(d), fail);
      }
      info.refHash = info.fastHash = usbFnv1a(scratch_, kCh376DiskSecBytes);
      info.selfTest = SelfTest::RefFailed;
      info.baud = hw_->baud();
      ph_ = Ph::Cap;
      return OpenPoll::Pending;

    case Ph::Auto:  // 5. Діагностика: чи живий чіп на нижчих швидкостях
      if (!bring(0, now, e)) return OpenPoll::Pending;
      rec(info, 0, e);
      if (e == Ch376Err::Ok) {
        info.baud = hw_->baud();
        return bad(UsbOpenFail::SlowLink, fail);
      }
      return bad(noMediaErr(e) ? UsbOpenFail::NoMedia : UsbOpenFail::NoChip, fail);

    case Ph::Cap: {
      uint32_t cap = 0;
      d = hw_->capacity(cap);
      if (d != DiskErr::Ok || cap == 0) {
        info.diskErr = d;
        return bad(d == DiskErr::Ok ? UsbOpenFail::ReadError : fromDisk(d), fail);
      }
      info.capacity = cap;
      ph_ = Ph::Pick;
      return OpenPoll::Pending;
    }

    case Ph::Pick: {
      const UsbOpenFail f = usbPickPartition(*hw_, scratch_, *pr_, info);
      ph_ = Ph::End;
      if (f != UsbOpenFail::None) {
        fail = f;
        return OpenPoll::Failed;
      }
      fail = UsbOpenFail::None;
      return OpenPoll::Done;
    }
  }
  return OpenPoll::Pending;
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

// ---- M4c2: відкриття по кроках
void UsbSession::freeTemp() {
  delete pr_;
  delete[] scratch_;
  pr_ = nullptr;
  scratch_ = nullptr;
}

UsbOpenFail UsbSession::openBegin(Ch376Link& link, IUsbOpenHw& hw) {
  info_ = UsbOpenInfo{};
  if (bd_ || pending_) return UsbOpenFail::AlreadyOpen;
  scratch_ = new (std::nothrow) uint8_t[kCh376DiskSecBytes];
  pr_ = new (std::nothrow) ProbeReport;
  disk_ = new (std::nothrow) Ch376Disk(link);
  bd_ = disk_ ? new (std::nothrow) Ch376BlockDev(*disk_) : nullptr;
  if (!scratch_ || !pr_ || !disk_ || !bd_) {
    delete bd_;
    delete disk_;
    bd_ = nullptr;
    disk_ = nullptr;
    freeTemp();
    return UsbOpenFail::NoMemory;
  }
  hw.attach(*disk_);
  hw_ = &hw;
  pending_ = true;
  steps_.begin(hw, scratch_, pr_, info_);
  return UsbOpenFail::None;
}

OpenPoll UsbSession::openStep(uint32_t now, UsbOpenFail& fail) {
  if (!pending_) {
    fail = UsbOpenFail::None;
    return bd_ ? OpenPoll::Done : OpenPoll::Failed;
  }
  const OpenPoll r = steps_.step(now, fail);
  if (r == OpenPoll::Pending) return r;
  pending_ = false;
  freeTemp();
  if (r == OpenPoll::Failed) {
    hw_->portEnd();
    delete bd_;
    delete disk_;
    bd_ = nullptr;
    disk_ = nullptr;
    return r;
  }
  bd_->setPartition(info_.partStart, info_.partSectors);
  return r;
}

void UsbSession::openAbort() {
  if (!pending_) return;
  steps_.abort();
  pending_ = false;
  freeTemp();
  hw_->portEnd();
  delete bd_;
  delete disk_;
  bd_ = nullptr;
  disk_ = nullptr;
}

#endif  // ENABLE_USB_CH376



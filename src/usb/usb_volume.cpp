#include "usb/usb_volume.h"

#if ENABLE_USB_CH376

#include <Arduino.h>
#include <string.h>

#include <new>

#include "SdFat.h"
#include "usb/ch376.h"
#include "usb/ch376_sdfat.h"
#include "usb/ch376_uart.h"
#include "util/log.h"

namespace {

constexpr const char* TAG = "usb";

// Чіп і порт на залізі: Serial2 через ch376_uart, протокол через Ch376, сектори через Ch376Disk сеансу.
class UartOpenHw final : public IUsbOpenHw {
 public:
  void attach(Ch376Disk& d) override { disk_ = &d; }
  bool portBegin() override { return ch376_uart::begin(); }

  Ch376Err bringUp(uint32_t baud) override {
    Ch376& d = ch376_uart::driver();
    {  // скид чіпа закриває файл M4b1; режим очікування зберігаємо (як у DBG,usb,init)
      Ch376Fs& fs = ch376_uart::fs();
      const bool m = fs.useInt();
      const uint32_t g = fs.gapUs();
      fs.reset();
      fs.setUseInt(m);
      fs.setGapUs(g);
      ch376_uart::reader().reset();
    }
    disk_->reset();
    disk_->setSecPerCmd(kCh376SectorsPerCmd);
    const uint32_t t0 = micros();
    Ch376Err e = d.reset();
    if (e == Ch376Err::Ok) e = d.checkExist();
    if (e != Ch376Err::Ok) {  // чіп лишився на швидкості попереднього сеансу: відкат і одна повторна спроба
      d.rollback(kCh376DefaultBaud);
      e = d.reset();
      if (e == Ch376Err::Ok) e = d.checkExist();
    }
    if (e == Ch376Err::Ok) e = negotiate(d, baud);
    if (e == Ch376Err::Ok) e = d.setUsbMode(kCh376ModeHost);
    if (e == Ch376Err::Ok) e = d.waitConnect(kCh376ConnectWaitMs);
    if (e == Ch376Err::Ok) e = d.diskConnect();
    if (e == Ch376Err::Ok) {
      const Ch376Err m = d.diskMount();  // BadFs (напр. GPT 30 ГБ) ігноруємо: сирі сектори читаються й без нього
      if (m == Ch376Err::Disconnected || m == Ch376Err::NoDisk) e = m;
      else if (m != Ch376Err::Ok) LOGI(TAG, "open: чіп не змонтував диск (%s) — це нормально, читаємо сирі сектори", ch376ErrName(m));
    }
    LOGI(TAG, "open: bringUp %u Бод → %s, порт %u Бод, %u мс", (unsigned)baud, ch376ErrName(e), (unsigned)d.baud(),
         (unsigned)((micros() - t0) / 1000u));
    return e;
  }

  uint32_t baud() const override { return ch376_uart::driver().baud(); }
  DiskErr capacity(uint32_t& s) override { return disk_->capacity(s); }
  DiskErr readSector(uint32_t lba, uint8_t* dst) override { return disk_->readSectors(lba, 1, dst); }
  void portEnd() override { stopChip(); }

  // Чіп лишається на узгодженій швидкості: скидаємо його на ній (порт → 9600), інакше наступний open його не знайде.
  static void stopChip() {
    if (!ch376_uart::started()) return;
    Ch376& d = ch376_uart::driver();
    if (d.baud() != kCh376DefaultBaud && d.reset() != Ch376Err::Ok) d.rollback(d.baud());
    ch376_uart::stop();  // Serial2.end(), зняття ISR і семафора
  }

 private:
  static Ch376Err negotiate(Ch376& d, uint32_t baud) {
    Ch376BaudDiv one{0, 0, 0};
    const Ch376BaudDiv* tab = kCh376NegotiateTable;
    size_t n = kCh376NegotiateN;
    if (baud != 0) {
      for (size_t i = 0; i < kCh376BaudTableN; i++)
        if (kCh376BaudTable[i].baud == baud) one = kCh376BaudTable[i];
      if (one.baud == 0) return Ch376Err::BadArg;
      tab = &one;
      n = 1;
    }
    Ch376Attempt att[kCh376NegotiateN];
    size_t nAtt = 0;
    return d.negotiateBaud(tab, n, att, kCh376NegotiateN, nAtt);
  }

  Ch376Disk* disk_ = nullptr;
};

}  // namespace

// ============================================================ UsbVolume

UsbVolume& UsbVolume::get() {
  static UsbVolume v;
  return v;
}

bool UsbVolume::present() const { return vol_ && ses_.isOpen() && ses_.dev()->present(); }

bool UsbVolume::open(UsbVolInfo& out) {
  out = UsbVolInfo{};
  const uint32_t t0 = millis();
  if (vol_) {
    out.fail = UsbOpenFail::AlreadyOpen;
    LOGW(TAG, "open: %s", usbOpenFailText(out.fail));
    return false;
  }
  UartOpenHw hw;
  UsbOpenFail f = ses_.open(ch376_uart::link(), hw);
  const UsbOpenInfo& in = ses_.info();
  for (uint8_t i = 0; i < in.nSteps; i++)
    LOGI(TAG, "open: крок %u: %u Бод → %s", (unsigned)(i + 1), (unsigned)in.steps[i].baud, ch376ErrName(in.steps[i].err));
  if (f == UsbOpenFail::None) {
    LOGI(TAG, "open: самоперевірка 2 Мбод: %s (хеш сектора 0: 1 Мбод %08X, 2 Мбод %08X)", selfTestName(in.selfTest),
         (unsigned)in.refHash, (unsigned)in.fastHash);
    dev_ = new (std::nothrow) Ch376SdDevice(*ses_.dev());
    vol_ = new (std::nothrow) FsVolume;
    if (!dev_ || !vol_) {
      f = UsbOpenFail::NoMemory;
    } else if (!vol_->begin(dev_, false, 0, 0)) {  // setCwv=false: поточний том SD не чіпаємо
      Ch376BlockDev* bd = ses_.dev();
      LOGW(TAG, "open: SdFat begin не вдався: збоїв чіпа %u, поза розділом %u, пристрій %s (%s)", (unsigned)bd->stats().errors,
           (unsigned)bd->stats().rangeErrors, bd->present() ? "є" : "ВІДСУТНІЙ", diskErrName(bd->lastError()));
      f = bd->present() ? UsbOpenFail::MountFailed
                        : ((bd->lastError() == DiskErr::Disconnected || bd->lastError() == DiskErr::NoDisk)
                               ? UsbOpenFail::NoMedia : UsbOpenFail::ReadError);
    }
    if (f != UsbOpenFail::None) {
      release();
      UartOpenHw::stopChip();
    }
  }
  out.fail = f;
  out.openMs = millis() - t0;
  if (f != UsbOpenFail::None) {
    LOGW(TAG, "open: ВІДМОВА (%s): %s", usbOpenFailName(f), usbOpenFailText(f));
    return false;
  }
  out.baud = in.baud;
  out.selfTest = in.selfTest;
  out.partStart = in.partStart;
  out.partSectors = in.partSectors;
  out.partIndex = in.partIndex;
  out.fsType = vol_->fatType();
  out.clusterBytes = vol_->bytesPerCluster();
  memcpy(out.label, in.label, sizeof out.label);
  const uint8_t want = in.kind == VolKind::ExFat ? 64 : (in.kind == VolKind::Fat32 ? 32 : 16);
  if (out.fsType != want)
    LOGW(TAG, "open: тип ФС SdFat (%u) не збігається з розбором disk_probe (%s)", (unsigned)out.fsType, volKindName(in.kind));
  return true;
}

void UsbVolume::release() {
  fileClose();
  if (vol_) {
    vol_->end();
    delete vol_;
    vol_ = nullptr;
  }
  delete dev_;
  dev_ = nullptr;
  ses_.close();
}

void UsbVolume::close() {
  if (!vol_ && !ses_.isOpen() && !ch376_uart::started()) return;
  release();
  UartOpenHw::stopChip();
}

UsbLsRes UsbVolume::listDir(const char* path, uint32_t maxN, UsbLsFn fn, void* ctx, uint32_t& count, bool& more) {
  count = 0;
  more = false;
  if (!vol_) return UsbLsRes::NoVolume;
  if (!ses_.dev()->present()) return UsbLsRes::Gone;
  FsFile* dir = new (std::nothrow) FsFile;
  FsFile* ent = new (std::nothrow) FsFile;
  char* nm = static_cast<char*>(malloc(kMediaNameMax));
  UsbLsRes res = UsbLsRes::Ok;
  if (!dir || !ent || !nm) {
    res = UsbLsRes::NoMemory;
  } else {
    *dir = vol_->open(path, O_RDONLY);
    if (!dir->isOpen()) res = ses_.dev()->present() ? UsbLsRes::OpenFail : UsbLsRes::Gone;
    else if (!dir->isDir()) res = UsbLsRes::NotDir;
    else {
      for (;;) {
        if (!ent->openNext(dir, O_RDONLY)) break;
        const size_t n = ent->getName(nm, kMediaNameMax);
        const bool ok = n > 0 && n < kMediaNameMax;
        if (!ok) nm[0] = 0;
        if (ok && (!strcmp(nm, ".") || !strcmp(nm, ".."))) {
          ent->close();
          continue;
        }
        if (count >= maxN) {
          more = true;
          ent->close();
          break;
        }
        const uint64_t sz = ent->fileSize();
        UsbDirEntry e{nm, ok, ent->isDir(), ent->isHidden(), sz > 0xFFFFFFFFull ? 0xFFFFFFFFu : (uint32_t)sz};
        ent->close();
        fn(e, ctx);
        count++;
      }
      if (!ses_.dev()->present()) res = UsbLsRes::Gone;
    }
  }
  if (dir) {
    if (dir->isOpen()) dir->close();
    delete dir;
  }
  if (ent) {
    if (ent->isOpen()) ent->close();
    delete ent;
  }
  free(nm);
  return res;
}

bool UsbVolume::fileOpen(const char* path, uint32_t& size) {
  fileClose();
  size = 0;
  if (!vol_ || !ses_.dev()->present()) return false;
  FsFile* f = new (std::nothrow) FsFile;
  if (!f) return false;
  *f = vol_->open(path, O_RDONLY);
  if (!f->isOpen() || !f->isFile()) {
    if (f->isOpen()) f->close();
    delete f;
    return false;
  }
  file_ = f;
  const uint64_t sz = f->fileSize();
  size = sz > 0xFFFFFFFFull ? 0xFFFFFFFFu : (uint32_t)sz;
  return true;
}

int UsbVolume::fileRead(void* dst, size_t n) {
  if (!file_ || !dst || !ses_.dev()->present()) return -1;
  return file_->read(dst, n);
}

bool UsbVolume::fileSeek(uint32_t pos) {
  if (!file_ || !ses_.dev()->present()) return false;
  return file_->seekSet(pos);
}

bool UsbVolume::fileIsOpen() const { return file_ && file_->isOpen(); }

void UsbVolume::fileClose() {
  if (!file_) return;
  if (file_->isOpen()) file_->close();
  delete file_;
  file_ = nullptr;
}

// ============================================================ UsbFileReader
// Семантика як у SdFileReader: read() = 0 — кінець або помилка (failed()); після зникнення носія — одразу 0.

bool UsbFileReader::isOpen() const { return v_.fileIsOpen(); }

void UsbFileReader::close() { v_.fileClose(); }

bool UsbFileReader::open(const char* path) {
  close();
  failed_ = false;
  pos_ = size_ = 0;
  if (!v_.fileOpen(path, size_)) {
    failed_ = true;
    return false;
  }
  return true;
}

size_t UsbFileReader::read(void* dst, size_t n) {
  if (n == 0) return 0;
  if (!v_.fileIsOpen() || !v_.present()) {
    failed_ = true;
    return 0;
  }
  const int r = v_.fileRead(dst, n);
  if (r < 0 || (r == 0 && pos_ < size_)) {
    failed_ = true;
    return 0;
  }
  pos_ += (uint32_t)r;
  return (size_t)r;
}

bool UsbFileReader::seek(uint32_t p) {
  if (!v_.fileIsOpen() || !v_.present()) {
    failed_ = true;
    return false;
  }
  if (!v_.fileSeek(p)) return false;
  pos_ = p;
  return true;
}

#endif  // ENABLE_USB_CH376

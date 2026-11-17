#include "usb/usb_volume.h"

#if ENABLE_USB_CH376

#include <Arduino.h>
#include <stdio.h>
#include <string.h>

#include <new>

#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "SdFat.h"
#include "usb/ch376.h"
#include "usb/ch376_sdfat.h"
#include "usb/ch376_uart.h"
#include "util/log.h"

namespace {

constexpr const char* TAG = "usb";

// M4b4: один рекурсивний мʼютекс на весь USB-том (SdFat і UART чіпа не потокобезпечні). Створюється разом із синглтоном.
SemaphoreHandle_t s_mtx = nullptr;
bool s_profOn = false;                 // облік скану (profBegin/profEnd)
uint32_t s_lkN = 0, s_lkUs = 0, s_lkMax = 0;
struct Lk {
  bool ok;
  explicit Lk(uint32_t ms = kUsbLockWaitMs) : ok(false) {
    if (!s_mtx) return;
    if (!s_profOn) {
      ok = xSemaphoreTakeRecursive(s_mtx, pdMS_TO_TICKS(ms)) == pdTRUE;
      return;
    }
    const int64_t t = esp_timer_get_time();
    ok = xSemaphoreTakeRecursive(s_mtx, pdMS_TO_TICKS(ms)) == pdTRUE;
    const uint32_t d = (uint32_t)(esp_timer_get_time() - t);
    s_lkN++;
    s_lkUs += d;
    if (d > s_lkMax) s_lkMax = d;
  }
  ~Lk() { if (ok) xSemaphoreGiveRecursive(s_mtx); }
  Lk(const Lk&) = delete;
  Lk& operator=(const Lk&) = delete;
};

// Час виконання операції під мʼютексом (після його захоплення); працює лише коли облік увімкнено.
struct PT {
  bool on;
  int64_t t0;
  uint32_t &n, &us;
  uint32_t* mx;
  PT(bool o, uint32_t& n_, uint32_t& us_, uint32_t* mx_ = nullptr) : on(o), t0(o ? esp_timer_get_time() : 0), n(n_), us(us_), mx(mx_) {}
  ~PT() {
    if (!on) return;
    const uint32_t d = (uint32_t)(esp_timer_get_time() - t0);
    n++;
    us += d;
    if (mx && d > *mx) *mx = d;
  }
};

// Швидкість у «Мбод» для логу: 2000000 → "2", 1500000 → "1,5" (значення беруться з констант, а не з тексту).
void fmtMbod(char* out, size_t cap, uint32_t baud) {
  const unsigned w = (unsigned)(baud / 1000000u), f = (unsigned)((baud % 1000000u) / 100000u);
  if (f) snprintf(out, cap, "%u,%u", w, f);
  else snprintf(out, cap, "%u", w);
}

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

UsbVolume::UsbVolume() : rd_(*this) {
  if (!s_mtx) s_mtx = xSemaphoreCreateRecursiveMutex();
}

UsbVolume& UsbVolume::get() {
  static UsbVolume v;
  return v;
}

bool UsbVolume::present() const {
  Lk g;
  return g.ok && vol_ && ses_.isOpen() && ses_.dev()->present();
}

bool UsbVolume::presentNoWait() const {
  Lk g(0);
  if (!g.ok) return true;  // чіп зайнятий обміном — пристрій явно відповідає
  return vol_ && ses_.isOpen() && ses_.dev()->present();
}

bool UsbVolume::lockFor(uint32_t ms) { return s_mtx && xSemaphoreTakeRecursive(s_mtx, pdMS_TO_TICKS(ms)) == pdTRUE; }

void UsbVolume::unlock() {
  if (s_mtx) xSemaphoreGiveRecursive(s_mtx);
}

bool UsbVolume::open(UsbVolInfo& out) {
  out = UsbVolInfo{};
  Lk lk;
  if (!lk.ok) {
    out.fail = UsbOpenFail::NoPort;
    LOGW(TAG, "open: мʼютекс USB зайнятий довше %u мс", (unsigned)kUsbLockWaitMs);
    return false;
  }
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
    char mFast[10], mRef[10];
    fmtMbod(mFast, sizeof mFast, kUsbBaudFast);
    fmtMbod(mRef, sizeof mRef, kUsbBaudRef);
    LOGI(TAG, "open: самоперевірка %s Мбод: %s (хеш сектора 0: %s Мбод %08X, %s Мбод %08X)", mFast, selfTestName(in.selfTest), mRef,
         (unsigned)in.refHash, mFast, (unsigned)in.fastHash);
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
  serial_ = in.serial;
  out.serial = in.serial;
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

void UsbVolume::closeDirs() {
  for (int i = 0; i < kUsbDirHandles; i++) {
    if (!dirs_[i]) continue;
    if (dirs_[i]->isOpen()) dirs_[i]->close();
    delete dirs_[i];
    dirs_[i] = nullptr;
  }
}

void UsbVolume::release() {
  fileClose();
  closeDirs();
  serial_ = 0;
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
  Lk lk;  // не вдалось (мʼютекс чужий довше kUsbLockWaitMs) — все одно закриваємо: виклик лише коли читачів немає
  if (!vol_ && !ses_.isOpen() && !ch376_uart::started()) return;
  release();
  UartOpenHw::stopChip();
}

UsbLsRes UsbVolume::listDir(const char* path, uint32_t maxN, UsbLsFn fn, void* ctx, uint32_t& count, bool& more) {
  count = 0;
  more = false;
  Lk lk;
  if (!lk.ok) return UsbLsRes::Gone;
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

// ---- перелік каталогу по одному запису (M4b4)
void UsbVolume::profBegin() {
  Lk lk;
  prof_ = UsbScanProf{};
  s_lkN = s_lkUs = s_lkMax = 0;
  base0_[0] = base0_[1] = base0_[2] = 0;
  if (vol_ && ses_.isOpen()) {
    const BlockDevStats& b = ses_.dev()->stats();
    base0_[0] = b.calls;
    base0_[1] = b.sectors;
    base0_[2] = b.cmds;
  }
  prof_.on = true;
  s_profOn = true;
}

void UsbVolume::profEnd() {
  if (!prof_.on) return;
  s_profOn = false;
  prof_.on = false;
  prof_.lockN = s_lkN;
  prof_.lockWaitUs = s_lkUs;
  prof_.lockWaitMaxUs = s_lkMax;
  Lk lk;
  if (vol_ && ses_.isOpen()) {
    const BlockDevStats& b = ses_.dev()->stats();
    prof_.calls = b.calls - base0_[0];
    prof_.sectors = b.sectors - base0_[1];
    prof_.cmds = b.cmds - base0_[2];
  }
}

int UsbVolume::dirOpen(const char* path) {
  Lk lk;
  PT pt(prof_.on, prof_.openDirN, prof_.openDirUs, &prof_.openDirMaxUs);
  if (!lk.ok || !vol_ || !ses_.dev()->present()) return -1;
  for (int i = 0; i < kUsbDirHandles; i++) {
    if (dirs_[i]) continue;
    FsFile* d = new (std::nothrow) FsFile;
    if (!d) return -1;
    *d = vol_->open(path, O_RDONLY);
    if (!d->isOpen() || !d->isDir()) {
      if (d->isOpen()) d->close();
      delete d;
      return -1;
    }
    dirs_[i] = d;
    return i;
  }
  return -1;
}

int UsbVolume::dirNext(int h, char* name, size_t cap, bool& nameOk, bool& isDir, bool& hidden) {
  Lk lk;
  PT pt(prof_.on, prof_.nextN, prof_.nextUs, &prof_.nextMaxUs);
  if (!lk.ok || !vol_ || h < 0 || h >= kUsbDirHandles || !dirs_[h]) return -1;
  if (!ses_.dev()->present()) {
    if (prof_.on) prof_.errN++;
    return -1;
  }
  FsFile ent;
  if (!ent.openNext(dirs_[h], O_RDONLY)) {  // кінець каталогу або зникнення носія
    const bool alive = ses_.dev()->present();
    if (prof_.on) (alive ? prof_.endN : prof_.errN)++;
    return alive ? 0 : -1;
  }
  if (prof_.on) prof_.entries++;
  const size_t n = ent.getName(name, cap);
  nameOk = n > 0 && n < cap;
  if (!nameOk) name[0] = 0;
  isDir = ent.isDir();
  hidden = ent.isHidden();
  ent.close();
  return 1;
}

void UsbVolume::dirClose(int h) {
  Lk lk;
  PT pt(prof_.on, prof_.closeN, prof_.closeUs);
  if (!lk.ok || h < 0 || h >= kUsbDirHandles || !dirs_[h]) return;
  if (dirs_[h]->isOpen()) dirs_[h]->close();
  delete dirs_[h];
  dirs_[h] = nullptr;
}

bool UsbVolume::fileOpen(const char* path, uint32_t& size) {
  Lk lk;
  fileClose();
  size = 0;
  if (!lk.ok || !vol_ || !ses_.dev()->present()) return false;
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
  Lk lk;
  if (!lk.ok) return -1;
  if (!file_ || !dst || !ses_.dev()->present()) return -1;
  return file_->read(dst, n);
}

bool UsbVolume::fileSeek(uint32_t pos) {
  Lk lk;
  if (!lk.ok) return false;
  if (!file_ || !ses_.dev()->present()) return false;
  return file_->seekSet(pos);
}

bool UsbVolume::fileIsOpen() const {
  Lk lk;
  return lk.ok && file_ && file_->isOpen();
}

void UsbVolume::fileClose() {
  Lk lk;
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


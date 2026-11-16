#include "usb/ch376_disk.h"

#include "config.h"

#if ENABLE_USB_CH376

#include <string.h>

// Протокол (за даташитом, звірити на залізі): DISK_READ = 57 AB 54 [LBA 4 Б LE] [n]. Для кожного сектора 8 блоків:
// статус 0x1D → RD_USB_DATA0 (довжина 64 + дані) → DISK_RD_GO (0x55). Після останнього блоку — статус 0x14.
// DISK_CAPACITY = 57 AB 3E: статус 0x14 → RD_USB_DATA0 (довжина 4, кількість секторів LE).
// Очікування статусу: INT# (якщо є й увімкнено) → GET_STATUS; запасний шлях — опитування без затримок на 1 мс.

const char* diskErrName(DiskErr e) {
  switch (e) {
    case DiskErr::Ok: return "ok";
    case DiskErr::NoReply: return "noreply";
    case DiskErr::BadReply: return "badreply";
    case DiskErr::Timeout: return "timeout";
    case DiskErr::Aborted: return "aborted";
    case DiskErr::Disconnected: return "disconn";
    case DiskErr::NoDisk: return "nodisk";
    case DiskErr::ReadError: return "readerr";
    case DiskErr::BadArg: return "badarg";
    case DiskErr::ChipStatus: return "chipstat";
  }
  return "?";
}

DiskErr Ch376Disk::mapStatus(uint8_t st) {
  switch (st) {
    case kCh376IntSuccess: return DiskErr::Ok;
    case kCh376IntDisconnect: return DiskErr::Disconnected;
    case kCh376ErrDiskDisc: return DiskErr::NoDisk;
    case kCh376IntDiskErr:
    case kCh376IntBufOver: return DiskErr::ReadError;
    default: return DiskErr::ChipStatus;
  }
}

void Ch376Disk::reset() {
  useInt_ = kCh376FsUseInt;
  secPerCmd_ = kCh376DiskSecPerCmdDef;
  settle_ = kCh376DiskSettleDef;
  skipSt_ = false;
  capKnown_ = false;
  cap_ = 0;
  lastSt_ = 0;
  tWait_ = tStat_ = 0;
  st_ = DiskStats{};
}

void Ch376Disk::setSecPerCmd(uint32_t n) {
  if (n >= 1 && n <= kCh376DiskMaxSectors) secPerCmd_ = n;
}

DiskErr Ch376Disk::failCmd(DiskErr e) {
  st_.errors++;
  return e;
}

DiskErr Ch376Disk::send(uint8_t cmd, const uint8_t* a, size_t na) {
  if (na > 5) return DiskErr::BadArg;
  if (link_.abortRequested()) return DiskErr::Aborted;
  uint8_t f[8] = {kCh376Sync0, kCh376Sync1, cmd, 0, 0, 0, 0, 0};
  for (size_t i = 0; i < na; i++) f[3 + i] = a[i];
  link_.flushRx();  // відповіді на попередню команду вже не потрібні
  link_.write(f, 3 + na);
  return DiskErr::Ok;
}

// GET_STATUS: один байт. Після нього — пауза, щоб запізнілий дубль статусу прийшов і був скинутий наступною командою.
DiskErr Ch376Disk::getStatus(uint8_t& st) {
  DiskErr e = send(kCh376CmdGetStatus, nullptr, 0);
  if (e != DiskErr::Ok) return e;
  uint8_t b = 0;
  if (!link_.readByte(b, kCh376ReplyMs)) return DiskErr::NoReply;
  st = b;
  const uint32_t w = settle_ * link_.byteTimeUs();
  if (w) {
    const uint32_t t = link_.nowUs();
    while ((uint32_t)(link_.nowUs() - t) < w) link_.yieldNow();
  }
  return DiskErr::Ok;
}

// Чекає ненульового статусу. INT режим: waitIntLow (≤ kCh376DiskIntSliceMs) → GET_STATUS, і GET_STATUS виконується
// навіть без INT (запасний шлях, лічильник intMisses). Режим опитування: GET_STATUS + yieldNow().
DiskErr Ch376Disk::waitStatus(uint8_t& st, uint32_t tmoMs) {
  const uint32_t t0 = link_.nowUs();
  const uint32_t lim = tmoMs * 1000u;
  const bool viaInt = useInt_ && link_.hasIntPin();
  for (;;) {
    if (link_.abortRequested()) return DiskErr::Aborted;
    const uint32_t el = (uint32_t)(link_.nowUs() - t0);
    if (el >= lim) return DiskErr::Timeout;
    if (viaInt) {
      uint32_t slice = lim - el;
      if (slice > kCh376DiskIntSliceMs * 1000u) slice = kCh376DiskIntSliceMs * 1000u;
      if (link_.waitIntLow(slice)) st_.intWaits++;
      else st_.intMisses++;
    }
    const uint32_t tg = link_.nowUs();
    uint8_t s = kCh376IntNone;
    DiskErr e = getStatus(s);
    if (e != DiskErr::Ok) return e;
    if (s != kCh376IntNone) {
      st = s;
      lastSt_ = s;
      tWait_ = (uint32_t)(tg - t0);
      tStat_ = (uint32_t)(link_.nowUs() - tg);
      return DiskErr::Ok;
    }
    st_.polls++;
    if (!viaInt) link_.yieldNow();
  }
}

// RD_USB_DATA0 для блоку: довжина має бути 64. Інакше BadReply, і хвіст вичитується, щоб не зсунути наступні відповіді.
DiskErr Ch376Disk::readBlock(uint8_t* dst) {
  DiskErr e = send(kCh376CmdRdUsbData0, nullptr, 0);
  if (e != DiskErr::Ok) return e;
  uint8_t l = 0;
  if (!link_.readByte(l, kCh376DiskDataMs)) return DiskErr::Timeout;
  uint8_t tmp[kCh376DiskBlkBytes];
  if (l != kCh376DiskBlkBytes) {
    uint8_t junk[16];
    for (int i = 0; i < 8; i++) {
      if (link_.readBytes(junk, sizeof junk, 2000u) == 0) break;
    }
    return DiskErr::BadReply;
  }
  uint8_t* out = dst ? dst : tmp;
  if (link_.readBytes(out, kCh376DiskBlkBytes, kCh376DiskDataMs * 1000u) < kCh376DiskBlkBytes) return DiskErr::Timeout;
  return DiskErr::Ok;
}

// Одна DISK_READ на n секторів (n ≤ kCh376DiskMaxSectors).
DiskErr Ch376Disk::oneCmd(uint32_t lba, uint32_t n, uint8_t* dst) {
  const uint8_t a[5] = {(uint8_t)lba, (uint8_t)(lba >> 8), (uint8_t)(lba >> 16), (uint8_t)(lba >> 24), (uint8_t)n};
  const uint32_t tc = link_.nowUs();
  DiskErr e = send(kCh376CmdDiskRead, a, 5);
  if (e != DiskErr::Ok) return failCmd(e);
  const uint32_t total = n * kCh376DiskBlksPerSec;
  for (uint32_t i = 0; i < total; i++) {
    const uint32_t tb = link_.nowUs();
    if (i == 0 || !skipSt_) {
      uint8_t st = 0;
      e = waitStatus(st, kCh376DiskOpMs);
      if (e != DiskErr::Ok) return failCmd(e);
      st_.waitUs += tWait_;
      st_.statUs += tStat_;
      if (st != kCh376IntDiskRead) {
        const DiskErr m = mapStatus(st);
        return failCmd(m == DiskErr::Ok ? DiskErr::ChipStatus : m);
      }
    } else {
      tWait_ = tStat_ = 0;  // експеримент (г): статус не запитано
    }
    uint8_t* d = dst ? dst + (size_t)i * kCh376DiskBlkBytes : nullptr;
    const uint32_t tx = link_.nowUs();
    e = readBlock(d);
    if (e != DiskErr::Ok) return failCmd(e);
    const uint32_t tr = link_.nowUs();
    e = send(kCh376CmdDiskRdGo, nullptr, 0);
    if (e != DiskErr::Ok) return failCmd(e);
    const uint32_t blk = (uint32_t)(tr - tb);
    st_.blocks++;
    st_.blkSumUs += blk;
    if (blk < st_.blkMinUs) st_.blkMinUs = blk;
    if (blk > st_.blkMaxUs) st_.blkMaxUs = blk;
    st_.xferUs += (uint32_t)(tr - tx);
  }
  // Кінець команди: статус 0x14.
  uint8_t st = 0;
  e = waitStatus(st, kCh376DiskOpMs);
  if (e != DiskErr::Ok) return failCmd(e);
  st_.waitUs += tWait_;
  st_.statUs += tStat_;
  e = mapStatus(st);
  if (e != DiskErr::Ok) return failCmd(e);
  const uint32_t dc = (uint32_t)(link_.nowUs() - tc);
  st_.cmds++;
  st_.sectors += n;
  st_.cmdSumUs += dc;
  if (dc > st_.cmdMaxUs) st_.cmdMaxUs = dc;
  return DiskErr::Ok;
}

DiskErr Ch376Disk::capacity(uint32_t& sectors) {
  capKnown_ = false;
  DiskErr e = send(kCh376CmdDiskCapacity, nullptr, 0);
  if (e != DiskErr::Ok) return e;
  uint8_t st = 0;
  e = waitStatus(st, kCh376DiskOpMs);
  if (e != DiskErr::Ok) return e;
  e = mapStatus(st);
  if (e != DiskErr::Ok) return e;
  e = send(kCh376CmdRdUsbData0, nullptr, 0);
  if (e != DiskErr::Ok) return e;
  uint8_t l = 0;
  if (!link_.readByte(l, kCh376DiskDataMs)) return DiskErr::Timeout;
  uint8_t b[kCh376DiskCapLen];
  if (l != kCh376DiskCapLen || link_.readBytes(b, sizeof b, kCh376DiskDataMs * 1000u) < sizeof b) return DiskErr::BadReply;
  sectors = (uint32_t)b[0] | ((uint32_t)b[1] << 8) | ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24);
  cap_ = sectors;
  capKnown_ = true;
  return DiskErr::Ok;
}

DiskErr Ch376Disk::readSectors(uint32_t lba, uint32_t n, uint8_t* dst) {
  if (n == 0) return DiskErr::BadArg;
  if (capKnown_ && (uint64_t)lba + n > cap_) return DiskErr::BadArg;
  uint32_t done = 0;
  while (done < n) {
    const uint32_t left = n - done;
    const uint32_t k = left < secPerCmd_ ? left : secPerCmd_;
    DiskErr e = oneCmd(lba + done, k, dst ? dst + (size_t)done * kCh376DiskSecBytes : nullptr);
    if (e != DiskErr::Ok) return e;
    done += k;
  }
  return DiskErr::Ok;
}

#endif  // ENABLE_USB_CH376

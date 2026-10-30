#include "config.h"

#if ENABLE_USB_CH376

#include "usb/ch376_fs.h"

#include <string.h>

// Файловий шар CH376S: кадри 0x57 0xAB <код> [аргументи]. SET_FILE_NAME іде ОДНИМ записом (код + рядок + 0x00).
// Читання: BYTE_READ(n) → повтор { статус 0x1D → RD_USB_DATA0 (довжина + дані) → BYTE_RD_GO } → статус 0x14.
// Очікування статусу: INT# (якщо є й увімкнено) → один GET_STATUS; запасний шлях — опитування GET_STATUS без затримок на 1 мс.

const char* ch376FsErrName(FsErr e) {
  switch (e) {
    case FsErr::Ok: return "ok";
    case FsErr::NotFound: return "notfound";
    case FsErr::IsDir: return "isdir";
    case FsErr::NotDir: return "notdir";
    case FsErr::Disconnected: return "disconn";
    case FsErr::ReadError: return "readerr";
    case FsErr::Timeout: return "timeout";
    case FsErr::NoReply: return "noreply";
    case FsErr::BadReply: return "badreply";
    case FsErr::BadArg: return "badarg";
    case FsErr::BadName: return "badname";
    case FsErr::NotOpen: return "notopen";
    case FsErr::Aborted: return "aborted";
    case FsErr::ChipStatus: return "chipstatus";
  }
  return "?";
}

namespace {

// Запис каталогу (32 Б) → Ch376DirEntry. false: пропустити (".", "..", мітка тому, довгі записи, видалені).
bool parseEntry(const uint8_t* r, Ch376DirEntry& e) {
  const uint8_t attr = r[kCh376DirOffAttr];
  if (r[0] == '.' || r[0] == 0xE5 || r[0] == 0x00) return false;
  if (attr & kCh376AttrVolume) return false;  // мітка тому; довгі записи (0x0F) теж мають цей біт
  size_t k = 0;
  auto put = [&](uint8_t c) { e.name[k++] = (c >= 0x20 && c < 0x7F) ? (char)c : '?'; };
  for (size_t i = 0; i < 8 && r[i] != ' '; i++) put(r[i]);
  if (r[8] != ' ') {
    e.name[k++] = '.';
    for (size_t i = 8; i < 11 && r[i] != ' '; i++) put(r[i]);
  }
  e.name[k] = 0;
  if (k == 0) return false;
  e.attr = attr;
  e.size = (uint32_t)r[kCh376DirOffSize] | ((uint32_t)r[kCh376DirOffSize + 1] << 8) |
           ((uint32_t)r[kCh376DirOffSize + 2] << 16) | ((uint32_t)r[kCh376DirOffSize + 3] << 24);
  return true;
}

}  // namespace

void Ch376Fs::reset() {
  useInt_ = kCh376FsUseInt;
  gapUs_ = 0;
  open_ = false;
  lastSt_ = 0;
  st_ = Ch376FsStats{};
  tWaitUs_ = tStatUs_ = 0;
}

bool Ch376Fs::valid83(const char* s, size_t n) {
  if (!s || n == 0 || n > kCh376FsNameMax) return false;
  size_t base = 0, ext = 0;
  bool dot = false;
  for (size_t i = 0; i < n; i++) {
    const char c = s[i];
    if (c == '.') {
      if (dot || i == 0) return false;
      dot = true;
      continue;
    }
    const bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
                    (c != 0 && strchr("$%'-_@~`!(){}^#&", c) != nullptr);
    if (!ok) return false;
    if (dot) ext++;
    else base++;
  }
  if (base == 0 || base > 8 || ext > 3) return false;
  if (dot && ext == 0) return false;
  return true;
}

FsErr Ch376Fs::mapStatus(uint8_t st) {
  switch (st) {
    case kCh376IntSuccess: return FsErr::Ok;
    case kCh376IntDisconnect:
    case kCh376ErrDiskDisc: return FsErr::Disconnected;
    case kCh376ErrMissFile: return FsErr::NotFound;
    case kCh376IntDiskErr:
    case kCh376IntBufOver: return FsErr::ReadError;
    case kCh376ErrFoundName: return FsErr::NotDir;
    default: return FsErr::ChipStatus;
  }
}

FsErr Ch376Fs::send(uint8_t cmd, const uint8_t* a, size_t na) {
  if (na > 5) return FsErr::BadArg;
  if (link_.abortRequested()) return FsErr::Aborted;
  uint8_t f[8] = {kCh376Sync0, kCh376Sync1, cmd, 0, 0, 0, 0, 0};
  for (size_t i = 0; i < na; i++) f[3 + i] = a[i];
  link_.flushRx();  // відповіді на попередню команду вже не потрібні
  link_.write(f, 3 + na);
  return FsErr::Ok;
}

// SET_FILE_NAME: [root → '/'] + імʼя у ВЕЛИКИХ літерах + 0x00, одним записом.
FsErr Ch376Fs::sendName(const char* s, size_t n, bool root) {
  if (n > kCh376FsNameMax) return FsErr::BadName;
  if (link_.abortRequested()) return FsErr::Aborted;
  uint8_t f[3 + 1 + kCh376FsNameMax + 1];
  size_t k = 0;
  f[k++] = kCh376Sync0;
  f[k++] = kCh376Sync1;
  f[k++] = kCh376CmdSetFileName;
  if (root) f[k++] = '/';
  for (size_t i = 0; i < n; i++) {
    char c = s[i];
    if (c >= 'a' && c <= 'z') c = (char)(c - 32);
    f[k++] = (uint8_t)c;
  }
  f[k++] = 0;
  link_.flushRx();
  link_.write(f, k);
  if (gapUs_) {
    const uint32_t t = link_.nowUs();
    while ((uint32_t)(link_.nowUs() - t) < gapUs_) link_.yieldNow();
  }
  return FsErr::Ok;
}

FsErr Ch376Fs::getStatus(uint8_t& st) {
  FsErr e = send(kCh376CmdGetStatus, nullptr, 0);
  if (e != FsErr::Ok) return e;
  uint8_t b = 0;
  if (!link_.readByte(b, kCh376ReplyMs)) return FsErr::NoReply;
  st = b;
  // Чіп шле статус двічі (по INT і у відповідь на GET_STATUS): другий байт приходить із запізненням, що залежить від
  // швидкості, і потрапляв би на початок наступної відповіді. Даємо йому час прийти; наступна команда скине RX.
  const uint32_t w = kCh376FsSettleBytes * link_.byteTimeUs();
  if (w) {
    const uint32_t t = link_.nowUs();
    while ((uint32_t)(link_.nowUs() - t) < w) link_.yieldNow();
  }
  return FsErr::Ok;
}

// Чекає ненульового статусу. INT режим: waitIntLow (≤ kCh376FsIntSliceMs) → GET_STATUS; якщо INT не прийшов за зріз,
// GET_STATUS усе одно виконується (запасний шлях, лічильник intMisses). Режим опитування: GET_STATUS + yieldNow().
FsErr Ch376Fs::waitStatus(uint8_t& st, uint32_t tmoMs) {
  const uint32_t t0 = link_.nowUs();
  const uint32_t lim = tmoMs * 1000u;
  const bool viaInt = useInt_ && link_.hasIntPin();
  for (;;) {
    if (link_.abortRequested()) return FsErr::Aborted;
    const uint32_t el = (uint32_t)(link_.nowUs() - t0);
    if (el >= lim) return FsErr::Timeout;
    if (viaInt) {
      uint32_t slice = lim - el;
      if (slice > kCh376FsIntSliceMs * 1000u) slice = kCh376FsIntSliceMs * 1000u;
      if (link_.waitIntLow(slice)) st_.intWaits++;
      else st_.intMisses++;
    }
    const uint32_t tg = link_.nowUs();
    uint8_t s = kCh376IntNone;
    FsErr e = getStatus(s);
    if (e != FsErr::Ok) return e;
    if (s != kCh376IntNone) {
      st = s;
      lastSt_ = s;
      tWaitUs_ = (uint32_t)(tg - t0);
      tStatUs_ = (uint32_t)(link_.nowUs() - tg);
      return FsErr::Ok;
    }
    st_.polls++;
    if (!viaInt) link_.yieldNow();
  }
}

// RD_USB_DATA0: [довжина][дані]. Довжина 0 або > cap — BadReply (хвіст вичитується, щоб не псувати наступні відповіді).
// dst == nullptr — дані приймаються й відкидаються. Зайвий дубль статусу перед відповіддю прибирає пауза в getStatus() + flushRx().
FsErr Ch376Fs::readData(uint8_t* dst, size_t cap, size_t& len) {
  len = 0;
  FsErr e = send(kCh376CmdRdUsbData0, nullptr, 0);
  if (e != FsErr::Ok) return e;
  uint8_t l = 0;
  if (!link_.readByte(l, kCh376FsDataMs)) return FsErr::Timeout;
  if (l == 0 || l > cap) {
    for (int i = 0; i < 8 && link_.readBytes(blk_, sizeof blk_, 2000u) > 0; i++) {}
    return FsErr::BadReply;
  }
  if (dst) {
    if (link_.readBytes(dst, l, kCh376FsDataMs * 1000u) < l) return FsErr::Timeout;
  } else {
    uint8_t tmp[32];
    for (size_t left = l; left > 0;) {
      const size_t k = left < sizeof tmp ? left : sizeof tmp;
      if (link_.readBytes(tmp, k, kCh376FsDataMs * 1000u) < k) return FsErr::Timeout;
      left -= k;
    }
  }
  len = l;
  return FsErr::Ok;
}

FsErr Ch376Fs::failRead(FsErr e) {
  st_.errors++;
  if (e == FsErr::Disconnected) open_ = false;
  return e;
}

void Ch376Fs::noteBlock(uint32_t t0, uint32_t t1, uint32_t t2) {
  const uint32_t d = (uint32_t)(t2 - t0);
  st_.blocks++;
  st_.blkSumUs += d;
  if (d < st_.blkMinUs) st_.blkMinUs = d;
  if (d > st_.blkMaxUs) st_.blkMaxUs = d;
  st_.waitUs += tWaitUs_;
  st_.statUs += tStatUs_;
  st_.xferUs += (uint32_t)(t2 - t1);
}

FsErr Ch376Fs::openStep(const char* el, size_t n, bool root, bool last, FsKind& kind) {
  FsErr e = sendName(el, n, root);
  if (e != FsErr::Ok) return e;
  e = send(kCh376CmdFileOpen, nullptr, 0);
  if (e != FsErr::Ok) return e;
  uint8_t st = 0;
  e = waitStatus(st, kCh376FsOpMs);
  if (e != FsErr::Ok) return e;
  if (st == kCh376ErrOpenDir) {
    kind = FsKind::Dir;
    return FsErr::Ok;
  }
  if (st == kCh376IntSuccess) {
    open_ = true;
    kind = FsKind::File;
    if (!last) {  // файл посеред шляху
      close();
      return FsErr::NotDir;
    }
    return FsErr::Ok;
  }
  return mapStatus(st);
}

FsErr Ch376Fs::openPath(const char* path, FsKind& kind) {
  kind = FsKind::None;
  if (!path) return FsErr::BadArg;
  // 1) весь шлях перевіряється до першого звернення до чіпа
  const char* el[kCh376FsDepthMax];
  uint8_t len[kCh376FsDepthMax];
  size_t cnt = 0;
  for (const char* p = path; *p;) {
    if (*p == '/') {
      p++;
      continue;
    }
    const char* b = p;
    while (*p && *p != '/') p++;
    const size_t n = (size_t)(p - b);
    if (cnt >= kCh376FsDepthMax || !valid83(b, n)) return FsErr::BadName;
    el[cnt] = b;
    len[cnt] = (uint8_t)n;
    cnt++;
  }
  if (open_) close();
  uint8_t stale = 0;
  getStatus(stale);  // непрочитана подія (напр. зайва) не має підмінити статус першої команди
  // 2) поетапно: перший елемент — від кореня ("/ІМ'Я"), наступні — відносно відкритого каталогу
  FsErr e = FsErr::Ok;
  if (cnt == 0) {
    e = openStep("", 0, true, true, kind);  // "/" — корінь
  } else {
    for (size_t i = 0; i < cnt && e == FsErr::Ok; i++) e = openStep(el[i], len[i], i == 0, i + 1 == cnt, kind);
  }
  if (e != FsErr::Ok) kind = FsKind::None;
  return e;
}

FsErr Ch376Fs::rawOpen(const char* name, uint8_t& st, uint8_t* entry, bool& hasEntry) {
  hasEntry = false;
  st = 0;
  if (!name || !entry) return FsErr::BadArg;
  if (open_) close();
  uint8_t stale = 0;
  getStatus(stale);
  FsErr e = sendName(name, strlen(name), false);
  if (e != FsErr::Ok) return e;
  e = send(kCh376CmdFileOpen, nullptr, 0);
  if (e != FsErr::Ok) return e;
  e = waitStatus(st, kCh376FsOpMs);
  if (e != FsErr::Ok) return e;
  if (st == kCh376IntSuccess) open_ = true;
  if (st == kCh376IntDiskRead) {
    size_t len = 0;
    e = readData(blk_, kCh376DirInfoLen, len);
    if (e != FsErr::Ok) return e;
    memcpy(entry, blk_, len);
    hasEntry = len == kCh376DirInfoLen;
  }
  return FsErr::Ok;
}

FsErr Ch376Fs::rawCmd(uint8_t cmd, const uint8_t* a, size_t na, uint8_t& st, uint8_t* data, size_t& dlen) {
  dlen = 0;
  st = 0;
  if (!data) return FsErr::BadArg;
  if (open_) close();
  uint8_t stale = 0;
  getStatus(stale);
  FsErr e = send(cmd, a, na);
  if (e != FsErr::Ok) return e;
  e = waitStatus(st, kCh376FsOpMs);
  if (e != FsErr::Ok) return e;
  if (send(kCh376CmdRdUsbData0, nullptr, 0) != FsErr::Ok) return FsErr::Ok;
  uint8_t l = 0;
  lastLen_ = link_.readByte(l, 30) ? l : 0xEE;  // 0xEE — відповіді немає
  if (lastLen_ != 0xEE && l > 0 && l <= kCh376StageMax) dlen = link_.readBytes(data, l, kCh376FsDataMs * 1000u);
  else link_.flushRx();
  return FsErr::Ok;
}

FsErr Ch376Fs::rawSector(uint32_t lba, uint8_t block, uint8_t* out, size_t& len) {
  len = 0;
  if (block > 7 || !out) return FsErr::BadArg;
  if (open_) close();
  uint8_t stale = 0;
  getStatus(stale);
  const uint8_t a[5] = {(uint8_t)lba, (uint8_t)(lba >> 8), (uint8_t)(lba >> 16), (uint8_t)(lba >> 24), 1};
  FsErr e = send(kCh376CmdDiskRead, a, 5);
  if (e != FsErr::Ok) return e;
  for (uint8_t b = 0; b < 8; b++) {
    uint8_t st = 0;
    e = waitStatus(st, kCh376FsOpMs);
    if (e != FsErr::Ok) return e;
    if (st != kCh376IntDiskRead) {
      const FsErr m = mapStatus(st);
      return m == FsErr::Ok ? FsErr::ChipStatus : m;
    }
    size_t l = 0;
    e = readData(blk_, kCh376StageMax, l);
    if (e != FsErr::Ok) return e;
    if (b == block) {
      memcpy(out, blk_, l);
      len = l;
    }
    e = send(kCh376CmdDiskRdGo, nullptr, 0);
    if (e != FsErr::Ok) return e;
  }
  uint8_t st = 0;
  e = waitStatus(st, kCh376FsOpMs);  // кінець операції, очікуємо 0x14
  if (e != FsErr::Ok) return e;
  const FsErr m = mapStatus(st);
  return m;
}

FsErr Ch376Fs::fileSize(uint32_t& size) {
  size = 0;
  if (!open_) return FsErr::NotOpen;
  const uint8_t a = kCh376GetFileSizeArg;
  FsErr e = send(kCh376CmdGetFileSize, &a, 1);
  if (e != FsErr::Ok) return e;
  // Відповідь: 4 байти, перед якими може бути зайвий байт (копія статусу): читаємо до 5 з короткою паузою бездіяльності.
  uint8_t r[5];
  const size_t n = link_.readBytes(r, 5, 2 * link_.byteTimeUs() + 300u);
  if (n < 4) return FsErr::NoReply;
  const uint8_t* b = r + (n - 4);
  size = (uint32_t)b[0] | ((uint32_t)b[1] << 8) | ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24);
  return FsErr::Ok;
}

FsErr Ch376Fs::locate(uint32_t off) {
  if (!open_) return FsErr::NotOpen;
  const uint8_t a[4] = {(uint8_t)off, (uint8_t)(off >> 8), (uint8_t)(off >> 16), (uint8_t)(off >> 24)};
  FsErr e = send(kCh376CmdByteLocate, a, 4);
  if (e != FsErr::Ok) return failRead(e);
  uint8_t s = 0;
  e = waitStatus(s, kCh376FsOpMs);
  if (e != FsErr::Ok) return failRead(e);
  e = mapStatus(s);
  return e == FsErr::Ok ? e : failRead(e);
}

FsErr Ch376Fs::read(void* dst, size_t n, size_t& got) {
  got = 0;
  if (!open_) return FsErr::NotOpen;
  uint8_t* d = static_cast<uint8_t*>(dst);
  while (got < n) {
    size_t want = n - got;
    if (want > 0xFFFFu) want = 0xFFFFu;
    const uint8_t a[2] = {(uint8_t)want, (uint8_t)(want >> 8)};
    FsErr e = send(kCh376CmdByteRead, a, 2);
    if (e != FsErr::Ok) return failRead(e);
    uint32_t t0 = link_.nowUs();
    size_t chunk = 0;
    for (;;) {
      uint8_t s = 0;
      e = waitStatus(s, kCh376FsOpMs);
      if (e != FsErr::Ok) return failRead(e);
      if (s == kCh376IntSuccess) break;  // запит виконано (або файл закінчився раніше)
      if (s != kCh376IntDiskRead) return failRead(mapStatus(s));
      const uint32_t t1 = link_.nowUs();
      size_t len = 0;
      e = readData(d ? d + got : nullptr, want - chunk, len);  // блок читається просто в буфер викликача
      if (e != FsErr::Ok) return failRead(e);
      const uint32_t t2 = link_.nowUs();
      const size_t take = len;
      got += take;
      chunk += take;
      st_.bytes += (uint32_t)take;
      e = send(kCh376CmdByteRdGo, nullptr, 0);
      if (e != FsErr::Ok) return failRead(e);
      noteBlock(t0, t1, t2);
      t0 = link_.nowUs();
    }
    if (chunk < want) break;  // кінець файлу
  }
  return FsErr::Ok;
}

FsErr Ch376Fs::close() {
  if (!open_) return FsErr::Ok;
  open_ = false;
  const uint8_t a = kCh376FileCloseArg;
  FsErr e = send(kCh376CmdFileClose, &a, 1);
  if (e != FsErr::Ok) return e;
  uint8_t s = 0;
  e = waitStatus(s, kCh376FsOpMs);
  if (e != FsErr::Ok) return e;
  return mapStatus(s);
}

void Ch376Fs::abort() {
  const bool was = open_;
  open_ = false;
  for (int i = 0; i < 8 && link_.readBytes(blk_, sizeof blk_, 2000u) > 0; i++) {}  // дочекатись кінця потоку від чіпа
  link_.flushRx();
  if (was) {
    const uint8_t a = kCh376FileCloseArg;
    if (send(kCh376CmdFileClose, &a, 1) == FsErr::Ok) {
      uint8_t s = 0;
      waitStatus(s, kCh376FsAbortMs);  // результат не важливий
    }
  }
  link_.flushRx();
}

FsErr Ch376Fs::list(const char* dirPath, Ch376DirCb cb, void* ctx, uint16_t maxN, uint16_t& count, bool& more) {
  count = 0;
  more = false;
  if (!cb) return FsErr::BadArg;
  FsKind k = FsKind::None;
  FsErr e = openPath(dirPath, k);
  if (e != FsErr::Ok) return e;
  if (k != FsKind::Dir) {
    close();
    return FsErr::NotDir;
  }
  e = sendName("*", 1, false);  // перелік поточного (щойно відкритого) каталогу
  if (e != FsErr::Ok) return e;
  e = send(kCh376CmdFileOpen, nullptr, 0);
  if (e != FsErr::Ok) return e;
  for (;;) {
    uint8_t s = 0;
    e = waitStatus(s, kCh376FsOpMs);
    if (e != FsErr::Ok) return e;
    if (s == kCh376ErrMissFile) return FsErr::Ok;  // кінець переліку
    if (s != kCh376IntDiskRead) {
      const FsErr m = mapStatus(s);
      return m == FsErr::Ok ? FsErr::ChipStatus : m;
    }
    size_t len = 0;
    e = readData(blk_, kCh376DirInfoLen, len);
    if (e != FsErr::Ok) return e;
    if (len != kCh376DirInfoLen) return FsErr::BadReply;
    Ch376DirEntry de;
    if (parseEntry(blk_, de)) {
      if (count >= maxN) {
        more = true;
        return FsErr::Ok;
      }
      count++;
      if (!cb(de, ctx)) {
        more = true;  // перервано колбеком
        return FsErr::Ok;
      }
    }
    e = send(kCh376CmdFileEnumGo, nullptr, 0);
    if (e != FsErr::Ok) return e;
  }
}

#endif  // ENABLE_USB_CH376

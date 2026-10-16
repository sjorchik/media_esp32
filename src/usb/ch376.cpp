#include "config.h"

#if ENABLE_USB_CH376  // типова збірка ESP32 компілює src/ цілком: без прапорця код не потрапляє в бінарник

#include "usb/ch376.h"

// Протокол: кадр команди = 0x57 0xAB <код> [аргументи до 2 байт]. Відповідь — переважно 1 байт.
// Сміття перед відповіддю пропускається до kCh376JunkMax байтів. Без Arduino.h.

const char* ch376ErrName(Ch376Err e) {
  switch (e) {
    case Ch376Err::Ok: return "ok";
    case Ch376Err::NoReply: return "noreply";
    case Ch376Err::BadReply: return "badreply";
    case Ch376Err::Timeout: return "timeout";
    case Ch376Err::Aborted: return "aborted";
    case Ch376Err::Disconnected: return "disconn";
    case Ch376Err::NoDisk: return "nodisk";
    case Ch376Err::BadFs: return "badfs";
    case Ch376Err::BadArg: return "badarg";
    case Ch376Err::BaudFail: return "baudfail";
  }
  return "?";
}

Ch376Err Ch376::sendCmd(uint8_t cmd, const uint8_t* args, size_t na) {
  if (na > 2) return Ch376Err::BadArg;
  uint8_t f[5] = {kCh376Sync0, kCh376Sync1, cmd, 0, 0};
  for (size_t i = 0; i < na; i++) f[3 + i] = args[i];
  link_.flushRx();  // відповіді на попередню команду вже не потрібні
  link_.write(f, 3 + na);
  return Ch376Err::Ok;
}

// Чекає одного з байтів ok[0..nok). Сміття пропускає; ліміт kCh376JunkMax.
// Нічого не прийшло → NoReply; прийшло лише сміття → BadReply.
Ch376Err Ch376::readReply(const uint8_t* ok, size_t nok, uint32_t tmoMs, uint8_t& out) {
  size_t junk = 0;
  bool any = false;
  for (;;) {
    if (link_.abortRequested()) return Ch376Err::Aborted;
    uint8_t b = 0;
    if (!link_.readByte(b, tmoMs)) return any ? Ch376Err::BadReply : Ch376Err::NoReply;
    any = true;
    for (size_t i = 0; i < nok; i++) {
      if (b == ok[i]) {
        out = b;
        return Ch376Err::Ok;
      }
    }
    if (++junk >= kCh376JunkMax) return Ch376Err::BadReply;
  }
}

Ch376Err Ch376::reset() {
  sendCmd(kCh376CmdReset, nullptr, 0);  // RESET_ALL іде на поточній швидкості, відповіді нема
  link_.delayMs(kCh376ResetWaitMs);
  if (!link_.setBaud(kCh376DefaultBaud)) return Ch376Err::BaudFail;
  link_.flushRx();
  baud_ = kCh376DefaultBaud;
  return Ch376Err::Ok;
}

Ch376Err Ch376::checkExist() {
  const uint8_t x = kCh376CheckByte;
  Ch376Err e = sendCmd(kCh376CmdCheckExist, &x, 1);
  if (e != Ch376Err::Ok) return e;
  const uint8_t want = (uint8_t)(x ^ kCh376CheckXor);  // інверсія байта
  uint8_t r = 0;
  return readReply(&want, 1, kCh376ReplyMs, r);
}

Ch376Err Ch376::setUsbMode(uint8_t mode) {
  Ch376Err e = sendCmd(kCh376CmdSetUsbMode, &mode, 1);
  if (e != Ch376Err::Ok) return e;
  const uint8_t ok[2] = {kCh376RetSuccess, kCh376RetAbort};
  uint8_t r = 0;
  e = readReply(ok, 2, kCh376ReplyMs, r);
  if (e != Ch376Err::Ok) return e;
  if (r != kCh376RetSuccess) return Ch376Err::BadReply;
  link_.delayMs(kCh376ModeWaitMs);
  return Ch376Err::Ok;
}

Ch376Err Ch376::getStatus(uint8_t& st) {
  Ch376Err e = sendCmd(kCh376CmdGetStatus, nullptr, 0);
  if (e != Ch376Err::Ok) return e;
  uint8_t b = 0;
  if (!link_.readByte(b, kCh376ReplyMs)) return Ch376Err::NoReply;
  st = b;
  lastEv_ = b;
  return Ch376Err::Ok;
}

// Опитування статусу (або INT, якщо увімкнено). Відсутність події до тайм-ауту → Timeout.
// NoReply від GET_STATUS вважаємо «подій ще немає» і опитуємо далі.
Ch376Err Ch376::waitEvent(uint8_t& ev, uint32_t tmoMs) {
  const uint32_t t0 = link_.nowUs();
  const uint32_t limitUs = tmoMs * 1000u;
  for (;;) {
    if (link_.abortRequested()) return Ch376Err::Aborted;
    const bool lineWait = kCh376IntPin && link_.hasIntPin();
    if (!lineWait || link_.intLow()) {
      uint8_t st = kCh376IntNone;
      Ch376Err e = getStatus(st);
      if (e == Ch376Err::Ok && st != kCh376IntNone) {
        ev = st;
        return Ch376Err::Ok;
      }
    }
    if ((uint32_t)(link_.nowUs() - t0) >= limitUs) return Ch376Err::Timeout;
    link_.delayMs(kCh376PollMs);
  }
}

Ch376Err Ch376::waitConnect(uint32_t timeoutMs) {
  uint8_t ev = 0;
  Ch376Err e = waitEvent(ev, timeoutMs);
  if (e != Ch376Err::Ok) return e;
  if (ev == kCh376IntConnect) return Ch376Err::Ok;
  if (ev == kCh376IntDisconnect) return Ch376Err::Disconnected;
  return Ch376Err::BadReply;
}

Ch376Err Ch376::diskConnect() {
  Ch376Err e = sendCmd(kCh376CmdDiskConnect, nullptr, 0);
  if (e != Ch376Err::Ok) return e;
  uint8_t ev = 0;
  e = waitEvent(ev, kCh376ConnectWaitMs);
  if (e != Ch376Err::Ok) return e;
  if (ev == kCh376IntSuccess) return Ch376Err::Ok;
  if (ev == kCh376IntDisconnect) return Ch376Err::Disconnected;
  return Ch376Err::NoDisk;
}

Ch376Err Ch376::diskMount() {
  Ch376Err e = sendCmd(kCh376CmdDiskMount, nullptr, 0);
  if (e != Ch376Err::Ok) return e;
  uint8_t ev = 0;
  e = waitEvent(ev, kCh376MountWaitMs);
  if (e != Ch376Err::Ok) return e;
  if (ev == kCh376IntSuccess) return Ch376Err::Ok;
  if (ev == kCh376IntDisconnect) return Ch376Err::Disconnected;
  if (ev == kCh376ErrDiskDisc) return Ch376Err::NoDisk;
  return Ch376Err::BadFs;
}

// SET_BAUDRATE: команда і відповідь іще на старій швидкості; потім перемикаємо порт.
Ch376Err Ch376::setChipBaud(uint8_t lo, uint8_t hi) {
  const uint8_t a[2] = {lo, hi};
  Ch376Err e = sendCmd(kCh376CmdSetBaud, a, 2);
  if (e != Ch376Err::Ok) return e;
  const uint8_t ok = kCh376RetSuccess;
  uint8_t r = 0;
  return readReply(&ok, 1, kCh376ReplyMs, r);
}

// Відкат після невдалої спроби на швидкості toBaud. Кожен наступний крок застосовується,
// лише якщо попередній не повернув звʼязок. Після відкату порт завжди на kCh376DefaultBaud.
//  а) чіп не перемкнувся: порт назад на 9600, перевірка;
//  б) чіп перемкнувся: порт на його швидкість, програмний скид (він повертає 9600), перевірка;
//  в) є RST: апаратний скид, порт на 9600, перевірка;
//  г) нічого не допомогло: порт на 9600, стан невідомий (чіп може не відповідати).
void Ch376::rollbackTo(uint32_t toBaud) {
  link_.setBaud(kCh376DefaultBaud);
  if (checkExist() == Ch376Err::Ok) {
    baud_ = kCh376DefaultBaud;
    return;
  }
  link_.setBaud(toBaud);
  if (reset() == Ch376Err::Ok && checkExist() == Ch376Err::Ok) return;  // reset() сам ставить 9600
  if (link_.hwReset()) {
    link_.delayMs(kCh376ResetWaitMs);
    link_.setBaud(kCh376DefaultBaud);
    link_.flushRx();
    baud_ = kCh376DefaultBaud;
    if (checkExist() == Ch376Err::Ok) return;
  }
  link_.setBaud(kCh376DefaultBaud);
  link_.flushRx();
  baud_ = kCh376DefaultBaud;
}

Ch376Err Ch376::negotiateBaud(const Ch376BaudDiv* table, size_t n, Ch376Attempt* att, size_t maxAtt, size_t& nAtt) {
  nAtt = 0;
  if (!table || n == 0) return Ch376Err::BadArg;
  for (size_t i = 0; i < n; i++) {
    const Ch376BaudDiv& t = table[i];
    Ch376Err r = setChipBaud(t.lo, t.hi);
    if (r == Ch376Err::Ok && !link_.setBaud(t.baud)) r = Ch376Err::BaudFail;
    if (r == Ch376Err::Ok) r = checkExist();
    if (att && nAtt < maxAtt) att[nAtt] = Ch376Attempt{t.baud, r};
    nAtt++;
    if (r == Ch376Err::Ok) {
      baud_ = t.baud;
      return Ch376Err::Ok;
    }
    rollbackTo(t.baud);
  }
  return Ch376Err::BaudFail;
}

#endif  // ENABLE_USB_CH376

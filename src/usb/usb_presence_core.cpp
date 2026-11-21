#include "usb/usb_presence_core.h"

#if ENABLE_USB_CH376

const char* usbPresName(UsbPres p) {
  switch (p) {
    case UsbPres::Unknown: return "unknown";
    case UsbPres::Absent: return "absent";
    case UsbPres::Present: return "present";
    case UsbPres::Unsupported: return "unsupported";
  }
  return "?";
}

void UsbPresenceCore::begin(uint32_t now) {
  if (mode_ == 0) return;
  needInit_ = true;
  gaveUp_ = false;
  initTries_ = 0;
  initAt_ = now + kUsbPresInitDelayMs;
}

void UsbPresenceCore::restartInit(uint32_t now) {
  if (mode_ == 0) return;
  toIdle();
  needInit_ = true;
  gaveUp_ = false;
  initTries_ = 0;
  reinitMs_ = kUsbPresReinitMs;
  initAt_ = now;
}

void UsbPresenceCore::invalidate(uint32_t now) {
  if (mode_ == 0) return;
  if (ph_ != Ph::Idle) ph_ = Ph::Idle;
  ownPort_ = false;
  needInit_ = true;
  gaveUp_ = false;
  initTries_ = 0;
  if (st_ != UsbPres::Unknown) setState(UsbPres::Unknown);
  initAt_ = now + kUsbPresSettleMs;
}

void UsbPresenceCore::setState(UsbPres s) {
  st_ = s;
  if (s != UsbPres::Unknown) {
    recheck_ = false;
    force_ = false;
  }
}

bool UsbPresenceCore::takeChange(bool& flag) {
  const bool now = st_ == UsbPres::Present;
  if (now == published_) return false;
  published_ = now;
  flag = now;
  return true;
}

void UsbPresenceCore::openFailed(UsbOpenFail f) {
  switch (f) {
    case UsbOpenFail::NoMedia: setState(UsbPres::Absent); break;
    case UsbOpenFail::UnsupportedFs:
    case UsbOpenFail::NoPartition:
    case UsbOpenFail::MountFailed: setState(UsbPres::Unsupported); break;
    default: break;  // збій чіпа/памʼяті/читання: що з флешкою — невідомо, стан не чіпаємо
  }
}

void UsbPresenceCore::sessionClosed(bool recheck) {
  ownPort_ = false;
  if (c_.chipHost()) {  // сесія привела чіп у host і запаркувала його: окрема ініціалізація не потрібна
    needInit_ = false;
    gaveUp_ = false;
    initTries_ = 0;
  }
  if (recheck) {
    recheck_ = true;
    force_ = true;
    if (st_ == UsbPres::Present) st_ = UsbPres::Unknown;  // флешка могла зникнути: до перевірки не стверджуємо
  }
}

bool UsbPresenceCore::pending() {
  // Лише рівень: INT# LOW тримається до GET_STATUS. Лічильник фронтів не годиться — фронти породжує й наш власний обмін із чіпом.
  return mode_ == 1 && c_.intLow();
}

UsbPres UsbPresenceCore::stateForOpen() {
  if (mode_ == 0) return UsbPres::Unknown;
  if (ph_ != Ph::Idle || needInit_ || recheck_ || pending()) return UsbPres::Unknown;
  return st_;
}

void UsbPresenceCore::toIdle() {
  if (ownPort_ && c_.portOpen()) c_.portPark();
  ownPort_ = false;
  ph_ = Ph::Idle;
}

void UsbPresenceCore::initFail(uint32_t now, const char* what, Ch376Err e) {
  toIdle();
  failWhat_ = what;
  failErr_ = e;
  failRetryMs_ = reinitMs_;
  stat_.initFails++;
  initTries_++;
  needInit_ = true;
  initAt_ = now + reinitMs_;
  if (reinitMs_ < kUsbPresReinitMaxMs) reinitMs_ *= 2;
  if (initTries_ >= kUsbPresInitTries) {
    needInit_ = false;
    gaveUp_ = true;
  }  // чіпа, імовірно, немає: далі лише DBG,usb,pres,init
}

void UsbPresenceCore::retry(uint32_t now) {
  stat_.retries++;
  toIdle();
  if (retry_ < kUsbPresRetryMax) {
    retry_++;
    force_ = true;
    recheck_ = true;
    t_ = now + kUsbPresRetryMs;
    ph_ = Ph::Idle;
    return;
  }
  retry_ = 0;
  recheck_ = false;
  force_ = false;
  needInit_ = true;  // чіп мовчить: повторна ініціалізація з паузою
  initAt_ = now + reinitMs_;
}

void UsbPresenceCore::verdict(UsbPres s) {
  setState(s);
  retry_ = 0;
  stat_.lastMs = t0_;
  toIdle();
}

bool UsbPresenceCore::poll(uint32_t now, uint32_t tmoMs, uint8_t& ev, bool& timedOut) {
  timedOut = false;
  if ((int32_t)(now - next_) < 0) return false;
  next_ = now + kCh376PollMs;
  if (!c_.hasInt() || c_.intLow()) {  // GET_STATUS без готової події чекає тайм-аут відповіді (≈50 мс): питаємо, лише коли INT# уже LOW
    uint8_t st = kCh376IntNone;
    const Ch376Err e = c_.status(st);
    if (e == Ch376Err::Ok && st != kCh376IntNone) {
      ev = st;
      return true;
    }
  }
  if ((uint32_t)(now - t0_) >= tmoMs) timedOut = true;
  return false;
}

void UsbPresenceCore::tick(const UsbPresEnv& env) {
  if (mode_ == 0) return;
  const uint32_t now = env.now;
  uint8_t ev = 0;
  bool tmo = false;

  if (ph_ == Ph::Idle) {
    if (env.portBusy) return;  // порт чужий (сесія USB, консоль)
    const bool wantInit = needInit_ && (int32_t)(now - initAt_) >= 0;
    const bool wantEvent = !needInit_ && (recheck_ ? (int32_t)(now - t_) >= 0 : (mode_ == 1 ? pending() : (int32_t)(now - pollAt_) >= 0));
    if (!wantInit && !wantEvent) {
      if (!needInit_ && !gaveUp_ && !c_.chipHost() && !c_.portOpen()) {  // чіп скинуто поза нами (DBG,usb,stop): ініціалізувати
        needInit_ = true;
        initAt_ = now + kUsbPresSettleMs;
      }
      return;
    }
    if (env.defer) {  // BT з телефоном: heap не чіпаємо; подія лишається (INT# LOW), оброблю після виходу з BT
      if (stat_.defers < 0xFFFF) stat_.defers++;
      return;
    }
    startedAt_ = now;
    t0_ = now;
    if (wantInit) {
      stat_.inits++;
      ph_ = Ph::InitWake;
    } else {
      stat_.events++;
      if (mode_ == 2) pollAt_ = now + kUsbPresPollMs;
      t_ = now + (mode_ == 1 && !recheck_ ? kUsbPresSettleMs : 0);
      ph_ = Ph::Settle;
    }
    return;
  }

  // Послідовність уже йде: порт належить нам. Якщо BT став активним і heap малий — віддати памʼять, подію не губити.
  if (env.defer && ph_ != Ph::Settle) {
    toIdle();
    recheck_ = true;
    force_ = true;
    t_ = now;
    if (stat_.defers < 0xFFFF) stat_.defers++;
    return;
  }

  switch (ph_) {
    case Ph::Idle:
      return;

    // ---------------- ініціалізація (раз після старту або після втрати чіпа)
    case Ph::InitWake: {
      ownPort_ = true;
      if (!c_.portWake()) { initFail(now, "portWake", Ch376Err::Ok); return; }
      Ch376Err e = c_.ping();
      if (e != Ch376Err::Ok) {  // чіп міг лишитись на швидкості попереднього сеансу (перезавантаження MCU): відкат
        c_.recover();
        e = c_.ping();
      }
      if (e != Ch376Err::Ok) { initFail(now, "ping після відкату", e); return; }
      ph_ = Ph::InitReset;
      return;
    }
    case Ph::InitReset:
      c_.resetSend();
      t_ = now;
      ph_ = Ph::InitResetWait;
      return;
    case Ph::InitResetWait:
      if ((uint32_t)(now - t_) < kCh376ResetWaitMs) return;
      {
        Ch376Err e = c_.resetFinish();
        const char* w = "resetFinish";
        if (e == Ch376Err::Ok) { e = c_.ping(); w = "ping після скиду"; }
        if (e != Ch376Err::Ok) { initFail(now, w, e); return; }
      }
      ph_ = Ph::InitBaud;
      return;
    case Ph::InitBaud:  // 2 Мбод; резерв 1 Мбод; інакше лишаємось на 9600 (перевірка працює і так)
      if (c_.setBaud(kUsbBaudFast) != Ch376Err::Ok) c_.setBaud(kUsbBaudRef);
      ph_ = Ph::InitHost;
      return;
    case Ph::InitHost:
      {
        const Ch376Err e = c_.setHost();
        if (e != Ch376Err::Ok) { initFail(now, "setHost", e); return; }
      }
      t_ = now;
      next_ = now;
      ph_ = Ph::InitSettle;
      return;
    case Ph::InitSettle: {  // флешка могла стояти вже: чекаємо на CONNECT, опитуючи GET_STATUS
      if ((int32_t)(now - next_) < 0) return;
      next_ = now + kUsbPresInitPollMs;
      uint8_t st = kCh376IntNone;
      Ch376Err e = Ch376Err::NoReply;
      if (!c_.hasInt() || c_.intLow()) e = c_.status(st);
      if (e == Ch376Err::Ok && st != kCh376IntNone) {
        needInit_ = false;
        initTries_ = 0;
        reinitMs_ = kUsbPresReinitMs;
        force_ = true;  // далі — повна перевірка (DISK_CONNECT → MOUNT → probe)
        connTries_ = 0;
        ph_ = Ph::DiskConn;
        return;
      }
      if ((uint32_t)(now - t_) >= kUsbPresInitSettleMs) {
        needInit_ = false;
        initTries_ = 0;
        reinitMs_ = kUsbPresReinitMs;
        verdict(UsbPres::Absent);
        if (mode_ == 2) pollAt_ = now + kUsbPresPollMs;
      }
      return;
    }

    // ---------------- перевірка після події
    case Ph::Settle:
      if ((int32_t)(now - t_) < 0) return;
      ph_ = Ph::Wake;
      return;
    case Ph::Wake:
      ownPort_ = true;
      if (!c_.portWake() || c_.ping() != Ch376Err::Ok) { retry(now); return; }
      ph_ = Ph::Status;
      return;
    case Ph::Status: {
      uint8_t st = kCh376IntNone;
      if (c_.status(st) != Ch376Err::Ok) { retry(now); return; }
      if (st == kCh376IntNone && !force_) {  // подій немає (режим 2: опитування) — стан без змін
        toIdle();
        return;
      }
      if (st == kCh376IntDisconnect && !force_) {
        verdict(UsbPres::Absent);
        return;
      }
      connTries_ = 0;
      force_ = true;
      ph_ = Ph::DiskConn;
      return;
    }
    case Ph::DiskConn:
      if (c_.diskConnectSend() != Ch376Err::Ok) { retry(now); return; }
      t0_ = now;
      next_ = now;
      ph_ = Ph::DiskConnWait;
      return;
    case Ph::DiskConnWait:
      if (!poll(now, kUsbPresConnectMs, ev, tmo)) {
        if (tmo) verdict(UsbPres::Absent);
        return;
      }
      if (ev == kCh376IntSuccess) { ph_ = Ph::Mount; return; }
      if (ev == kCh376IntConnect && connTries_ < 2) {  // повторне підключення під час очікування
        connTries_++;
        ph_ = Ph::DiskConn;
        return;
      }
      verdict(UsbPres::Absent);  // відключення, «диска немає» й решта подій
      return;
    case Ph::Mount:
      if (c_.diskMountSend() != Ch376Err::Ok) { ph_ = Ph::Probe; return; }
      t0_ = now;
      next_ = now;
      ph_ = Ph::MountWait;
      return;
    case Ph::MountWait:
      if (!poll(now, kCh376MountWaitMs, ev, tmo)) {
        if (tmo) ph_ = Ph::Probe;  // як у open(): тайм-аут монтування чіпом ігноруємо
        return;
      }
      if (ev == kCh376IntDisconnect || ev == kCh376ErrDiskDisc) { verdict(UsbPres::Absent); return; }
      ph_ = Ph::Probe;
      return;
    case Ph::Probe: {
      stat_.probes++;
      const UsbOpenFail f = c_.probe();
      switch (f) {
        case UsbOpenFail::None: verdict(UsbPres::Present); return;
        case UsbOpenFail::NoMedia: verdict(UsbPres::Absent); return;
        case UsbOpenFail::UnsupportedFs:
        case UsbOpenFail::NoPartition: verdict(UsbPres::Unsupported); return;
        default:  // збій читання: повторити; після повторів флешка нечитана → Unsupported
          if (retry_ < kUsbPresRetryMax) { retry(now); return; }
          verdict(UsbPres::Unsupported);
          return;
      }
    }
  }
}

#endif  // ENABLE_USB_CH376

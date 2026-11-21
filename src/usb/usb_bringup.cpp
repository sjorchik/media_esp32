#include "usb/usb_bringup.h"

#if ENABLE_USB_CH376

void UsbBringUp::begin(uint32_t baud) {
  baud_ = baud;
  err_ = Ch376Err::Ok;
  retried_ = false;
  t0_ = next_ = 0;
  ph_ = Ph::ResetSend;
}

void UsbBringUp::abort() {
  if (active()) ph_ = Ph::Idle;
}

bool UsbBringUp::fail(Ch376Err e) {
  err_ = e;
  ph_ = Ph::Done;
  return true;
}

bool UsbBringUp::finish() {
  err_ = Ch376Err::Ok;
  ph_ = Ph::Done;
  return true;
}

bool UsbBringUp::poll(uint32_t now, uint32_t tmoMs, uint8_t& ev, bool& timedOut) {
  timedOut = false;
  if ((int32_t)(now - next_) < 0) return false;
  next_ = now + kCh376PollMs;
  if (!c_.hasInt() || c_.intLow()) {  // без готової події GET_STATUS чекає тайм-аут відповіді ≈50 мс: питаємо, лише коли INT# уже LOW
    uint8_t st = kCh376IntNone;
    const Ch376Err e = c_.status(st);
    if (e == Ch376Err::Ok && st != kCh376IntNone) {
      ev = st;
      return true;
    }
  }
  if ((uint32_t)(now - t0_) >= tmoMs) timedOut = true;  // як waitEvent: тайм-аут перевіряється після невдалого опиту
  return false;
}

bool UsbBringUp::step(uint32_t now) {
  uint8_t ev = 0;
  bool tmo = false;
  switch (ph_) {
    case Ph::Idle:
    case Ph::Done:
      return ph_ == Ph::Done;

    case Ph::ResetSend:
      c_.resetSend();  // результат ігнорується, як у Ch376::reset()
      t0_ = now;
      ph_ = Ph::ResetWait;
      return false;

    case Ph::ResetWait: {
      if ((uint32_t)(now - t0_) < kCh376ResetWaitMs) return false;
      const Ch376Err e = c_.resetFinish();
      if (e != Ch376Err::Ok) return fail(e);
      ph_ = Ph::Check;
      return false;
    }

    case Ph::Check: {
      const Ch376Err e = c_.ping();
      if (e == Ch376Err::Ok) {
        ph_ = Ph::Baud;
        return false;
      }
      if (retried_) return fail(e);  // чіп лишився на швидкості попереднього сеансу: відкат і одна повторна спроба
      retried_ = true;
      c_.recover();  // результат ігнорується (як rollback у bringUp): далі повторний скид і CHECK_EXIST
      ph_ = Ph::ResetSend;
      return false;
    }

    case Ph::Baud: {
      const Ch376Err e = c_.setBaud(baud_);
      if (e != Ch376Err::Ok) return fail(e);
      ph_ = Ph::Host;
      return false;
    }

    case Ph::Host: {
      const Ch376Err e = c_.setHost();
      if (e != Ch376Err::Ok) return fail(e);
      t0_ = now;
      next_ = now;
      ph_ = Ph::Connect;
      return false;
    }

    case Ph::Connect:  // як waitConnect: подія підключення; відключення — Disconnected; інша подія — BadReply
      if (!poll(now, kCh376ConnectWaitMs, ev, tmo)) return tmo ? fail(Ch376Err::Timeout) : false;
      if (ev == kCh376IntConnect) {
        ph_ = Ph::DiskConn;
        return false;
      }
      return fail(ev == kCh376IntDisconnect ? Ch376Err::Disconnected : Ch376Err::BadReply);

    case Ph::DiskConn: {
      const Ch376Err e = c_.diskConnectSend();
      if (e != Ch376Err::Ok) return fail(e);
      t0_ = now;
      next_ = now;
      ph_ = Ph::DiskConnWait;
      return false;
    }

    case Ph::DiskConnWait:  // як diskConnect: успіх → Ok; відключення → Disconnected; інша подія → NoDisk
      if (!poll(now, kCh376ConnectWaitMs, ev, tmo)) return tmo ? fail(Ch376Err::Timeout) : false;
      if (ev == kCh376IntSuccess) {
        ph_ = Ph::Mount;
        return false;
      }
      return fail(ev == kCh376IntDisconnect ? Ch376Err::Disconnected : Ch376Err::NoDisk);

    case Ph::Mount:
      if (c_.diskMountSend() != Ch376Err::Ok) return finish();  // як у bringUp: збій монтування чіпом ігнорується
      t0_ = now;
      next_ = now;
      ph_ = Ph::MountWait;
      return false;

    case Ph::MountWait:  // лише відключення й «диска немає» зривають відкриття; BadFs і тайм-аут (GPT 30 ГБ) ігноруються
      if (!poll(now, kCh376MountWaitMs, ev, tmo)) return tmo ? finish() : false;
      if (ev == kCh376IntDisconnect) return fail(Ch376Err::Disconnected);
      if (ev == kCh376ErrDiskDisc) return fail(Ch376Err::NoDisk);
      return finish();
  }
  return false;
}

#endif  // ENABLE_USB_CH376

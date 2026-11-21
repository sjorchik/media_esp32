#pragma once
#include <stddef.h>
#include <stdint.h>

#include "config.h"

#if ENABLE_USB_CH376

#include "usb/usb_chip.h"

// M4c2. Неблокуючий аналог bringUp(baud) з usb_volume.cpp: RESET_ALL → CHECK_EXIST → [відкат] → швидкість → host → очікування
// підключення → DISK_CONNECT → DISK_MOUNT. Той самий порядок і ті самі результати (Ch376Err), але очікування — таймерами між
// кроками step(now). Один крок — одна коротка дія. Чистий C++ (без Arduino), тестується в native.
// Виняток: відкат recover() (лише після збою CHECK_EXIST) і setBaud() блокують до ≈0,4 с / ≈50 мс — як і блокуюча версія.
class UsbBringUp {
 public:
  explicit UsbBringUp(IUsbChip& c) : c_(c) {}

  void begin(uint32_t baud);     // baud == 0 — список швидкостей (діагностика); порт має бути відкрито
  bool step(uint32_t nowMs);     // true — завершено, результат у err(); після завершення step() нічого не робить
  void abort();                  // зупинити; чіп лишається в тому стані, де був (стан — chipBaud()/chipHost())
  bool active() const { return ph_ != Ph::Idle && ph_ != Ph::Done; }
  Ch376Err err() const { return err_; }
  uint32_t baud() const { return baud_; }

 private:
  enum class Ph : uint8_t { Idle, ResetSend, ResetWait, Check, Baud, Host, Connect, DiskConn, DiskConnWait, Mount, MountWait, Done };
  bool fail(Ch376Err e);
  bool finish();
  // Один опит GET_STATUS не частіше kCh376PollMs; true — подія в ev; timedOut — вичерпано tmoMs від t0_.
  bool poll(uint32_t now, uint32_t tmoMs, uint8_t& ev, bool& timedOut);

  IUsbChip& c_;
  Ph ph_ = Ph::Idle;
  Ch376Err err_ = Ch376Err::Ok;
  uint32_t baud_ = 0;
  uint32_t t0_ = 0, next_ = 0;
  bool retried_ = false;
};

#endif  // ENABLE_USB_CH376

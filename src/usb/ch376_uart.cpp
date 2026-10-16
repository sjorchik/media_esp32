#include "usb/ch376_uart.h"

#if ENABLE_USB_CH376

#include <Arduino.h>

#include "pins.h"

namespace {

// Реалізація Ch376Link на Serial2. Тайм-аути — за micros(); очікування байта — опитування з кроком 1 мс.
class UartLink : public Ch376Link {
 public:
  void write(const uint8_t* d, size_t n) override { Serial2.write(d, n); }

  bool readByte(uint8_t& b, uint32_t tmoMs) override {
    const uint32_t t0 = micros();
    for (;;) {
      if (Serial2.available() > 0) {
        int c = Serial2.read();
        if (c >= 0) {
          b = (uint8_t)c;
          return true;
        }
      }
      if ((uint32_t)(micros() - t0) >= tmoMs * 1000u) return false;
      delay(1);
    }
  }

  void flushRx() override {
    while (Serial2.available() > 0) Serial2.read();
  }

  // Лише порт; чіп перемикає SET_BAUDRATE окремо. Успіх перевіряє checkExist() у протоколі.
  bool setBaud(uint32_t baud) override {
    Serial2.updateBaudRate(baud);
    return true;
  }

  bool hasIntPin() const override { return CH_INT_PIN >= 0; }
  bool intLow() const override { return digitalRead(CH_INT_PIN) == LOW; }

  // Імпульс RST: активний рівень — kCh376RstActiveHigh (низька впевненість, звірити з даташитом).
  bool hwReset() override {
    if (CH_RST_PIN < 0) return false;
    digitalWrite(CH_RST_PIN, kCh376RstActiveHigh ? HIGH : LOW);
    delay(kCh376RstPulseMs);
    digitalWrite(CH_RST_PIN, kCh376RstActiveHigh ? LOW : HIGH);
    return true;
  }

  bool abortRequested() const override { return false; }  // перервання з консолі не реалізовано
  void delayMs(uint32_t ms) override { delay(ms); }
  uint32_t nowUs() const override { return (uint32_t)micros(); }
};

UartLink s_link;
Ch376 s_drv(s_link);
bool s_started = false;

}  // namespace

namespace ch376_uart {

bool begin() {
  if (s_started) return true;
  // Розмір RX-буфера задається до begin(); пам'ять виділяється тут, а не при старті прошивки.
  Serial2.setRxBufferSize(kCh376RxBuf);
  Serial2.begin(kCh376DefaultBaud, SERIAL_8N1, PIN_CH_RX, PIN_CH_TX);
  if (CH_INT_PIN >= 0) pinMode(CH_INT_PIN, INPUT_PULLUP);
  if (CH_RST_PIN >= 0) {
    pinMode(CH_RST_PIN, OUTPUT);
    digitalWrite(CH_RST_PIN, kCh376RstActiveHigh ? LOW : HIGH);  // у неактивному рівні
  }
  s_started = true;
  return true;
}

bool started() { return s_started; }

Ch376& driver() { return s_drv; }

}  // namespace ch376_uart

#endif  // ENABLE_USB_CH376

#pragma once
#include <stddef.h>
#include <stdint.h>

// Абстракція звʼязку з CH376S. Протокол (ch376.cpp) працює лише через цей інтерфейс:
// на залізі його реалізує ch376_uart.cpp (Serial2), у тестах — mock-емулятор чіпа.
// Без залежностей від Arduino, збирається в native.
class Ch376Link {
 public:
  virtual ~Ch376Link() {}

  // Запис байтів у порт.
  virtual void write(const uint8_t* d, size_t n) = 0;

  // Один байт з тайм-аутом (мс). false — байта не було до тайм-ауту.
  virtual bool readByte(uint8_t& b, uint32_t timeoutMs) = 0;

  // Скинути прийнятий, але не прочитаний буфер.
  virtual void flushRx() = 0;

  // Змінити швидкість порту (лише сам порт, чіп не чіпається). false — порт не прийняв швидкість.
  virtual bool setBaud(uint32_t baud) = 0;

  // Лінія INT (активна LOW). hasIntPin() == false, якщо пін не підключено (CH_INT_PIN < 0).
  virtual bool hasIntPin() const = 0;
  virtual bool intLow() const = 0;

  // Апаратний скид RST (імпульс LOW). false, якщо пін не підключено (CH_RST_PIN < 0).
  virtual bool hwReset() = 0;

  // true, якщо викликаючий попросив перервати операцію.
  virtual bool abortRequested() const = 0;

  virtual void delayMs(uint32_t ms) = 0;

  // Монотонний час, мкс (для тайм-аутів і вимірів у DBG,usb).
  virtual uint32_t nowUs() const = 0;
};

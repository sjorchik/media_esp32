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

  // ---- M4b1: гарячий шлях файлового шару. Не чисті, щоб існуючі реалізації й mock-и збиралися без змін. ----

  // Блокуюче очікування, поки INT# стане LOW (подія чіпа), не довше timeoutUs. true — INT низький.
  // Типово: одна перевірка intLow() без очікування. Справжня реалізація: GPIO-переривання → семафор.
  virtual bool waitIntLow(uint32_t /*timeoutUs*/) { return intLow(); }

  // Тривалість одного байта на лінії, мкс (10 біт / бод). Типово 0 (mock без часу на лінії).
  virtual uint32_t byteTimeUs() const { return 0; }

  // Віддати процесор в циклі опитування (без затримки на 1 тік). Типово нічого.
  virtual void yieldNow() {}

  // Прийом до n байтів. timeoutUs — тайм-аут БЕЗДІЯЛЬНОСТІ (скільки чекати наступного байта). Повертає прийняте.
  // Типово: readByte по одному (тайм-аут округлюється вгору до мс).
  virtual size_t readBytes(uint8_t* dst, size_t n, uint32_t timeoutUs) {
    size_t k = 0;
    const uint32_t ms = (timeoutUs + 999u) / 1000u;
    while (k < n && readByte(dst[k], ms)) k++;
    return k;
  }
};


#pragma once
#include <stddef.h>
#include <stdint.h>

#include "config.h"
#include "usb/ch376_link.h"

// Результат кроку драйвера. Ok — успіх; решта — конкретна причина (для DBG,usb).
enum class Ch376Err : uint8_t {
  Ok,
  NoReply,       // байтів не було до тайм-ауту
  BadReply,      // байти були, але жоден не збігся з очікуваним
  Timeout,       // подія не настала за тайм-аут
  Aborted,       // link повідомив про перервання
  Disconnected,  // подія відключення пристрою
  NoDisk,        // диска немає (ERR_DISK_DISCON або DISK_CONNECT без успіху)
  BadFs,         // диск є, але mount не вдався
  BadArg,        // некоректний аргумент
  BaudFail,      // перемикання швидкості або відкат не вдались
};

// Коротка назва коду для логів (ASCII).
const char* ch376ErrName(Ch376Err e);

// Спроба перемикання швидкості. rx — перший байт відповіді на SET_BAUDRATE (-1, якщо відповіді не було).
struct Ch376Attempt {
  uint32_t baud;
  Ch376Err res;
  int rx;
};

class Ch376 {
 public:
  explicit Ch376(Ch376Link& link) : link_(link) {}

  // RESET_ALL, пауза, порт на 9600, очистка RX.
  Ch376Err reset();

  // CHECK_EXIST: чіп має повернути x XOR kCh376CheckXor.
  Ch376Err checkExist();

  // SET_USB_MODE (kCh376ModeHost = 0x06 з авто-SOF).
  Ch376Err setUsbMode(uint8_t mode);

  // GET_STATUS: один байт стану переривання (kCh376IntNone, якщо події немає).
  Ch376Err getStatus(uint8_t& st);

  // Очікування події (INT, якщо kCh376IntPin, інакше опитування). Тайм-аут → Timeout.
  Ch376Err waitEvent(uint8_t& ev, uint32_t timeoutMs);

  // Очікування підключення. Відключення → Disconnected.
  Ch376Err waitConnect(uint32_t timeoutMs);

  // DISK_CONNECT: успіх → Ok; немає диска → NoDisk.
  Ch376Err diskConnect();

  // DISK_MOUNT: успіх → Ok; kCh376ErrDiskDisc → NoDisk; інші → BadFs.
  Ch376Err diskMount();

  // Перебір швидкостей з таблиці (від більшої). Після кожної невдалої спроби — програмний відкат.
  // Якщо чіп втрачено (не знайдено ні на одній швидкості), перебір зупиняється: BaudFail, chipLost() == true.
  // Викликати одразу після reset(). att — опційні записи спроб (до maxAtt), nAtt — загальна кількість.
  Ch376Err negotiateBaud(const Ch376BaudDiv* table, size_t n, Ch376Attempt* att, size_t maxAtt, size_t& nAtt);

  // Програмний відкат після спроби на швидкості tried: 9600, потім tried, потім відомі швидкості.
  // Знайдений на іншій швидкості чіп скидається на ній і повертається на 9600. Скидає прапорці lost/recovered.
  Ch376Err rollback(uint32_t tried);

  // Порт на швидкість baud без зміни чіпа.
  Ch376Err portTo(uint32_t baud);

  // Сирий обмін для діагностики: кадр 57 AB cmd args, усі байти за windowMs. Повертає кількість байтів;
  // у out — не більше cap.
  size_t rawExchange(uint8_t cmd, const uint8_t* args, size_t na, uint32_t windowMs, uint8_t* out, size_t cap);

  // Варіант B: надіслати кадр, одразу перемкнути порт на newBaud, слухати windowMs.
  size_t rawSwitchExchange(uint8_t cmd, const uint8_t* args, size_t na, uint32_t newBaud, uint32_t windowMs,
                           uint8_t* out, size_t cap);

  // Стан
  void setBaudState(uint32_t b) { baud_ = b; }
  uint32_t baud() const { return baud_; }
  uint8_t lastEvent() const { return lastEv_; }
  bool chipLost() const { return lost_; }
  uint32_t recoveredAt() const { return recAt_; }  // швидкість, на якій знайдено чіп під час відкату; 0 — не було

 private:
  Ch376Err sendCmd(uint8_t cmd, const uint8_t* args, size_t na);
  Ch376Err readReply(const uint8_t* ok, size_t nok, uint32_t tmoMs, uint8_t& out);
  Ch376Err setChipBaud(uint8_t lo, uint8_t hi);
  Ch376Err recover(uint32_t tried);
  size_t listen(uint32_t windowMs, uint8_t* out, size_t cap);

  Ch376Link& link_;
  uint32_t baud_ = kCh376DefaultBaud;
  uint8_t lastEv_ = 0;
  bool lost_ = false;
  uint32_t recAt_ = 0;
  int firstRx_ = -1;  // перший байт останньої readReply
  int setRx_ = -1;    // перший байт відповіді на останній SET_BAUDRATE
};

#pragma once
#include <stddef.h>
#include <stdint.h>

#include "config.h"
#include "usb/ch376_link.h"

// Результат кроку драйвера. Ok — успіх; решта — конкретна причина (для DBG,usb).
enum class Ch376Err : uint8_t {
  Ok,
  NoReply,       // байтів не було до тайм-ауту (чіп мовчить або відповідь обрізана)
  BadReply,      // байти були, але жоден не збігся з очікуваним (сміття, невірна відповідь)
  Timeout,       // подія не настала за тайм-аут
  Aborted,       // link повідомив про перервання
  Disconnected,  // подія відключення пристрою
  NoDisk,        // диска немає (ERR_DISK_DISCON або DISK_CONNECT без успіху)
  BadFs,         // диск є, але mount не вдався (непідтримувана ФС тощо)
  BadArg,        // некоректний аргумент (порожня таблиця швидкостей тощо)
  BaudFail,      // перемикання швидкості або відкат не вдались
};

// Коротка назва коду для логів (ASCII, щоб рядки DBG,usb були короткими).
const char* ch376ErrName(Ch376Err e);

// Спроба перемикання швидкості (для лога negotiateBaud).
struct Ch376Attempt {
  uint32_t baud;
  Ch376Err res;
};

// Протокольний рівень. Усі методи блокуючі, з тайм-аутами; викликаються з однієї задачі.
// Стан: лише baud() і lastEvent(); інше — у link.
class Ch376 {
 public:
  explicit Ch376(Ch376Link& link) : link_(link) {}

  // RESET_ALL, пауза, порт на kCh376DefaultBaud (9600), очистка RX.
  Ch376Err reset();

  // CHECK_EXIST: чіп має повернути x XOR kCh376CheckXor.
  Ch376Err checkExist();

  // SET_USB_MODE (наприклад kCh376ModeHost). Відповідь kCh376RetSuccess.
  Ch376Err setUsbMode(uint8_t mode);

  // GET_STATUS: один байт стану переривання (kCh376IntNone, якщо події немає).
  Ch376Err getStatus(uint8_t& st);

  // Очікування події: за INT (якщо kCh376IntPin і пін є) або опитуванням GET_STATUS.
  // ev — код події (kCh376IntConnect, kCh376IntDisconnect, ...). Тайм-аут → Timeout.
  Ch376Err waitEvent(uint8_t& ev, uint32_t timeoutMs);

  // Очікування підключення флешки (USB_INT_CONNECT). Відключення → Disconnected.
  Ch376Err waitConnect(uint32_t timeoutMs);

  // DISK_CONNECT: успіх → Ok; немає диска → NoDisk.
  Ch376Err diskConnect();

  // DISK_MOUNT: успіх → Ok; kCh376ErrDiskDisc → NoDisk; інші коди → BadFs.
  Ch376Err diskMount();

  // Перебір швидкостей з таблиці (від більшої до меншої). Викликати лише одразу після reset().
  // Для кожної: SET_BAUDRATE (на поточній швидкості), перемикання порту, checkExist.
  // Невдача → відкат на kCh376DefaultBaud (крок за кроком, див. rollbackTo).
  // Повертає Ok і записує досягнуту швидкість у baud(); інакше BaudFail (порт на 9600).
  // att — опційний масив записів спроб (до maxAtt); nAtt — загальна кількість спроб.
  Ch376Err negotiateBaud(const Ch376BaudDiv* table, size_t n, Ch376Attempt* att, size_t maxAtt, size_t& nAtt);

  // Поточна швидкість, яку драйвер вважає робочою.
  uint32_t baud() const { return baud_; }

  // Останній байт статусу з GET_STATUS.
  uint8_t lastEvent() const { return lastEv_; }

 private:
  Ch376Err sendCmd(uint8_t cmd, const uint8_t* args, size_t na);
  Ch376Err readReply(const uint8_t* ok, size_t nok, uint32_t tmoMs, uint8_t& out);
  Ch376Err setChipBaud(uint8_t lo, uint8_t hi);
  void rollbackTo(uint32_t fromBaud);

  Ch376Link& link_;
  uint32_t baud_ = kCh376DefaultBaud;
  uint8_t lastEv_ = 0;
};

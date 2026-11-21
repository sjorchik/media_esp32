#pragma once
#include <stddef.h>
#include <stdint.h>

#include "config.h"
#include "usb/ch376.h"
#include "usb/usb_volume_core.h"

#if ENABLE_USB_CH376

// M4c2. Чіп CH376S для кроків у loop: кожен метод — ОДНА коротка дія (без очікування подій). Довгі очікування (скид, підключення,
// DISK_CONNECT, DISK_MOUNT) робить викликач таймером між викликами: так app не блокується. Без Arduino.h; на залізі реалізація —
// usb_chip_uart.cpp (Serial2 + Ch376), у тестах — підміна. Усі виклики — з однієї задачі (app).
class IUsbChip {
 public:
  virtual ~IUsbChip() {}

  // INT# (GPIO, активний LOW; рівень 0 тримається до GET_STATUS) і лічильник спадних фронтів з ISR. Працюють і при вимкненому UART2.
  virtual bool hasInt() const = 0;   // пін INT# підключено (інакше статус опитується без оглядки на рівень)
  virtual bool intLow() const = 0;
  virtual uint32_t intEdges() const = 0;

  // Порт UART2. portWake — на швидкості, збереженій у чіпі, БЕЗ нового узгодження; portPark — без скидання чіпа.
  virtual bool portWake() = 0;       // false — не вистачило heap/порту
  virtual void portPark() = 0;       // Serial2.end(); ISR на INT# лишається
  virtual bool portOpen() const = 0;
  virtual uint32_t chipBaud() const = 0;  // швидкість, на якій зараз чіп (9600 після скиду)
  virtual bool chipHost() const = 0;      // чіп у host після останнього скиду: його можна паркувати

  // Обмін.
  virtual Ch376Err ping() = 0;                  // CHECK_EXIST
  virtual Ch376Err recover() = 0;               // програмний відкат (блокує до ≈0,4 с) — лише після збою ping()
  virtual Ch376Err resetSend() = 0;             // RESET_ALL без паузи
  virtual Ch376Err resetFinish() = 0;           // після паузи kCh376ResetWaitMs: порт на 9600
  virtual Ch376Err setBaud(uint32_t baud) = 0;  // одна спроба швидкості (0 — список, діагностика) із відкатом при збої; ≈50 мс
  virtual Ch376Err setHost() = 0;               // SET_USB_MODE host
  virtual Ch376Err status(uint8_t& st) = 0;     // GET_STATUS один раз: Ok + st (kCh376IntNone — подій немає)
  virtual Ch376Err diskConnectSend() = 0;       // лише відправка DISK_CONNECT; статус — status()
  virtual Ch376Err diskMountSend() = 0;         // лише відправка DISK_MOUNT; статус — status()
  // Ємність + швидка перевірка придатності розділу/ФС (MBR/GPT/BPB через probeDisk, ≈1 КБ heap лише на час виклику, без скану).
  // None — придатна; NoMedia — диск зник; UnsupportedFs/NoPartition — флешка є, ФС не підходить; інше — збій читання.
  virtual UsbOpenFail probe() = 0;
};

#endif  // ENABLE_USB_CH376

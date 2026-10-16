#pragma once
#include "config.h"

#if ENABLE_USB_CH376
#include "usb/ch376.h"

// Arduino-шар CH376S: Serial2 (RX16/TX17), опційні RST (13) і INT (4) з pins.h.
// Запускається лише з консолі (DBG,usb*), не при старті: Serial2 і RX-буфер (heap) виділяються тоді.
namespace ch376_uart {

// Запустити Serial2 на kCh376DefaultBaud і налаштувати піни. Ідемпотентно. Виклик з задачі console.
bool begin();

// Чи вже запущено (begin() викликали).
bool started();

// Протокольний рівень. До begin() не викликати команди; baud() і lastEvent() читаються безпечно.
Ch376& driver();

}  // namespace ch376_uart

#endif  // ENABLE_USB_CH376

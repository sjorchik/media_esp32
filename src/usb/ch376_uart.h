#pragma once
#include "config.h"

#if ENABLE_USB_CH376
#include "usb/ch376.h"

// Arduino-шар CH376S: Serial2 (RX16/TX17), RST (13, у платі не виведено), INT (4).
// Запускається лише командою консолі (DBG,usb*), не при старті.
namespace ch376_uart {

// Запустити Serial2 на 9600 і налаштувати піни. Ідемпотентно.
bool begin();

// Чи запущено.
bool started();

// Зупинити Serial2 (звільнити RX-буфер) і скинути стан драйвера на 9600. Ідемпотентно.
void stop();

// Рівень піна INT: 1 або 0; -1 — піна немає або драйвер не запущено.
int intLevel();

// Протокольний рівень. baud() і lastEvent() читаються безпечно і до begin().
Ch376& driver();

}  // namespace ch376_uart

#endif  // ENABLE_USB_CH376

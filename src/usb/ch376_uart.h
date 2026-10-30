#pragma once
#include "config.h"

#if ENABLE_USB_CH376
#include "usb/ch376.h"
#include "usb/ch376_file_reader.h"
#include "usb/ch376_fs.h"

// Arduino-шар CH376S: Serial2 (RX16/TX17), RST (13, у платі не виведено), INT (4).
// Запускається лише командою консолі (DBG,usb*), не при старті.
namespace ch376_uart {

// Запустити Serial2 на 9600, налаштувати піни, повісити ISR на INT# (falling → семафор). Ідемпотентно.
bool begin();

// Чи запущено.
bool started();

// Зупинити Serial2 (звільнити RX-буфер), зняти ISR і семафор, скинути стан драйвера на 9600 і файлового шару. Ідемпотентно.
void stop();

// Рівень піна INT: 1 або 0; -1 — піна немає або драйвер не запущено.
int intLevel();

// Протокольний рівень. baud() і lastEvent() читаються безпечно і до begin().
Ch376& driver();

// Файловий шар і читач (M4b1). Статичні обʼєкти; драйвер вони не запускають. stop() скидає їх стан.
// Трасування кадрів (діагностика): буфер у heap (≈0,8 КБ) лише поки ввімкнено. T — байти до чіпа, R — від чіпа.
// Переповнення відкидає хвіст. Вимкнення звільняє памʼять.
bool trace(bool on);
void traceDump();   // надрукувати накопичене і очистити

Ch376Fs& fs();
Ch376FileReader& reader();

}  // namespace ch376_uart

#endif  // ENABLE_USB_CH376


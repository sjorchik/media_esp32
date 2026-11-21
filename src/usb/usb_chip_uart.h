#pragma once
#include "config.h"

#if ENABLE_USB_CH376

#include "usb/usb_chip.h"

// M4c2. Реалізація IUsbChip на залізі: Serial2 + Ch376 з ch376_uart. Єдиний екземпляр (без стану, лише посилання на драйвер).
IUsbChip& uartChip();

#endif  // ENABLE_USB_CH376

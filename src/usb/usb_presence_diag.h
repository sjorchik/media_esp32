#pragma once
#include "proto/frame.h"

// Консоль M4c2: DBG,usb,park | wake | int[,reset] | pres[,auto|init]. Реалізація лише при ENABLE_USB_CH376=1.
// Експеримент здійсненності паркування чіпа: чіп лишається в host на швидкості, що збережена в ньому, при вимкненому UART2.
namespace UsbPresDiag {
// m.args[1] — підкоманда. true — розпізнано (навіть якщо аргументи хибні).
bool handle(const proto::Msg& m);
}  // namespace UsbPresDiag

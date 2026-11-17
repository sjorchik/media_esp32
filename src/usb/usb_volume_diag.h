#pragma once
#include "proto/frame.h"

// Консоль M4b3: DBG,usb,mount|umount|vls|vread|vseq|vtag. Реалізація лише при ENABLE_USB_CH376=1.
// Команди з M4a..M4b2 (init, ls, read, seq, sec, rawread, part ...) не змінено: префікс «v» — читання через SdFat.
namespace UsbVolDiag {
// m.args[1] — підкоманда. true — розпізнано (навіть якщо аргументи хибні: друкується підказка).
bool handle(const proto::Msg& m);
}  // namespace UsbVolDiag

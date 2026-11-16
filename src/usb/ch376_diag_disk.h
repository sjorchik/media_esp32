#pragma once
#include "proto/frame.h"

// Консоль M4b2: DBG,usb,rawread|part|rawspeed|rawseq|rawpre|rawcfg. Реалізація лише при ENABLE_USB_CH376=1.
// Команди DBG,usb,* з M4a/M4a2/M4b1 (зокрема sec) не змінено: ці команди живуть поруч.
namespace Ch376DiagDisk {
// m.args[1] — підкоманда. true — підкоманду розпізнано (навіть якщо аргументи хибні: друкується підказка).
bool handle(const proto::Msg& m);
}  // namespace Ch376DiagDisk

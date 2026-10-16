#pragma once
#include "proto/frame.h"

// Консольна діагностика CH376S: DBG,usb[,init|reset|ping,N|baud[,бод,K]].
// Реалізація лише при ENABLE_USB_CH376=1; при 0 цей модуль порожній, а debug_cmds.cpp його не викликає.
namespace Ch376Diag {
// m.args[0] == "usb"; m.args[1..] — підкоманда та аргументи.
void handle(const proto::Msg& m);
}  // namespace Ch376Diag

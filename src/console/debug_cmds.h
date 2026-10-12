#pragma once
#include "proto/frame.h"

// Налагоджувальні команди DBG,… (лише UART0).
namespace DebugCmds {
void handle(const proto::Msg& m);
}
#pragma once
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

#include "core/commands.h"

namespace Router {
void init(QueueHandle_t cmdQueue);
// body — змінюваний буфер тіла кадру (NAME,арг,…); викликається з задач link_rx і console.
void dispatch(char* body, Origin origin);
bool post(const Cmd& c);  // для внутрішніх команд
}  // namespace Router
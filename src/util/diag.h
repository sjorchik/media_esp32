#pragma once
#include <stdint.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

// Діагностика heap і стеків (розділ «Обмеження по ресурсах»)
namespace Diag {
void registerTask(const char* name, TaskHandle_t h);
void logHeap(const char* tag);   // free / largest block / min free heap
void logTasks();                 // watermark стеку кожної зареєстрованої задачі
void tick(uint32_t nowMs);       // періодичний лог (kDiagPeriodMs)
}  // namespace Diag
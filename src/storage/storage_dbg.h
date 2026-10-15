#pragma once
#include <stdint.h>

// DBG-команди сховища (M5b2): boot, settings. Консоль лише ставить запит (post), виконує loopTask (service) —
// так запис і читання NVS відбуваються з однієї задачі (статичні буфери сховищ не потокобезпечні).
namespace StorageDbg {

enum class Req : uint8_t { None = 0, Boot, BootClear, BootCrash, BootSet, Settings, SettingsClear };

bool post(Req r, uint8_t arg = 0);   // з будь-якої задачі; false: попередній запит ще не виконано
void service();                      // лише з loopTask (main loop)

}  // namespace StorageDbg

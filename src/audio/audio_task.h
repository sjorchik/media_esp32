#pragma once
#include <stdint.h>

#include "audio/audio_out.h"

class PcmRing;

// Задача аудіо (ядро 1, prio 6): тест-тон для DBG,tone і помпа PCM з кільця джерела в AudioOut
// (єдиний продюсер write()). На етапах 3–4 тут же працюватиме декодер MP3.
namespace AudioTask {
void begin(AudioOut& out);
bool requestTone(uint16_t hz, uint16_t ms);  // false, якщо зайнято
bool busy();
void attachStream(PcmRing* ring);            // кільце PCM (один раз на старті)
void wake();                                 // нові дані в кільці (з будь-якої задачі, не з ISR)
}  // namespace AudioTask
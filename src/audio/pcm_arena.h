#pragma once
#include <stddef.h>
#include <stdint.h>

#include "config.h"

// Єдина статична область (kPcmArenaBytes), спільна для двох споживачів — вони ніколи не працюють разом:
//   BT: перші kBtRingBytes байтів = кільце PCM (PcmRing);
//   SD: набір декодера (стан minimp3, вхідний буфер, PCM-кадр, статичний стек задачі «mp3»),
//       розкладка — struct SdSet у mp3_player.cpp; стек лежить ПІСЛЯ перших kBtRingBytes байтів,
//       тому BT-кільце його не зачіпає (задача «mp3» створюється один раз і далі живе припаркованою).
// Власник міняється лише в activate()/deactivate() джерел (задача app), порядок гарантує SourceManager:
// deactivate() попереднього завершується до activate() нового.
alignas(16) extern uint8_t g_pcmArena[kPcmArenaBytes];

namespace PcmArena {
enum class Owner : uint8_t { None, Bt, Sd };

Owner owner();
const char* ownerName(Owner o);
bool acquire(Owner o);   // лише з app; true, якщо область вільна або вже ваша
void release(Owner o);   // лише власник (інакше ігнорується з попередженням)
}  // namespace PcmArena

#pragma once
#include <stdint.h>

// M5c6. Сторож зависання app. Окремий таймер esp_timer (власна задача, не залежить від app, що може бути заблокована/голодувати):
// arm() заводить одноразовий таймер; якщо app не викликає disarm() за afterMs, у контексті таймера один раз викликається dump(ctx),
// який виводить внутрішній стан у лог. Лише діагностика, поведінку не змінює. Не Arduino-збірка (native): no-op.
namespace StallWatch {
using DumpFn = void (*)(void* ctx);
#ifdef ARDUINO
void arm(DumpFn dump, void* ctx, uint32_t afterMs);
void disarm();
#else
inline void arm(DumpFn, void*, uint32_t) {}
inline void disarm() {}
#endif
}  // namespace StallWatch

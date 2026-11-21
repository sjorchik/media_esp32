#pragma once
#include "config.h"

#if ENABLE_USB_CH376

#include "usb/usb_presence_core.h"

// M4c2. Сервіс присутності USB-флешки (Arduino-шар над UsbPresenceCore): єдиний власник прапорця MEDIA usb. Усі виклики — з app
// (SourceManager::loop, джерело USB, UsbVolume); консольні — лише DBG,usb,pres*/park/wake/int (див. usb_presence_diag).
namespace UsbPresence {
void begin(uint32_t nowMs);                                          // після старту: фонова ініціалізація через kUsbPresInitDelayMs
void tick(uint32_t nowMs, bool btActive, uint32_t freeHeap);         // щоітерації loop; один короткий крок
UsbPres state();
UsbPres stateForOpen();                                              // Unknown, поки подію не оброблено
bool takeChange(bool& flag);                                         // прапорець MEDIA usb змінився → викликач оновлює DeviceState і шле MEDIA
void yield();                                                        // перед відкриттям тому: віддати порт
void sessionOpened();
void openFailed(UsbOpenFail f);
void volumeLost();
void sessionClosed(bool recheck);
void invalidate();                                                   // чіп скинуто ззовні (DBG,usb,stop)
void restartInit();                                                  // DBG,usb,pres,init
void setManual(bool on);                                             // консольні експерименти: фон не втручається
bool manual();
void dbgPrint();                                                     // DBG,usb,pres
}  // namespace UsbPresence

#endif  // ENABLE_USB_CH376

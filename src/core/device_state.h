#pragma once
#include <stddef.h>

// Спільний стан пристрою (носії, BT). Потокобезпечний (spinlock) — BT-колбеки йдуть з інших задач.
namespace DeviceState {
void init();
bool sd();
bool usb();
void setMedia(bool sd, bool usb);
void setSd(bool sd);  // M5c4b: лише sd; usb не чіпає (його веде драйвер USB / STUB_USB_PRESENT)
bool btConnected();
void btName(char* out, size_t cap);
void setBt(bool connected, const char* name);
}  // namespace DeviceState

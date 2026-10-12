#pragma once
#include <stddef.h>

// Спільний стан пристрою (носії, BT). Потокобезпечний (spinlock) — BT-колбеки йдуть з інших задач.
namespace DeviceState {
void init();
bool sd();
bool usb();
void setMedia(bool sd, bool usb);
bool btConnected();
void btName(char* out, size_t cap);
void setBt(bool connected, const char* name);
}  // namespace DeviceState
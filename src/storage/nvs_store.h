#pragma once
#include <stddef.h>
#include <stdint.h>

// Обгортка NVS (namespace "media"). Потокобезпечна. Резюме (шлях/позиція) — етапи 2+.
namespace NvsStore {
bool begin();
bool getU8(const char* key, uint8_t& v);
bool putU8(const char* key, uint8_t v);
bool getU32(const char* key, uint32_t& v);
bool putU32(const char* key, uint32_t v);
bool getStr(const char* key, char* out, size_t cap);
bool putStr(const char* key, const char* v);
}  // namespace NvsStore
#pragma once
#include <stddef.h>
#include <stdint.h>

// Обгортка NVS (namespace "media"). Потокобезпечна. Блоби (M5b1) — для резюме треку (storage/resume_store).
namespace NvsStore {
bool begin();
bool getU8(const char* key, uint8_t& v);
bool putU8(const char* key, uint8_t v);
bool getU32(const char* key, uint32_t& v);
bool putU32(const char* key, uint32_t v);
bool getStr(const char* key, char* out, size_t cap);
bool putStr(const char* key, const char* v);
size_t blobLen(const char* key);                                        // 0 = ключа немає або порожній
bool getBlob(const char* key, void* out, size_t cap, size_t& len);      // false: немає, не влізло або помилка
bool putBlob(const char* key, const void* v, size_t len);
bool remove(const char* key);                                           // true, якщо ключа вже немає
}  // namespace NvsStore

#pragma once
#include <stdint.h>

#include "core/types.h"

// Єдине місце формування подій WROOM → S3. Потокобезпечно, не блокує.
// Пріоритет: READY/STATE/MEDIA/BT/ERR — високий; TRACK/FOLDER/META/TIME — низький.
namespace Events {
void ready();
void state(SrcId src, PlayState st, bool shuf, uint8_t rep);
void media(bool sd, bool usb);
void bt(bool connected, const char* name);
void meta(const char* artist, const char* title, const char* album);
void track(uint16_t n, uint16_t total);
void folder(const char* name, uint16_t count);
void position(uint32_t posS, uint32_t durS);   // подія TIME
void err(const char* code, const char* detail = nullptr);
}  // namespace Events
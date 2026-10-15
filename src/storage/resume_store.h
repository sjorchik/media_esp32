#pragma once
#include <stdint.h>

#include "storage/resume_record.h"

// Сховище резюме (M5b1) поверх NvsStore: ключі rsm_sd / rsm_usb. ВИКЛИКАТИ ЛИШЕ З ЗАДАЧІ app (loopTask),
// не із задачі mp3: запис у флеш на мить зупиняє кеш (статичний буфер блоба тут не потокобезпечний).
// Помилка NVS лише логується — відтворення не блокується. Джерело usb — лише ключ, без логіки.
namespace ResumeStore {

enum class Src : uint8_t { Sd = 0, Usb = 1 };
enum class Load : uint8_t { Ok, None, Bad };   // None: запису немає; Bad: є, але не пройшов розбір (why — причина)

Load load(Src s, Resume::Record& r, Resume::ParseErr* why = nullptr);  // при Ok кеш CRC = вміст NVS
bool save(Src s, const char* path, uint32_t off, uint32_t ms);        // true: записано або вже було таке саме
bool erase(Src s);

const char* key(Src s);

}  // namespace ResumeStore

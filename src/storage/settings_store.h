#pragma once
#include <stdint.h>

#include "storage/settings_record.h"

// Сховище налаштувань (M5b2) поверх NvsStore: ключ set_v1. ВИКЛИКАТИ ЛИШЕ З ЗАДАЧІ app (loopTask):
// статичний буфер блоба не потокобезпечний, запис у флеш на мить зупиняє кеш.
// Запис лише якщо блоб змінився (кеш CRC). Помилка NVS лише логується.
namespace SettingsStore {

enum class Load : uint8_t { Ok, None, Bad };   // None: запису немає; Bad: є, але не пройшов розбір

Load load(Settings::Record& r, Settings::ParseErr* why = nullptr);   // при Ok кеш CRC = вміст NVS
bool save(uint8_t lastSrc, uint8_t shuf, uint8_t rep);              // true: записано або вже було таке саме
bool erase();

}  // namespace SettingsStore

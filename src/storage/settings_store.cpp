#include "storage/settings_store.h"

#include "storage/nvs_store.h"
#include "util/log.h"

namespace {

constexpr const char* TAG = "set";
constexpr const char* kKey = "set_v1";

uint8_t s_buf[Settings::kBlobSize];  // єдиний статичний буфер блоба (10 Б); лише loopTask
uint32_t s_crc = 0;                  // CRC блоба, що лежить у NVS (запис або читання)
bool s_have = false;

inline uint32_t blobCrc(const uint8_t* b, size_t n) {  // CRC — в останніх 4 байтах блоба
  return (uint32_t)b[n - 4] | ((uint32_t)b[n - 3] << 8) | ((uint32_t)b[n - 2] << 16) | ((uint32_t)b[n - 1] << 24);
}

}  // namespace

SettingsStore::Load SettingsStore::load(Settings::Record& r, Settings::ParseErr* why) {
  if (why) *why = Settings::ParseErr::Ok;
  size_t n = NvsStore::blobLen(kKey);
  if (n == 0) {
    s_have = false;
    return Load::None;
  }
  size_t got = 0;
  Settings::ParseErr e = Settings::ParseErr::Short;
  if (n <= sizeof s_buf && NvsStore::getBlob(kKey, s_buf, sizeof s_buf, got)) e = Settings::parse(s_buf, got, r);
  else if (n > sizeof s_buf) e = Settings::ParseErr::Len;
  if (e != Settings::ParseErr::Ok) {
    if (why) *why = e;
    s_have = false;  // у NVS сміття: наступний запис його перезапише
    LOGW(TAG, "%s: запис пошкоджено (%s), %u Б", kKey, Settings::errName(e), (unsigned)n);
    return Load::Bad;
  }
  s_crc = blobCrc(s_buf, got);
  s_have = true;
  return Load::Ok;
}

bool SettingsStore::save(uint8_t lastSrc, uint8_t shuf, uint8_t rep) {
  size_t n = Settings::serialize(lastSrc, shuf, rep, s_buf, sizeof s_buf);
  if (!n) {
    LOGW(TAG, "%s: значення поза діапазоном (src=%u shuf=%u rep=%u) — не зберігається", kKey, (unsigned)lastSrc,
         (unsigned)shuf, (unsigned)rep);
    return false;
  }
  uint32_t crc = blobCrc(s_buf, n);
  if (s_have && s_crc == crc) return true;  // не змінилось — флеш не чіпаємо
  if (!NvsStore::putBlob(kKey, s_buf, n)) {
    s_have = false;  // стан NVS невідомий — наступна спроба напише знову
    LOGW(TAG, "%s: помилка запису в NVS", kKey);
    return false;
  }
  s_crc = crc;
  s_have = true;
  return true;
}

bool SettingsStore::erase() {
  s_have = false;
  bool ok = NvsStore::remove(kKey);
  if (!ok) LOGW(TAG, "%s: не вдалося стерти", kKey);
  return ok;
}

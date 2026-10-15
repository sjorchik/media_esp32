#include "storage/nvs_store.h"

#include <Arduino.h>
#include <Preferences.h>

#include "util/log.h"

namespace {
Preferences s_p;
SemaphoreHandle_t s_mtx = nullptr;
bool s_ok = false;

struct G {
  G() { if (s_mtx) xSemaphoreTake(s_mtx, portMAX_DELAY); }
  ~G() { if (s_mtx) xSemaphoreGive(s_mtx); }
};
}  // namespace

bool NvsStore::begin() {
  s_mtx = xSemaphoreCreateMutex();
  s_ok = s_p.begin("media", false);
  if (!s_ok) LOGE("nvs", "не вдалося відкрити NVS");
  // M5c6: застарілий ключ last_src (u8) від попередньої версії; M5b2 його не читає. Стираємо один раз.
  if (s_ok && s_p.isKey("last_src")) {
    const bool ok = s_p.remove("last_src");
    LOGI("nvs", "застарілий ключ last_src стерто: %s", ok ? "ок" : "помилка");
  }
  return s_ok;
}

bool NvsStore::getU8(const char* key, uint8_t& v) {
  G g;
  if (!s_ok || !s_p.isKey(key)) return false;
  v = s_p.getUChar(key, 0);
  return true;
}

bool NvsStore::putU8(const char* key, uint8_t v) {
  G g;
  return s_ok && s_p.putUChar(key, v) == sizeof(uint8_t);
}

bool NvsStore::getU32(const char* key, uint32_t& v) {
  G g;
  if (!s_ok || !s_p.isKey(key)) return false;
  v = s_p.getUInt(key, 0);
  return true;
}

bool NvsStore::putU32(const char* key, uint32_t v) {
  G g;
  return s_ok && s_p.putUInt(key, v) == sizeof(uint32_t);
}

bool NvsStore::getStr(const char* key, char* out, size_t cap) {
  G g;
  if (!s_ok || !cap || !s_p.isKey(key)) return false;
  return s_p.getString(key, out, cap) > 0;
}

bool NvsStore::putStr(const char* key, const char* v) {
  G g;
  return s_ok && s_p.putString(key, v) > 0;
}

size_t NvsStore::blobLen(const char* key) {
  G g;
  if (!s_ok || !s_p.isKey(key)) return 0;
  return s_p.getBytesLength(key);
}

bool NvsStore::getBlob(const char* key, void* out, size_t cap, size_t& len) {
  G g;
  len = 0;
  if (!s_ok || !out || !cap || !s_p.isKey(key)) return false;
  size_t n = s_p.getBytesLength(key);
  if (n == 0 || n > cap) return false;
  if (s_p.getBytes(key, out, n) != n) return false;
  len = n;
  return true;
}

bool NvsStore::putBlob(const char* key, const void* v, size_t len) {
  G g;
  return s_ok && v && len && s_p.putBytes(key, v, len) == len;
}

bool NvsStore::remove(const char* key) {
  G g;
  if (!s_ok) return false;
  return !s_p.isKey(key) || s_p.remove(key);
}


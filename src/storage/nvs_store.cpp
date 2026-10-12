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
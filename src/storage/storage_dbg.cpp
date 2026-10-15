#include "storage/storage_dbg.h"

#include <Arduino.h>
#include <atomic>
#include <stdlib.h>

#include "config.h"
#include "core/types.h"
#include "storage/boot_guard_store.h"
#include "storage/settings_store.h"
#include "util/log.h"

namespace {

constexpr const char* TAG = "dbg";
std::atomic<uint16_t> s_req{0};  // (Req << 8) | арг; 0 = немає запиту

void showBoot() {
  LOGI(TAG, "boot: причина=%s аварійний=%d лічильник=%u/%u захист=%d стабільно=%d AUTO_RESUME_SOURCE=%d аптайм=%u с",
       BootGuard::reasonName(), BootGuard::wasCrash() ? 1 : 0, (unsigned)BootGuard::count(), (unsigned)kCrashLoopMax,
       BootGuard::protect() ? 1 : 0, BootGuard::stableDone() ? 1 : 0, (int)AUTO_RESUME_SOURCE,
       (unsigned)(millis() / 1000));
}

void showSettings() {
  Settings::Record r;
  Settings::ParseErr why = Settings::ParseErr::Ok;
  switch (SettingsStore::load(r, &why)) {
    case SettingsStore::Load::Ok:
      LOGI(TAG, "settings: set_v1 v%u last=%s(%u) shuf=%u rep=%u CRC ok", (unsigned)r.version, srcName((SrcId)r.lastSrc),
           (unsigned)r.lastSrc, (unsigned)r.shuf, (unsigned)r.rep);
      break;
    case SettingsStore::Load::None:
      LOGI(TAG, "settings: set_v1 запису немає (діють значення за замовчуванням)");
      break;
    default:
      LOGW(TAG, "settings: set_v1 пошкоджено (%s)", Settings::errName(why));
      break;
  }
}

}  // namespace

bool StorageDbg::post(Req r, uint8_t arg) {
  uint16_t none = 0;
  return s_req.compare_exchange_strong(none, (uint16_t)(((uint16_t)r << 8) | arg));
}

void StorageDbg::service() {
  uint16_t v = s_req.load();
  if (!v) return;
  const Req r = (Req)(v >> 8);
  const uint8_t arg = (uint8_t)(v & 0xFF);
  (void)arg;
  switch (r) {
    case Req::Boot:
      showBoot();
      break;
    case Req::BootClear:
      LOGI(TAG, "boot,clear: %s", BootGuard::clear() ? "лічильник скинуто (режим захисту цього запуску не змінено)" : "помилка");
      break;
    case Req::BootSet:
#if BOOT_TEST_CMDS
      LOGI(TAG, "boot,set: лічильник=%u %s", (unsigned)arg, BootGuard::setCount(arg) ? "записано" : "помилка");
#endif
      break;
    case Req::BootCrash:
#if BOOT_TEST_CMDS
      LOGW(TAG, "boot,crash: abort() через 50 мс (тест аварійного перезапуску)");
      delay(50);
      abort();
#endif
      break;
    case Req::Settings:
      showSettings();
      break;
    case Req::SettingsClear:
      LOGI(TAG, "settings,clear: %s (значення в памʼяті лишилися до перезапуску)", SettingsStore::erase() ? "set_v1 стерто" : "помилка");
      break;
    default:
      break;
  }
  s_req.store(0);
}

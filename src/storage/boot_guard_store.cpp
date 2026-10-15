#include "storage/boot_guard_store.h"

#include "config.h"
#include "esp_system.h"
#include "storage/boot_guard.h"
#include "storage/nvs_store.h"
#include "storage/resume_store.h"
#include "util/log.h"

namespace {

constexpr const char* TAG = "boot";
constexpr const char* kKey = "boot_crash";

esp_reset_reason_t s_rst = ESP_RST_UNKNOWN;
bool s_crash = false;
bool s_prot = false;
bool s_stable = false;
uint8_t s_cnt = 0;  // дзеркало значення в NVS (відсутній ключ = 0)

bool isCrash(esp_reset_reason_t r) {
  return r == ESP_RST_PANIC || r == ESP_RST_INT_WDT || r == ESP_RST_TASK_WDT || r == ESP_RST_WDT;
}

bool writeCount(uint8_t v) {  // пише лише якщо відрізняється від дзеркала
  if (v == s_cnt) return true;
  if (!NvsStore::putU8(kKey, v)) {
    LOGW(TAG, "%s: помилка запису в NVS", kKey);
    return false;
  }
  s_cnt = v;
  return true;
}

}  // namespace

void BootGuard::init() {
  s_rst = esp_reset_reason();
  s_crash = isCrash(s_rst);
  uint8_t c = 0;
  if (!NvsStore::getU8(kKey, c)) c = 0;
  s_cnt = c;
  BootLogic::Decision d = BootLogic::onBoot(s_crash ? BootLogic::Reason::Crash : BootLogic::Reason::Normal, c, kCrashLoopMax);
  if (d.write) writeCount(d.count);
  s_prot = d.protect;
  LOGI(TAG, "перезапуск: %s (%s), аварій підряд: %u (поріг %u)", reasonName(), s_crash ? "аварійний" : "нормальний",
       (unsigned)d.count, (unsigned)kCrashLoopMax);
  if (s_prot) {
    ResumeStore::erase(ResumeStore::Src::Sd);
    ResumeStore::erase(ResumeStore::Src::Usb);
    LOGW(TAG, "РЕЖИМ ЗАХИСТУ: %u аварій підряд — резюме пропущено, rsm_sd/rsm_usb стерто, AUTO_RESUME_SOURCE ігнорується",
         (unsigned)d.count);
  }
}

void BootGuard::tick(uint32_t nowMs) {
  if (s_stable || nowMs < kBootStableMs) return;
  s_stable = true;
  BootLogic::Decision d = BootLogic::onStable(s_cnt);
  if (d.write && writeCount(d.count)) LOGI(TAG, "стабільна робота %u с — лічильник аварій скинуто", (unsigned)(kBootStableMs / 1000));
}

bool BootGuard::protect() { return s_prot; }

const char* BootGuard::reasonName() {
  switch (s_rst) {
    case ESP_RST_POWERON: return "POWERON";
    case ESP_RST_EXT: return "EXT";
    case ESP_RST_SW: return "SW";
    case ESP_RST_PANIC: return "PANIC";
    case ESP_RST_INT_WDT: return "INT_WDT";
    case ESP_RST_TASK_WDT: return "TASK_WDT";
    case ESP_RST_WDT: return "WDT";
    case ESP_RST_DEEPSLEEP: return "DEEPSLEEP";
    case ESP_RST_BROWNOUT: return "BROWNOUT";
    case ESP_RST_SDIO: return "SDIO";
    default: return "UNKNOWN";
  }
}

bool BootGuard::wasCrash() { return s_crash; }
uint8_t BootGuard::count() { return s_cnt; }
bool BootGuard::stableDone() { return s_stable; }
bool BootGuard::clear() { return writeCount(0); }
bool BootGuard::setCount(uint8_t v) { return writeCount(v); }

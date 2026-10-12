#include <Arduino.h>
#include "esp_bt.h"

#include "audio/audio_out.h"
#include "audio/audio_task.h"
#include "config.h"
#include "console/console.h"
#include "core/app.h"
#include "core/commands.h"
#include "core/device_state.h"
#include "core/events.h"
#include "core/router.h"
#include "proto/link.h"
#include "sources/stub_source.h"
#include "storage/nvs_store.h"
#include "util/diag.h"
#include "util/log.h"

// Не даємо initArduino() звільнити всю памʼять BT (потрібен BT Classic на етапі 3);
// BLE-частину звільняємо самі нижче.
extern "C" bool btInUse() { return true; }

AudioOut g_audio;
SourceManager g_sources;

static StubSource s_bt(SrcId::BT), s_sd(SrcId::SD), s_usb(SrcId::USB);
static QueueHandle_t s_cmdQ = nullptr;

void setup() {
  AudioOut::earlyMute();  // 1-й рядок: XSMT=LOW (мʼют)
  Log::begin();
  LOGI("main", "медіамодуль WROOM fw=%s proto=%d", FW_VERSION, kProtoVer);

#if RELEASE_BLE_MEM
  esp_err_t er = esp_bt_controller_mem_release(ESP_BT_MODE_BLE);  // лише BT Classic
  LOGI("main", "bt mem_release(BLE): %d", (int)er);
#endif

  Diag::registerTask("app", xTaskGetCurrentTaskHandle());
  NvsStore::begin();
  DeviceState::init();
  g_audio.begin();

  s_cmdQ = xQueueCreate(kCmdQueueDepth, sizeof(Cmd));
  Router::init(s_cmdQ);
  g_sources.begin(&g_audio, &s_bt, &s_sd, &s_usb);

  Link::begin();
  Console::begin();
  AudioTask::begin(g_audio);

  Events::ready();  // READY,<fw>,1; початковий стан src=off, state=idle, DAC замʼючений
  Diag::logHeap("boot");
  Diag::logTasks();
#if AUTO_RESUME_SOURCE
  g_sources.resumeLast();
#endif
}

void loop() {
  Cmd c;
  if (xQueueReceive(s_cmdQ, &c, pdMS_TO_TICKS(10)) == pdTRUE) {
    do {
      g_sources.handle(c);
    } while (xQueueReceive(s_cmdQ, &c, 0) == pdTRUE);
  }
  uint32_t now = millis();
  g_sources.loop(now);
  Diag::tick(now);
}
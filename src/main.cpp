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

// btInUse() визначає саме ядро Arduino (esp32-hal-bt.c, повертає true для збірки з BT),
// тому свого визначення тут немає: initArduino() не звільняє памʼять BT Classic.
// BLE-частину звільняємо самі нижче.

AudioOut g_audio;
SourceManager g_sources;
BtSource g_bt;

static StubSource s_sd(SrcId::SD), s_usb(SrcId::USB);
static QueueHandle_t s_cmdQ = nullptr;

void setup() {
  AudioOut::earlyMute();  // 1-й рядок: DAC у мʼюті (рівень залежить від DAC_MUTE_ACTIVE_HIGH)
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
  g_sources.begin(&g_audio, &g_bt, &s_sd, &s_usb);

  Link::begin();
  Console::begin();
  AudioTask::begin(g_audio);

  Events::ready();  // READY,<fw>,1; початковий стан src=off, state=idle, DAC замʼючений
  Diag::logHeap("boot-no-bt");
  g_bt.startStack();  // BT-стек піднімається один раз; після цього лишається ініціалізованим
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
#include "console/debug_cmds.h"

#include <stdlib.h>
#include <strings.h>

#include "audio/audio_task.h"
#include "core/app.h"
#include "core/device_state.h"
#include "core/router.h"
#include "proto/link.h"
#include "util/diag.h"
#include "util/log.h"

namespace {

constexpr const char* TAG = "dbg";

bool toInt(const char* s, long& v) {
  if (!s || !*s) return false;
  char* e = nullptr;
  v = strtol(s, &e, 10);
  return *e == 0;
}

void help() {
  LOGI(TAG, "Команди контракту: HELLO,1 GET SRC,<bt|sd|usb|off> PLAY PAUSE TOGGLE STOP NEXT PREV FNEXT FPREV SHUF,<0|1> REP,<0|1|2>");
  LOGI(TAG, "Формат: NAME,арг,… або $BODY*XX");
  LOGI(TAG, "DBG,help | DBG,heap | DBG,tasks | DBG,state");
  LOGI(TAG, "DBG,tone[,Гц[,мс]] — тест-тон (лише при src=off), типово 440 Гц 1500 мс");
  LOGI(TAG, "DBG,media,<sd>,<usb> — імітація hot-plug носіїв (0/1)");
}

}  // namespace

void DebugCmds::handle(const proto::Msg& m) {
  if (m.argc < 1) { help(); return; }
  const char* c = m.args[0];

  if (!strcasecmp(c, "help")) {
    help();
  } else if (!strcasecmp(c, "heap")) {
    Diag::logHeap("dbg");
  } else if (!strcasecmp(c, "tasks")) {
    Diag::logTasks();
  } else if (!strcasecmp(c, "state")) {
    char nm[33];
    DeviceState::btName(nm, sizeof nm);
    Link::Stats s = Link::stats();
    LOGI(TAG, "src=%s state=%s shuf=%d rep=%u", srcName(g_sources.current()),
         stateName(g_sources.state()), g_sources.shuffle() ? 1 : 0, (unsigned)g_sources.repeat());
    LOGI(TAG, "media sd=%d usb=%d | bt=%d '%s' | dac_muted=%d", DeviceState::sd(), DeviceState::usb(),
         DeviceState::btConnected(), nm, g_audio.isMuted());
    LOGI(TAG, "link good=%u bad=%u dropped=%u partner=%s", (unsigned)s.good, (unsigned)s.bad,
         (unsigned)s.dropped, s.alive ? "так" : "ні");
  } else if (!strcasecmp(c, "tone")) {
    long hz = 440, ms = 1500;
    if ((m.argc >= 2 && (!toInt(m.args[1], hz) || hz < 50 || hz > 8000)) ||
        (m.argc >= 3 && (!toInt(m.args[2], ms) || ms < 100 || ms > 10000))) {
      LOGW(TAG, "DBG,tone[,50..8000[,100..10000]]");
      return;
    }
    if (g_sources.current() != SrcId::Off) { LOGW(TAG, "тон лише при src=off"); return; }
    if (!AudioTask::requestTone((uint16_t)hz, (uint16_t)ms)) LOGW(TAG, "аудіо зайняте");
  } else if (!strcasecmp(c, "media")) {
    long sd, usb;
    if (m.argc != 3 || !toInt(m.args[1], sd) || !toInt(m.args[2], usb) || (sd | usb) & ~1L) {
      LOGW(TAG, "DBG,media,<0|1>,<0|1>");
      return;
    }
    Router::post(Cmd{proto::CmdId::DbgMedia, (int16_t)(sd | (usb << 1)), Origin::Console});
  } else {
    LOGW(TAG, "невідома DBG-команда '%s' (DBG,help)", c);
  }
}
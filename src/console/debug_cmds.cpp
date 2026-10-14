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
  LOGI(TAG, "DBG,bt — стан BT, кодек, частота, лічильники PCM, метадані");
  LOGI(TAG, "DBG,bt,pair — вікно видимості 120 с | DBG,bt,forget — забути телефон | DBG,bt,reset — скинути лічильники");
  LOGI(TAG, "DBG,sd — присутність, SPI, тип/FS/обсяг | DBG,scan — підсумок останнього скану");
  LOGI(TAG, "DBG,list[,N] — перші N рядків плейлиста (типово %d, 0 = усі) | DBG,sdread,<n> — прочитати трек №n (як у list), КБ/с", kDbgListDefault);
  LOGI(TAG, "DBG,play,<n> — відтворити трек №n (як у list; потрібен src=sd) | DBG,decode — статистика декодера, буфери, heap");
  LOGI(TAG, "DBG,tag,<n> — розібрати ID3 треку №n без відтворення (src=sd, stop): версія, кодування, hex, UTF-8, час");
  LOGI(TAG, "DBG,nav — індекс, папка/діапазон, repeat, shuffle, помилки підряд, історія переходів");
  LOGI(TAG, "DBG,link[,reset] — втрати черг, злиті TIME, час відправки кадру (UART1+дзеркало)");
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
  } else if (!strcasecmp(c, "bt")) {
    if (m.argc == 1) g_bt.dump();
    else if (!strcasecmp(m.args[1], "pair")) g_bt.requestPair();
    else if (!strcasecmp(m.args[1], "forget")) g_bt.requestForget();
    else if (!strcasecmp(m.args[1], "reset")) g_bt.resetStats();
    else LOGW(TAG, "DBG,bt[,pair|forget|reset]");
  } else if (!strcasecmp(c, "sd") || !strcasecmp(c, "scan") || !strcasecmp(c, "list") || !strcasecmp(c, "sdread") ||
             !strcasecmp(c, "play") || !strcasecmp(c, "decode") ||
             !strcasecmp(c, "nav") || !strcasecmp(c, "tag")) {
    SdSource::Dbg k = !strcasecmp(c, "sd") ? SdSource::Dbg::Info : !strcasecmp(c, "scan") ? SdSource::Dbg::Scan
                      : !strcasecmp(c, "list") ? SdSource::Dbg::List : !strcasecmp(c, "play") ? SdSource::Dbg::Play
                      : !strcasecmp(c, "decode") ? SdSource::Dbg::Decode
                      : !strcasecmp(c, "nav") ? SdSource::Dbg::Nav
                      : !strcasecmp(c, "tag") ? SdSource::Dbg::Tag : SdSource::Dbg::Read;
    long n = (k == SdSource::Dbg::List) ? kDbgListDefault : 0;
    if (k == SdSource::Dbg::List && m.argc >= 2 && (!toInt(m.args[1], n) || n < 0 || n > 65535)) {
      LOGW(TAG, "DBG,list[,0..65535]");
      return;
    }
    if ((k == SdSource::Dbg::Read || k == SdSource::Dbg::Play || k == SdSource::Dbg::Tag) && (m.argc != 2 || !toInt(m.args[1], n) || n < 1 || n > 65535)) {
      LOGW(TAG, "DBG,%s,<n> (n ≥ 1, номер із DBG,list)", k == SdSource::Dbg::Play ? "play" : k == SdSource::Dbg::Tag ? "tag" : "sdread");
      return;
    }
    if (!g_sd.dbg(k, (int)n)) LOGW(TAG, "SD: попередня DBG-команда ще виконується");
  } else if (!strcasecmp(c, "link")) {
    if (m.argc >= 2 && !strcasecmp(m.args[1], "reset")) {
      Link::resetTxStats();
      LOGI(TAG, "лічильники лінку скинуто");
      return;
    }
    Link::Stats s = Link::stats();
    LOGI(TAG, "link: tx=%u втрати hi=%u lo=%u | TIME злито=%u | макс. відправка=%u мкс, з них дзеркало UART0=%u мкс",
         (unsigned)s.txItems, (unsigned)s.lostHi, (unsigned)s.lostLo, (unsigned)s.timeCoalesced,
         (unsigned)s.txMaxUs, (unsigned)s.mirrorMaxUs);
  } else {
    LOGW(TAG, "невідома DBG-команда '%s' (DBG,help)", c);
  }
}




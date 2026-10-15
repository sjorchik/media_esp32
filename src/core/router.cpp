#include "core/router.h"

#include <string.h>

#include "config.h"
#include "console/debug_cmds.h"
#include "core/events.h"
#include "proto/frame.h"
#include "proto/proto_table.h"
#include "util/log.h"

namespace {

constexpr const char* TAG = "router";
QueueHandle_t s_q = nullptr;

// Лише десяткові цифри (контракт, розд. 2): без пробілів, знаку, префіксів.
bool parseInt(const char* s, long& v) {
  if (!s || !*s) return false;
  v = 0;
  for (const char* p = s; *p; p++) {
    if (*p < '0' || *p > '9') return false;
    v = v * 10 + (*p - '0');
    if (v > 100000) return false;  // усі валідні аргументи ≤ 2; захист від переповнення
  }
  return true;
}

bool parseSrc(const char* s, SrcId& out) {
  if (!strcmp(s, "bt")) out = SrcId::BT;
  else if (!strcmp(s, "sd")) out = SrcId::SD;
  else if (!strcmp(s, "usb")) out = SrcId::USB;
  else if (!strcmp(s, "off")) out = SrcId::Off;
  else return false;
  return true;
}

}  // namespace

void Router::init(QueueHandle_t q) { s_q = q; }

bool Router::post(const Cmd& c) {
  if (!s_q || xQueueSend(s_q, &c, 0) != pdTRUE) {
    LOGW(TAG, "черга команд переповнена, команду відкинуто");
    return false;
  }
  return true;
}

void Router::dispatch(char* body, Origin o) {
  using proto::CmdId;
  proto::Msg m;
  if (!proto::splitBody(body, m)) return;
  LOGD(TAG, "%s: %s (арг=%u)", o == Origin::Link ? "S3" : "UART0", m.name, (unsigned)m.argc);

  if (!strcmp(m.name, "DBG")) {  // лише з UART0
    if (o == Origin::Console) DebugCmds::handle(m);
    else Events::err(proto::errc::kUnknownCmd, m.name);
    return;
  }

  const proto::CmdDef* d = nullptr;
  for (size_t i = 0; i < proto::kCmdCount; i++) {
    if (!strcmp(proto::kCmds[i].name, m.name)) { d = &proto::kCmds[i]; break; }
  }
  if (!d) { Events::err(proto::errc::kUnknownCmd, m.name); return; }
  if (d->id == CmdId::List || d->id == CmdId::PlayFile) {
    Events::err(proto::errc::kNotImplemented);
    return;
  }
  if (m.tooMany || m.argc < d->minArgs || m.argc > d->maxArgs) {
    Events::err(proto::errc::kBadArg, d->name);
    return;
  }

  Cmd c{d->id, 0, o};
  long v = 0;
  switch (d->id) {
    case CmdId::Hb:
      return;  // лише ознака живості (час останнього валідного кадру оновлює link)
    case CmdId::Hello:
      if (!parseInt(m.args[0], v)) {  // нечислова / порожня версія
        Events::err(proto::errc::kBadArg, d->name);
        return;
      }
      if (v != kProtoVer) {  // M5c4b/Q2: чужа версія — лише READY (S3 бачить proto у READY); без стану й ERR
        LOGW(TAG, "HELLO,%ld: proto_ver не збігається (наша %d), лише READY", v, kProtoVer);
        Events::ready();
        return;
      }
      c.arg = (int16_t)v;
      break;
    case CmdId::Src: {
      SrcId s;
      if (!parseSrc(m.args[0], s)) { Events::err(proto::errc::kBadArg, d->name); return; }
      c.arg = (int16_t)s;
      break;
    }
    case CmdId::Shuf:
      if (!parseInt(m.args[0], v) || (v != 0 && v != 1)) { Events::err(proto::errc::kBadArg, d->name); return; }
      c.arg = (int16_t)v;
      break;
    case CmdId::Rep:
      if (!parseInt(m.args[0], v) || v < 0 || v > 2) { Events::err(proto::errc::kBadArg, d->name); return; }
      c.arg = (int16_t)v;
      break;
    default:
      break;
  }
  post(c);
}

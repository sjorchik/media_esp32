#include "core/events.h"

#include "config.h"
#include "proto/frame.h"
#include "proto/link.h"
#include "proto/proto_table.h"

using proto::Arg;

namespace {
void emit(const char* name, const Arg* a, size_t n, Link::Pri pri) {
  char buf[proto::kMaxFrame + 1];
  size_t len = proto::encodeFrame(buf, sizeof buf, name, a, n);
  if (len) Link::send(buf, len, pri);
}
}  // namespace

void Events::ready() {
  Arg a[] = {Arg::T(FW_VERSION), Arg::N(kProtoVer)};
  emit(proto::ev::kReady, a, 2, Link::Pri::Hi);
}

void Events::state(SrcId src, PlayState st, bool shuf, uint8_t rep) {
  Arg a[] = {Arg::S(srcName(src)), Arg::S(stateName(st)), Arg::N(shuf ? 1 : 0), Arg::N(rep)};
  emit(proto::ev::kState, a, 4, Link::Pri::Hi);
}

void Events::media(bool sd, bool usb) {
  Arg a[] = {Arg::N(sd ? 1 : 0), Arg::N(usb ? 1 : 0)};
  emit(proto::ev::kMedia, a, 2, Link::Pri::Hi);
}

void Events::bt(bool connected, const char* name) {
  Arg a[] = {Arg::S(connected ? "connected" : "disconnected"), Arg::T(name)};
  emit(proto::ev::kBt, a, 2, Link::Pri::Hi);
}

void Events::meta(const char* artist, const char* title, const char* album) {
  Arg a[] = {Arg::T(artist), Arg::T(title), Arg::T(album)};
  emit(proto::ev::kMeta, a, 3, Link::Pri::Lo);
}

void Events::track(uint16_t n, uint16_t total) {
  Arg a[] = {Arg::N(n), Arg::N(total)};
  emit(proto::ev::kTrack, a, 2, Link::Pri::Lo);
}

void Events::folder(const char* name, uint16_t count) {
  Arg a[] = {Arg::T(name), Arg::N(count)};
  emit(proto::ev::kFolder, a, 2, Link::Pri::Lo);
}

void Events::position(uint32_t posS, uint32_t durS) {
  Arg a[] = {Arg::N((long)posS), Arg::N((long)durS)};
  emit(proto::ev::kTime, a, 2, Link::Pri::Time);
}

void Events::err(const char* code, const char* detail) {
  Arg a[] = {Arg::S(code), Arg::T(detail)};
  emit(proto::ev::kErr, a, (detail && *detail) ? 2 : 1, Link::Pri::Hi);
}
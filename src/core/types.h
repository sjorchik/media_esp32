#pragma once
#include <stdint.h>

#include "proto/proto_table.h"

enum class SrcId : uint8_t { Off = 0, BT = 1, SD = 2, USB = 3 };
enum class PlayState : uint8_t { Idle, Scan, Play, Pause, Stop };
enum class Res : uint8_t { Ok, BadState, BadArg, NoMedia, NotImpl, ReadFail, DecodeFail, BtFail };

inline const char* srcName(SrcId s) {
  switch (s) {
    case SrcId::BT: return "bt";
    case SrcId::SD: return "sd";
    case SrcId::USB: return "usb";
    default: return "off";
  }
}

inline const char* stateName(PlayState s) {
  switch (s) {
    case PlayState::Scan: return "scan";
    case PlayState::Play: return "play";
    case PlayState::Pause: return "pause";
    case PlayState::Stop: return "stop";
    default: return "idle";
  }
}

inline const char* resCode(Res r) {
  switch (r) {
    case Res::BadState: return proto::errc::kBadState;
    case Res::BadArg: return proto::errc::kBadArg;
    case Res::NoMedia: return proto::errc::kNoMedia;
    case Res::NotImpl: return proto::errc::kNotImplemented;
    case Res::ReadFail: return proto::errc::kReadFail;
    case Res::DecodeFail: return proto::errc::kDecodeFail;
    case Res::BtFail: return proto::errc::kBtFail;
    default: return "";
  }
}
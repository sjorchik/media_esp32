#pragma once
#include <stddef.h>
#include <stdint.h>

// Таблиця команд і подій контракту proto_ver=1. Змінювати лише разом із контрактом.
namespace proto {

enum class CmdId : uint8_t {
  Hello, Get, Src, Play, Pause, Toggle, Stop, Next, Prev, FNext, FPrev,
  Shuf, Rep, Hb, List, PlayFile,
  DbgMedia   // ВНУТРІШНЯ (не з контракту, немає в kCmds): імітація hot-plug з DBG
};

struct CmdDef { const char* name; int8_t minArgs; int8_t maxArgs; CmdId id; };

inline constexpr CmdDef kCmds[] = {
  {"HELLO",    1, 1, CmdId::Hello},
  {"GET",      0, 0, CmdId::Get},
  {"SRC",      1, 1, CmdId::Src},
  {"PLAY",     0, 0, CmdId::Play},
  {"PAUSE",    0, 0, CmdId::Pause},
  {"TOGGLE",   0, 0, CmdId::Toggle},
  {"STOP",     0, 0, CmdId::Stop},
  {"NEXT",     0, 0, CmdId::Next},
  {"PREV",     0, 0, CmdId::Prev},
  {"FNEXT",    0, 0, CmdId::FNext},
  {"FPREV",    0, 0, CmdId::FPrev},
  {"SHUF",     1, 1, CmdId::Shuf},
  {"REP",      1, 1, CmdId::Rep},
  {"HB",       1, 1, CmdId::Hb},
  {"LIST",     0, 8, CmdId::List},      // зарезервовано → ERR,not_implemented
  {"PLAYFILE", 0, 8, CmdId::PlayFile},  // зарезервовано → ERR,not_implemented
};
inline constexpr size_t kCmdCount = sizeof(kCmds) / sizeof(kCmds[0]);

inline const char* cmdName(CmdId id) {
  for (size_t i = 0; i < kCmdCount; i++) if (kCmds[i].id == id) return kCmds[i].name;
  return "?";
}

namespace ev {  // події WROOM → S3
inline constexpr const char* kReady  = "READY";
inline constexpr const char* kState  = "STATE";
inline constexpr const char* kMedia  = "MEDIA";
inline constexpr const char* kBt     = "BT";
inline constexpr const char* kMeta   = "META";
inline constexpr const char* kTrack  = "TRACK";
inline constexpr const char* kFolder = "FOLDER";
inline constexpr const char* kTime   = "TIME";
inline constexpr const char* kErr    = "ERR";
inline constexpr const char* kHb     = "HB";
}  // namespace ev

namespace errc {  // коди ERR
inline constexpr const char* kUnknownCmd    = "unknown_cmd";
inline constexpr const char* kBadArg        = "bad_arg";
inline constexpr const char* kBadState      = "bad_state";
inline constexpr const char* kNoMedia       = "no_media";
inline constexpr const char* kReadFail      = "read_fail";
inline constexpr const char* kDecodeFail    = "decode_fail";
inline constexpr const char* kTooManyFiles  = "too_many_files";
inline constexpr const char* kNotImplemented = "not_implemented";
inline constexpr const char* kBtFail        = "bt_fail";
}  // namespace errc

}  // namespace proto
#include "audio/pcm_arena.h"

#include "util/log.h"

alignas(16) uint8_t g_pcmArena[kPcmArenaBytes];

namespace {
constexpr const char* TAG = "arena";
volatile PcmArena::Owner s_owner = PcmArena::Owner::None;
}  // namespace

PcmArena::Owner PcmArena::owner() { return s_owner; }

const char* PcmArena::ownerName(Owner o) {
  switch (o) {
    case Owner::Bt: return "bt";
    case Owner::Sd: return "sd";
    default: return "none";
  }
}

bool PcmArena::acquire(Owner o) {
  Owner cur = s_owner;
  if (cur != Owner::None && cur != o) {
    LOGE(TAG, "область зайнята (%s), запит %s відхилено", ownerName(cur), ownerName(o));
    return false;
  }
  s_owner = o;
  return true;
}

void PcmArena::release(Owner o) {
  if (s_owner != o) {
    LOGW(TAG, "release(%s), але власник %s", ownerName(o), ownerName(s_owner));
    return;
  }
  s_owner = Owner::None;
}

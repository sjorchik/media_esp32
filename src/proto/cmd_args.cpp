#include "proto/cmd_args.h"

#include <string.h>

#include "audio/eq.h"
#include "audio/level.h"
#include "config.h"

namespace proto {

namespace {
// Контракт фіксує 5 смуг і 3 слоти; зміна кількості = нова proto_ver.
static_assert(eq_cfg::kBandCount == 5, "EQ у контракті: рівно 5 аргументів");
static_assert(level::kSlots == 3, "LVL у контракті: bt|sd|usb");
constexpr const char* kSlotNames[level::kSlots] = {"bt", "sd", "usb"};  // порядок = level::Slot
}  // namespace

bool parseUInt(const char* s, long& v) {
  if (!s || !*s) return false;
  v = 0;
  for (const char* p = s; *p; p++) {
    if (*p < '0' || *p > '9') return false;
    v = v * 10 + (*p - '0');
    if (v > 100000) return false;  // захист від переповнення: усі валідні аргументи далеко менші
  }
  return true;
}

bool parseSInt(const char* s, long& v) {
  if (!s) return false;
  const bool neg = (*s == '-');
  long a = 0;
  if (!parseUInt(neg ? s + 1 : s, a)) return false;  // "-", "--1", "+1", " 1" → false
  v = neg ? -a : a;
  return true;
}

HelloVerdict helloVerdict(const char* arg, int ourVer, long& ver) {
  if (!parseUInt(arg, ver)) return HelloVerdict::BadArg;
  return ver == ourVer ? HelloVerdict::Full : HelloVerdict::ReadyOnly;
}

bool applyEq(const Msg& m) {
  constexpr uint8_t N = eq_cfg::kBandCount;
  if (m.tooMany || m.argc != N) return false;
  int8_t g[N];
  for (uint8_t b = 0; b < N; b++) {  // спершу перевіряємо всі п'ять, лише потім застосовуємо
    long v = 0;
    if (!parseSInt(m.args[b], v) || v < eq_cfg::kGainMinDb || v > eq_cfg::kGainMaxDb) return false;
    g[b] = (int8_t)v;
  }
  eq::setAllDb(g);
  return true;
}

bool applyLvl(const Msg& m) {
  if (m.tooMany || m.argc != 2) return false;
  int slot = -1;
  for (int i = 0; i < (int)level::kSlots; i++)
    if (!strcmp(m.args[0], kSlotNames[i])) slot = i;  // регістр значущий, як і в SRC
  long db = 0;
  if (slot < 0 || !parseSInt(m.args[1], db) || db < kLevelMinDb || db > kLevelMaxDb) return false;
  level::setDb((level::Slot)slot, (int8_t)db);
  return true;
}

}  // namespace proto

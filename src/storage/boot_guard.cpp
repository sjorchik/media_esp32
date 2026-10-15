#include "storage/boot_guard.h"

BootLogic::Decision BootLogic::onBoot(Reason r, uint8_t count, uint8_t threshold) {
  Decision d{0, false, false};
  if (r == Reason::Crash) {
    d.count = (count == 255) ? (uint8_t)255 : (uint8_t)(count + 1);
    d.protect = threshold != 0 && d.count >= threshold;
  }
  d.write = d.count != count;
  return d;
}

BootLogic::Decision BootLogic::onStable(uint8_t count) { return Decision{0, false, count != 0}; }

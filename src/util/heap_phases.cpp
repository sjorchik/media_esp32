#include "util/heap_phases.h"

#ifdef ARDUINO
#include <esp_heap_caps.h>
#include <esp_system.h>

#include "util/log.h"
#endif

namespace {
HeapPhase::Rec s_rec[HeapPhase::kN] = {};
bool s_init = false;

void ensureInit() {
  if (s_init) return;
  for (auto& r : s_rec) r = HeapPhase::Rec{0, 0xFFFFFFFFu, 0xFFFFFFFFu};
  s_init = true;
}
}  // namespace

const char* HeapPhase::name(Phase p) {
  switch (p) {
    case SdAct: return "sd-act";
    case BtAct: return "bt-act";
    case UsbAct: return "usb-act";
    case UsbDrv: return "usb-drv";
    case UsbVol: return "usb-vol";
    case ScanStart: return "scan-start";
    case ScanPeak: return "scan-peak";
    case ScanEnd: return "scan-end";
    case Play: return "play";
    case Deact: return "deact";
    case UsbClose: return "usb-close";
    default: return "?";
  }
}

void HeapPhase::record(Phase p, uint32_t freeB, uint32_t largestB) {
  if (p >= kN) return;
  ensureInit();
  Rec& r = s_rec[p];
  r.n++;
  if (freeB < r.minFree) r.minFree = freeB;
  if (largestB < r.minLargest) r.minLargest = largestB;
}

const HeapPhase::Rec& HeapPhase::get(Phase p) {
  ensureInit();
  return s_rec[p < kN ? p : 0];
}

void HeapPhase::reset() {
  s_init = false;
  ensureInit();
}

#ifdef ARDUINO
void HeapPhase::mark(Phase p) {
  record(p, (uint32_t)heap_caps_get_free_size(MALLOC_CAP_8BIT), (uint32_t)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
}

void HeapPhase::print() {
  ensureInit();
  LOGI("heap", "фази: мінімум free/largest від останнього скидання; глобальний min_free (від старту) = %u", (unsigned)esp_get_minimum_free_heap_size());
  for (int i = 0; i < kN; i++) {
    const Rec& r = s_rec[i];
    if (!r.n) LOGI("heap", "  %-10s замірів 0", name((Phase)i));
    else LOGI("heap", "  %-10s замірів %u, мін. free %u, мін. largest %u", name((Phase)i), (unsigned)r.n, (unsigned)r.minFree, (unsigned)r.minLargest);
  }
}
#endif

#include "usb/usb_presence.h"

#if ENABLE_USB_CH376

#include <Arduino.h>
#include <esp_heap_caps.h>

#include "usb/ch376_uart.h"
#include "usb/usb_chip_uart.h"
#include "util/log.h"

namespace {
constexpr const char* TAG = "usbpres";
UsbPresenceCore& core() {
  static UsbPresenceCore c(uartChip(), USB_PRESENCE_MODE);  // ≈ 0,1 КБ статичної памʼяті (ліміт M4c2 — 0,3 КБ у сумі)
  return c;
}
volatile bool s_manual = false;
UsbPres s_last = UsbPres::Unknown;
uint32_t s_heapStart = 0;  // free перед початком послідовності обміну (діагностика витоку)
uint16_t s_lastFails = 0;

void logInitFail() {
  const UsbPresenceCore::Stats& st = core().stats();
  if (st.initFails == s_lastFails) return;
  s_lastFails = st.initFails;
  LOGW(TAG, "ініціалізація чіпа не вдалась на кроці «%s» (%s); спроба %u, повтор через %u мс", core().lastFail(), ch376ErrName(core().lastFailErr()),
       (unsigned)st.initFails, (unsigned)core().lastFailReinitMs());
}

void logChange() {
  const UsbPres now = core().state();
  if (now == s_last) return;
  LOGI(TAG, "присутність флешки: %s → %s", usbPresName(s_last), usbPresName(now));
  s_last = now;
}
}  // namespace

namespace UsbPresence {

void begin(uint32_t nowMs) {
  core().begin(nowMs);
  LOGI(TAG, "режим %d (1 = паркування + INT#, 2 = опитування %u мс, 0 = вимкнено); ініціалізація чіпа через %u мс після старту", (int)USB_PRESENCE_MODE,
       (unsigned)kUsbPresPollMs, (unsigned)kUsbPresInitDelayMs);
}

void tick(uint32_t nowMs, bool btActive, uint32_t freeHeap) {
  if (USB_PRESENCE_MODE == 0 || s_manual) return;
  UsbPresenceCore& c = core();
  UsbPresEnv e;
  e.now = nowMs;
  e.defer = btActive && (USB_PRESENCE_MODE == 2 || freeHeap < kUsbPresHeapOkBt);
  e.portBusy = ch376_uart::started() && !c.busy();
  const uint32_t t0 = micros();
  const bool wasBusy = c.busy();
  if (!wasBusy) s_heapStart = (uint32_t)heap_caps_get_free_size(MALLOC_CAP_8BIT);
  c.tick(e);
  if (wasBusy && !c.busy()) {  // послідовність (ініціалізація або перевірка) завершилась: порт запаркований
    const uint32_t now = (uint32_t)heap_caps_get_free_size(MALLOC_CAP_8BIT);
    LOGI(TAG, "послідовність завершено: heap free %u → %u Б (%+d)", (unsigned)s_heapStart, (unsigned)now, (int)now - (int)s_heapStart);
  }
  const uint32_t dt = (micros() - t0) / 1000u;
  if ((wasBusy || c.busy()) && dt > kUsbPresStepWarnMs)
    LOGW(TAG, "крок перевірки %u мс (> %u мс; ініціалізація/probe/узгодження швидкості можуть бути довшими)", (unsigned)dt, (unsigned)kUsbPresStepWarnMs);
  logChange();
  logInitFail();
}

UsbPres state() { return core().state(); }
UsbPres stateForOpen() { return core().stateForOpen(); }
bool takeChange(bool& flag) { return core().takeChange(flag); }
void yield() { core().yield(); }
void sessionOpened() { core().sessionOpened(); logChange(); }
void openFailed(UsbOpenFail f) { core().openFailed(f); logChange(); }
void volumeLost() { core().volumeLost(); logChange(); }
void sessionClosed(bool recheck) { core().sessionClosed(recheck); logChange(); }
void invalidate() { core().invalidate(millis()); logChange(); }
void restartInit() { core().restartInit(millis()); }
void setManual(bool on) { s_manual = on; }
bool manual() { return s_manual; }

void dbgPrint() {
  UsbPresenceCore& c = core();
  const UsbPresenceCore::Stats& s = c.stats();
  LOGI(TAG, "стан=%s режим=%d ручний=%d порт=%d чіп: host=%d швидкість=%u INT#=%d фронтів=%u", usbPresName(c.state()), (int)USB_PRESENCE_MODE, s_manual ? 1 : 0,
       ch376_uart::started() ? 1 : 0, ch376_uart::driver().host() ? 1 : 0, (unsigned)ch376_uart::driver().baud(), uartChip().intLow() ? 0 : 1,
       (unsigned)uartChip().intEdges());
  LOGI(TAG, "подій=%u перевірок(probe)=%u повторів=%u відкладень(BT)=%u ініціалізацій=%u (невдалих %u, остання: «%s»/%s) очікує ініціалізації=%d",
       (unsigned)s.events, (unsigned)s.probes, (unsigned)s.retries, (unsigned)s.defers, (unsigned)s.inits, (unsigned)s.initFails, c.lastFail(),
       ch376ErrName(c.lastFailErr()), c.needInit() ? 1 : 0);
}

}  // namespace UsbPresence

#endif  // ENABLE_USB_CH376

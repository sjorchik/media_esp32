#include "core/device_state.h"

#include <Arduino.h>
#include <string.h>

#include "config.h"

namespace {
portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
bool s_sd = false, s_usb = false, s_bt = false;
char s_btName[33] = {0};
}  // namespace

void DeviceState::init() {
  portENTER_CRITICAL(&s_mux);
  s_sd = STUB_SD_PRESENT;
  s_usb = STUB_USB_PRESENT;
  s_bt = false;
  s_btName[0] = 0;
  portEXIT_CRITICAL(&s_mux);
}

bool DeviceState::sd() { return s_sd; }
bool DeviceState::usb() { return s_usb; }

void DeviceState::setMedia(bool sd, bool usb) {
  portENTER_CRITICAL(&s_mux);
  s_sd = sd;
  s_usb = usb;
  portEXIT_CRITICAL(&s_mux);
}

void DeviceState::setSd(bool sd) {
  portENTER_CRITICAL(&s_mux);
  s_sd = sd;
  portEXIT_CRITICAL(&s_mux);
}

bool DeviceState::btConnected() { return s_bt; }

void DeviceState::btName(char* out, size_t cap) {
  if (!cap) return;
  portENTER_CRITICAL(&s_mux);
  strncpy(out, s_btName, cap - 1);
  out[cap - 1] = 0;
  portEXIT_CRITICAL(&s_mux);
}

void DeviceState::setBt(bool connected, const char* name) {
  portENTER_CRITICAL(&s_mux);
  s_bt = connected;
  strncpy(s_btName, name ? name : "", sizeof s_btName - 1);
  s_btName[sizeof s_btName - 1] = 0;
  portEXIT_CRITICAL(&s_mux);
}

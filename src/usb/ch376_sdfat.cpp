#include "usb/ch376_sdfat.h"

#if ENABLE_USB_CH376

bool Ch376SdDevice::isBusy() { return false; }

bool Ch376SdDevice::readSector(uint32_t sector, uint8_t* dst) { return c_.readSector(sector, dst); }

bool Ch376SdDevice::readSectors(uint32_t sector, uint8_t* dst, size_t ns) { return c_.readSectors(sector, dst, ns); }

uint32_t Ch376SdDevice::sectorCount() { return c_.sectorCount(); }

bool Ch376SdDevice::syncDevice() { return true; }

bool Ch376SdDevice::writeSector(uint32_t, const uint8_t*) {
  wr_++;
  return false;
}

bool Ch376SdDevice::writeSectors(uint32_t, const uint8_t*, size_t) {
  wr_++;
  return false;
}

#endif  // ENABLE_USB_CH376

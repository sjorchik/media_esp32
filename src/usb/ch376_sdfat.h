#pragma once
#include "config.h"

#if ENABLE_USB_CH376

#include "SdFat.h"
#include "usb/ch376_blockdev.h"

// Обгортка SdFat над ядром блочного пристрою (M4b3). Лише Arduino-збірка з ENABLE_USB_CH376=1.
// Базовий клас — FsBlockDeviceInterface зі встановленого SdFat (common/FsBlockDeviceInterface.h, SdFat 2.2.3).
// Сектор 0 пристрою = початок розділу, тож том монтується як «без таблиці розділів»: FsVolume::begin(&dev, false, 0, 0).
// Запис не підтримується: методи запису повертають false (лічильник спроб — writeAttempts()).
#if !USE_BLOCK_DEVICE_INTERFACE
#error "USB-том потребує -DUSE_BLOCK_DEVICE_INTERFACE=1 (platformio.ini, лише env:esp32dev_usb): інакше FsVolume::begin приймає тільки SdCard*"
#endif

class Ch376SdDevice final : public FsBlockDeviceInterface {
 public:
  explicit Ch376SdDevice(Ch376BlockDev& core) : c_(core) {}

  bool isBusy() override;
  bool readSector(uint32_t sector, uint8_t* dst) override;
  bool readSectors(uint32_t sector, uint8_t* dst, size_t ns) override;
  uint32_t sectorCount() override;  // розмір розділу
  bool syncDevice() override;
  bool writeSector(uint32_t sector, const uint8_t* src) override;
  bool writeSectors(uint32_t sector, const uint8_t* src, size_t ns) override;

  uint32_t writeAttempts() const { return wr_; }

 private:
  Ch376BlockDev& c_;
  uint32_t wr_ = 0;
};

#endif  // ENABLE_USB_CH376

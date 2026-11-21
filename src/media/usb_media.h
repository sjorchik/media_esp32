#pragma once
#include "config.h"

#if ENABLE_USB_CH376

#include "media/imedia.h"
#include "usb/usb_volume.h"

// M4b4. IMedia для USB-флешки (CH376S + SdFat): тонка обгортка над UsbVolume. Увесь SdFat і UART чіпа — у usb/usb_volume.cpp
// під одним мʼютексом (скан в app, читання в задачі mp3, DBG у консолі не стикаються).
// Відмінності від SD: том відкриває/закриває джерело (mount() при активації, unmount() при виході/виніманні);
// виявлення вставлення, коли USB неактивний, — окреме завдання M4c, тож poll() лише помічає зникнення відкритого тому.
class UsbMedia final : public IMedia {
 public:
  bool begin() override { return true; }
  // Removed — коли відкритий том перестав відповідати (помилка читання Disconnected); Inserted не видається ніколи.
  MediaEvent poll(uint32_t nowMs) override;
  bool mount() override;      // UsbVolume::open(): 2 Мбод із самоперевіркою, відкат 1 Мбод; false — причина в логу й lastFail()
  void unmount() override;    // закрити файл/том/драйвер (ідемпотентно)
  // M4c2: відкриття по кроках (UsbVolume::openBegin/openStep/openAbort); mount() лишається блокуючим (DBG,usb,mount).
  bool mountBegin() override;
  MountPoll mountStep(uint32_t nowMs) override;
  void mountAbort() override;
  bool mounting() const override { return UsbVolume::get().opening(); }
  bool mounted() const override { return mounted_; }
  bool present() const override { return mounted_; }
  int openDir(const char* path) override;
  int nextEntry(int h, MediaEntry& e) override;
  void closeDir(int h) override;
  IFileReader& reader() override { return UsbVolume::get().reader(); }
  void info(MediaInfo& out) override;
  uint32_t volumeId() const override { return serial_; }
  void scanProfile(int what, uint32_t wallMs = 0, uint32_t busyMs = 0) override;
  void scanBegin() override;  // M4c1: кеш секторів (heap) + пропуск getName() для не-MP3
  void scanEnd() override;    // M4c1: знімок профілю, звільнення кеша (ідемпотентно)
  void ioLock() override;
  void ioUnlock() override;

  UsbOpenFail lastFail() const { return fail_; }
  uint32_t baud() const { return baud_; }
  uint32_t openMs() const { return openMs_; }

 private:
  bool mounted_ = false;
  UsbOpenFail fail_ = UsbOpenFail::None;
  uint32_t serial_ = 0, baud_ = 0, openMs_ = 0;
  uint8_t fsType_ = 0;
  uint32_t partSectors_ = 0;
  bool locked_ = false;
};

#endif  // ENABLE_USB_CH376



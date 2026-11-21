#include "usb/usb_chip_uart.h"

#if ENABLE_USB_CH376

#include <Arduino.h>
#include <stdlib.h>

#include <new>

#include "pins.h"
#include "usb/ch376_disk.h"
#include "usb/ch376_uart.h"
#include "usb/disk_probe.h"

namespace {

// Швидкість для самого чіпа: baud != 0 — одна пара з таблиці; 0 — перебір kCh376NegotiateTable (діагностика).
Ch376Err negotiate(Ch376& d, uint32_t baud) {
  Ch376BaudDiv one{0, 0, 0};
  const Ch376BaudDiv* tab = kCh376NegotiateTable;
  size_t n = kCh376NegotiateN;
  if (baud != 0) {
    for (size_t i = 0; i < kCh376BaudTableN; i++)
      if (kCh376BaudTable[i].baud == baud) one = kCh376BaudTable[i];
    if (one.baud == 0) return Ch376Err::BadArg;
    tab = &one;
    n = 1;
  }
  Ch376Attempt att[kCh376NegotiateN];
  size_t nAtt = 0;
  return d.negotiateBaud(tab, n, att, kCh376NegotiateN, nAtt);
}

// Адаптер для usbPickPartition: лише читання секторів через Ch376Disk; решта методів IUsbOpenHw тут не потрібна.
class ProbeHw final : public IUsbOpenHw {
 public:
  explicit ProbeHw(Ch376Disk& d) : d_(d) {}
  bool portBegin() override { return true; }
  Ch376Err bringUp(uint32_t) override { return Ch376Err::Ok; }
  uint32_t baud() const override { return ch376_uart::driver().baud(); }
  DiskErr capacity(uint32_t& s) override { return d_.capacity(s); }
  DiskErr readSector(uint32_t lba, uint8_t* dst) override { return d_.readSectors(lba, 1, dst); }
  void portEnd() override {}

 private:
  Ch376Disk& d_;
};

class UartChip final : public IUsbChip {
 public:
  bool hasInt() const override { return CH_INT_PIN >= 0; }
  bool intLow() const override { return CH_INT_PIN >= 0 && digitalRead(CH_INT_PIN) == LOW; }
  uint32_t intEdges() const override { return ch376_uart::intEdges(); }

  bool portWake() override { return ch376_uart::begin(); }
  void portPark() override { ch376_uart::park(); }
  bool portOpen() const override { return ch376_uart::started(); }
  uint32_t chipBaud() const override { return ch376_uart::driver().baud(); }
  bool chipHost() const override { return ch376_uart::driver().host(); }

  Ch376Err ping() override { return ch376_uart::driver().checkExist(); }
  Ch376Err recover() override { return ch376_uart::driver().rollback(kCh376DefaultBaud); }
  Ch376Err resetSend() override { return ch376_uart::driver().resetSend(); }
  Ch376Err resetFinish() override { return ch376_uart::driver().resetFinish(); }
  Ch376Err setBaud(uint32_t baud) override { return negotiate(ch376_uart::driver(), baud); }
  Ch376Err setHost() override { return ch376_uart::driver().setUsbMode(kCh376ModeHost); }
  Ch376Err status(uint8_t& st) override { return ch376_uart::driver().getStatus(st); }
  Ch376Err diskConnectSend() override { return ch376_uart::driver().sendDiskConnect(); }
  Ch376Err diskMountSend() override { return ch376_uart::driver().sendDiskMount(); }

  // Ємність + probeDisk: буфери (≈0,9 КБ) лише на час виклику; Ch376Disk — на стеку (без heap).
  UsbOpenFail probe() override {
    uint8_t* scratch = static_cast<uint8_t*>(malloc(kCh376DiskSecBytes));
    ProbeReport* pr = new (std::nothrow) ProbeReport;
    UsbOpenInfo* info = new (std::nothrow) UsbOpenInfo;
    UsbOpenFail f = UsbOpenFail::NoMemory;
    if (scratch && pr && info) {
      Ch376Disk disk(ch376_uart::link());
      disk.reset();
      disk.setSecPerCmd(kCh376SectorsPerCmd);
      ProbeHw hw(disk);
      uint32_t cap = 0;
      const DiskErr d = disk.capacity(cap);
      if (d != DiskErr::Ok || cap == 0) {
        f = (d == DiskErr::Disconnected || d == DiskErr::NoDisk) ? UsbOpenFail::NoMedia : UsbOpenFail::ReadError;
      } else {
        info->capacity = cap;
        f = usbPickPartition(hw, scratch, *pr, *info);
      }
    }
    delete info;
    delete pr;
    free(scratch);
    return f;
  }
};

}  // namespace

IUsbChip& uartChip() {
  static UartChip c;
  return c;
}

#endif  // ENABLE_USB_CH376

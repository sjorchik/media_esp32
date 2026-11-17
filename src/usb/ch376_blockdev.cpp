#include "usb/ch376_blockdev.h"

#include "config.h"

#if ENABLE_USB_CH376

void Ch376BlockDev::setPartition(uint32_t lbaOffset, uint32_t sectors) {
  off_ = lbaOffset;
  const uint64_t room = 0x100000000ull - (uint64_t)lbaOffset;  // LBA тримається в 32 бітах
  sectors_ = (uint64_t)sectors > room - 1u ? (uint32_t)(room - 1u) : sectors;
  gone_ = false;
  err_ = DiskErr::Ok;
  st_ = BlockDevStats{};
  d_.setSecPerCmd(kCh376SectorsPerCmd);
}

void Ch376BlockDev::markGone(DiskErr e) {
  if (!gone_) {
    gone_ = true;
    err_ = e == DiskErr::Ok ? DiskErr::Disconnected : e;
  }
}

bool Ch376BlockDev::fail(DiskErr e) {
  st_.errors++;
  markGone(e);
  return false;
}

bool Ch376BlockDev::readSector(uint32_t sector, uint8_t* dst) { return readSectors(sector, dst, 1); }

bool Ch376BlockDev::readSectors(uint32_t sector, uint8_t* dst, size_t n) {
  if (gone_) {
    st_.rejected++;
    return false;
  }
  if (!dst || n == 0 || sector >= sectors_ || n > (size_t)(sectors_ - sector)) {
    st_.rangeErrors++;
    return false;
  }
  st_.calls++;
  size_t done = 0;
  while (done < n) {
    const size_t left = n - done;
    const uint32_t k = left < kCh376SectorsPerCmd ? (uint32_t)left : kCh376SectorsPerCmd;
    st_.cmds++;
    const DiskErr e = d_.readSectors(off_ + sector + (uint32_t)done, k, dst + done * kCh376DiskSecBytes);
    if (e != DiskErr::Ok) {
      if (!isDeviceFailure(e)) {  // BadArg з Ch376Disk (за ємністю): помилка аргументів, не збій пристрою
        st_.rangeErrors++;
        return false;
      }
      return fail(e);
    }
    done += k;
  }
  st_.sectors += (uint32_t)n;
  return true;
}

#endif  // ENABLE_USB_CH376

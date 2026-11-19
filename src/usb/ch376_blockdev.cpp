#include "usb/ch376_blockdev.h"

#include "config.h"

#if ENABLE_USB_CH376

#include <string.h>

#include <new>

void Ch376BlockDev::setPartition(uint32_t lbaOffset, uint32_t sectors) {
  off_ = lbaOffset;
  const uint64_t room = 0x100000000ull - (uint64_t)lbaOffset;  // LBA тримається в 32 бітах
  sectors_ = (uint64_t)sectors > room - 1u ? (uint32_t)(room - 1u) : sectors;
  gone_ = false;
  err_ = DiskErr::Ok;
  st_ = BlockDevStats{};
  if (cache_) cache_->invalidate();  // новий том/перемонтування: старі сектори недійсні
  d_.setSecPerCmd(kCh376SectorsPerCmd);
}

void Ch376BlockDev::markGone(DiskErr e) {
  if (!gone_) {
    gone_ = true;
    err_ = e == DiskErr::Ok ? DiskErr::Disconnected : e;
    if (cache_) cache_->invalidate();  // носій зник: кеш недійсний
  }
}

bool Ch376BlockDev::cacheOpen(uint8_t slots, uint8_t readAhead) {
  cacheClose();
  SectorCache* c = new (std::nothrow) SectorCache;
  if (!c) return false;
  if (!c->open(slots, readAhead)) {
    delete c;
    return false;
  }
  cache_ = c;
  return true;
}

bool Ch376BlockDev::cacheResize(uint8_t slots, uint8_t readAhead) {
  SectorCache* c = new (std::nothrow) SectorCache;
  if (!c) return false;
  if (!c->open(slots, readAhead)) {
    delete c;
    return false;
  }
  if (cache_) c->restoreStats(cache_->stats());
  delete cache_;
  cache_ = c;
  return true;
}

void Ch376BlockDev::cacheClose() {
  delete cache_;
  cache_ = nullptr;
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
  if (cache_ && n == 1) return cachedRead(sector, dst);
  return fetch(sector, dst, n);
}

bool Ch376BlockDev::cachedRead(uint32_t sector, uint8_t* dst) {
  SectorCache& c = *cache_;
  c.note(sector);
  if (!c.slots()) return fetch(sector, dst, 1);  // режим «тінь»: лише лічильники
  const int hit = c.find(sector);
  if (hit >= 0) {
    memcpy(dst, c.slot(hit), kCh376DiskSecBytes);
    c.hit(hit);
    return true;
  }
  c.miss();
  const uint32_t k = c.planFetch(sector, sectors_);
  const int first = c.victimRun(k);
  if (!fetch(sector, c.slot(first), k)) return false;  // збій чіпа → Gone (липко); слоти не позначаємо
  c.fill(first, sector, k);
  memcpy(dst, c.slot(first), kCh376DiskSecBytes);
  return true;
}

bool Ch376BlockDev::fetch(uint32_t sector, uint8_t* dst, size_t n) {
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


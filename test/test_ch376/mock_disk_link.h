#pragma once
#include <stdint.h>
#include <string.h>

#include <vector>

#include "config.h"
#include "usb/ch376_link.h"

// Mock CH376S для сирого доступу до секторів (M4b2): образ диска в памʼяті, команди GET_STATUS (0x22),
// DISK_CAPACITY (0x3E), DISK_READ (0x54), DISK_RD_GO (0x55), RD_USB_DATA0 (0x27). Час керований (readByte/yield/
// waitIntLow його зсувають); INT# = «є видима подія». Збої задаються номером блоку (з 1, у межах сеансу mock-а).
inline uint8_t diskPattern(uint32_t off) { return (uint8_t)(off * 131u + (off >> 9) * 17u + (off >> 3) + 5u); }

class MockDiskLink : public Ch376Link {
 public:
  explicit MockDiskLink(uint32_t sectors) : img_((size_t)sectors * 512u) {
    for (size_t i = 0; i < img_.size(); i++) img_[i] = diskPattern((uint32_t)i);
  }

  std::vector<uint8_t>& image() { return img_; }
  size_t sectorCount() const { return img_.size() / 512u; }

  // Налаштування
  bool intPin = true;
  uint32_t byteUs = 0;       // byteTimeUs()
  uint32_t latencyUs = 0;    // затримка події після команди
  bool abortFlag = false;    // abortRequested()
  bool silentStatus = false; // GET_STATUS не відповідає
  // Збої (номер блоку з 1; -1 — немає)
  int ejectAtBlock = -1;     // подія 0x16, далі всі команди → 0x82
  int errAtBlock = -1;       // подія 0x1F на цьому блоці
  int truncAtBlock = -1;     // RD_USB_DATA0 цього блоку: довжина 64, але лише 10 байтів
  int badLenAtBlock = -1;    // RD_USB_DATA0 цього блоку: довжина 0xFF
  // Журнал
  int cmdCount[256] = {0};
  uint32_t lastLba = 0, lastCount = 0;
  int blockCnt = 0;          // блоків, виданих чіпом
  bool ejected = false;

  // ---- Ch376Link
  void write(const uint8_t* d, size_t n) override {
    if (n < 3 || d[0] != 0x57 || d[1] != 0xAB) return;
    cmdCount[d[2]]++;
    switch (d[2]) {
      case 0x22:  // GET_STATUS
        if (!silentStatus) rx.push_back(takeEvent());
        break;
      case 0x3E:  // DISK_CAPACITY
        capMode_ = true;
        post(0x14);
        break;
      case 0x54: {  // DISK_READ: LBA 4 Б LE, n 1 Б
        if (ejected) { post(0x82); break; }
        const uint32_t lba = (uint32_t)d[3] | ((uint32_t)d[4] << 8) | ((uint32_t)d[5] << 16) | ((uint32_t)d[6] << 24);
        const uint32_t cnt = d[7];
        lastLba = lba;
        lastCount = cnt;
        if (cnt == 0 || (uint64_t)lba + cnt > sectorCount()) { post(0x1F); break; }
        active_ = true;
        curLba_ = lba;
        total_ = cnt * 8u;
        idx_ = 0;
        nextBlock();
        break;
      }
      case 0x55:  // DISK_RD_GO
        if (ejected) { post(0x82); break; }
        if (!active_) { post(0x14); break; }
        idx_++;
        if (idx_ < total_) nextBlock();
        else { active_ = false; post(0x14); }
        break;
      case 0x27:  // RD_USB_DATA0
        if (capMode_) {
          capMode_ = false;
          const uint32_t c = (uint32_t)sectorCount();
          rx.push_back(4);
          for (int i = 0; i < 4; i++) rx.push_back((uint8_t)(c >> (8 * i)));
        } else if (blkReady_) {
          blkReady_ = false;
          if (curBlock_ == badLenAtBlock) {
            rx.push_back(0xFF);
          } else if (curBlock_ == truncAtBlock) {
            rx.push_back(64);
            for (int i = 0; i < 10; i++) rx.push_back(blk_[i]);
          } else {
            rx.push_back(64);
            rx.insert(rx.end(), blk_, blk_ + 64);
          }
        } else {
          rx.push_back(0);
        }
        break;
      default: break;
    }
  }

  bool readByte(uint8_t& b, uint32_t tmoMs) override {
    if (!rx.empty()) {
      b = rx.front();
      rx.erase(rx.begin());
      return true;
    }
    nowV += tmoMs * 1000u;
    return false;
  }
  void flushRx() override { rx.clear(); }
  bool setBaud(uint32_t) override { return true; }
  bool hasIntPin() const override { return intPin; }
  uint32_t byteTimeUs() const override { return byteUs; }
  bool intLow() const override { return visible(); }
  bool hwReset() override { return false; }
  bool abortRequested() const override { return abortFlag; }
  void delayMs(uint32_t ms) override { nowV += ms * 1000u; }
  uint32_t nowUs() const override { return nowV; }

  bool waitIntLow(uint32_t tmoUs) override {
    if (visible()) return true;
    uint32_t best = 0xFFFFFFFFu;
    for (const Ev& e : events)
      if (e.at < best) best = e.at;
    if (best != 0xFFFFFFFFu && best - nowV <= tmoUs) {
      nowV = best;
      return true;
    }
    nowV += tmoUs;
    return false;
  }
  void yieldNow() override { nowV += 20; }
  size_t readBytes(uint8_t* dst, size_t n, uint32_t tmoUs) override {
    size_t k = 0;
    while (k < n && !rx.empty()) {
      dst[k++] = rx.front();
      rx.erase(rx.begin());
    }
    if (k < n) nowV += tmoUs;
    return k;
  }

 private:
  struct Ev {
    uint32_t at;
    uint8_t code;
  };

  bool visible() const {
    for (const Ev& e : events)
      if (e.at <= nowV) return true;
    return false;
  }
  void post(uint8_t c) { events.push_back(Ev{nowV + latencyUs, c}); }
  // Як у чіпа: GET_STATUS віддає НАЙНОВІШУ видиму подію; старіші видимі відкидаються.
  uint8_t takeEvent() {
    uint8_t c = kCh376IntNone;
    bool any = false;
    for (size_t i = 0; i < events.size();) {
      if (events[i].at <= nowV) {
        c = events[i].code;
        any = true;
        events.erase(events.begin() + (long)i);
      } else {
        i++;
      }
    }
    return any ? c : kCh376IntNone;
  }

  // Видає наступний блок (або подію збою). blockCnt рахує всі блоки сеансу.
  void nextBlock() {
    blockCnt++;
    curBlock_ = blockCnt;
    if (blockCnt == ejectAtBlock) {
      ejected = true;
      active_ = false;
      blkReady_ = false;
      post(0x16);
      return;
    }
    if (blockCnt == errAtBlock) {
      active_ = false;
      blkReady_ = false;
      post(0x1F);
      return;
    }
    const size_t base = (size_t)(curLba_ + idx_ / 8u) * 512u + (size_t)(idx_ % 8u) * 64u;
    for (uint32_t i = 0; i < 64; i++) blk_[i] = img_[base + i];
    blkReady_ = true;
    post(0x1D);
  }

  std::vector<uint8_t> img_;
  std::vector<uint8_t> rx;
  std::vector<Ev> events;
  uint32_t nowV = 0;
  bool active_ = false;
  bool capMode_ = false;
  bool blkReady_ = false;
  uint32_t curLba_ = 0, total_ = 0, idx_ = 0;
  int curBlock_ = 0;
  uint8_t blk_[64] = {0};
};

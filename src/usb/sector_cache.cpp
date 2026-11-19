#include "usb/sector_cache.h"

#if ENABLE_USB_CH376

#include <string.h>

#include <new>

bool SectorCache::open(uint8_t slots, uint8_t readAhead) {
  close();
  if (slots > kMaxSlots) slots = kMaxSlots;
  Meta* m = new (std::nothrow) Meta;
  uint8_t* d = nullptr;
  if (m && slots) d = new (std::nothrow) uint8_t[(size_t)slots * kBytes];
  if (!m || (slots && !d)) {
    delete m;
    return false;
  }
  meta_ = m;
  data_ = d;
  tag_ = m->tag;
  age_ = m->age;
  slots_ = slots;
  ra_ = readAhead < 1 ? 1 : (readAhead > slots && slots ? slots : readAhead);
  if (!slots) ra_ = 1;
  memset(m->uniq, 0, sizeof m->uniq);
  for (uint8_t i = 0; i < kShadowWays; i++) m->shadow[i] = kNone;
  shHead_ = 0;
  clock_ = 0;
  last_ = lastEnd_ = kNone;
  seqHint_ = false;
  st_ = SectorCacheStats{};
  invalidate();
  return true;
}

void SectorCache::close() {
  delete[] data_;
  delete meta_;
  data_ = nullptr;
  meta_ = nullptr;
  tag_ = age_ = nullptr;
  slots_ = 0;
  ra_ = 1;
}

void SectorCache::invalidate() {
  if (!meta_) return;
  for (uint8_t i = 0; i < kMaxSlots; i++) {
    tag_[i] = kNone;
    age_[i] = 0;
  }
  last_ = lastEnd_ = kNone;
  seqHint_ = false;
}

void SectorCache::note(uint32_t lba) {
  if (!meta_) return;
  st_.reqs++;
  seqHint_ = (last_ != kNone && lba == last_ + 1) || (lastEnd_ != kNone && lba == lastEnd_);
  if (last_ != kNone) {
    if (lba == last_ + 1) st_.seq++;
    else if (lba == last_) st_.same++;
    else st_.jump++;
  }
  // унікальність: хеш у бітову карту
  const uint32_t h = (lba * 2654435761u) >> 21 & (kUniqBits - 1);
  uint32_t& w = meta_->uniq[h >> 5];
  const uint32_t bit = 1u << (h & 31);
  if (!(w & bit)) {
    w |= bit;
    st_.unique++;
  }
  // «тінь» LRU-вікна: повтор сектора з останніх kShadowWays запитів (різних секторів)
  bool in = false;
  for (uint8_t i = 0; i < kShadowWays; i++)
    if (meta_->shadow[i] == lba) in = true;
  if (in) st_.wouldHit++;
  else {
    meta_->shadow[shHead_] = lba;
    shHead_ = (uint8_t)((shHead_ + 1) % kShadowWays);
  }
  last_ = lba;
}

int SectorCache::find(uint32_t lba) const {
  for (uint8_t i = 0; i < slots_; i++)
    if (tag_[i] == lba) return i;
  return -1;
}

uint32_t SectorCache::planFetch(uint32_t lba, uint32_t sectorCount) const {
  uint32_t k = 1;
  if (ra_ > 1 && slots_ > 1 && seqHint_) k = ra_;  // seqHint_ виставив note() для цього запиту
  if (k > slots_) k = slots_ ? slots_ : 1;
  if (lba + k > sectorCount) k = sectorCount > lba ? sectorCount - lba : 1;
  if (k < 1) k = 1;
  for (uint32_t j = 1; j < k; j++)
    if (find(lba + j) >= 0) {
      k = j;
      break;
    }
  return k;
}

int SectorCache::victimRun(uint32_t k) const {
  if (!slots_ || k < 1 || k > slots_) return 0;
  int best = 0;
  uint32_t bestCost = 0xFFFFFFFFu;
  for (uint32_t s = 0; s + k <= slots_; s++) {
    uint32_t cost = 0;
    for (uint32_t j = 0; j < k; j++) {
      const uint32_t a = tag_[s + j] == kNone ? 0u : age_[s + j];
      if (a > cost) cost = a;
    }
    if (cost < bestCost) {
      bestCost = cost;
      best = (int)s;
    }
  }
  return best;
}

void SectorCache::fill(int first, uint32_t lba, uint32_t k) {
  for (uint32_t j = 0; j < k; j++) {
    const int i = first + (int)j;
    if (tag_[i] != kNone) st_.evictions++;
    tag_[i] = lba + j;
    age_[i] = ++clock_;
  }
  if (k > 1) st_.raSectors += k - 1;
  lastEnd_ = lba + k;
}

#endif  // ENABLE_USB_CH376

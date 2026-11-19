#pragma once
#include <stddef.h>
#include <stdint.h>

#include "config.h"

#if ENABLE_USB_CH376

// M4c1. LRU-кеш секторів блочного пристрою USB + лічильники звернень. Чистий C++ (без Arduino.h і SdFat), тестується в native.
// Живе на heap лише на час скану: open() при старті, close() після скану (і при close() тому). У відтворенні кеша немає.
// slots == 0 — режим «тінь»: даних нема, лише лічильники (унікальні сектори, повтори, «було б попадання в LRU-вікно»).
// Дані — slots × 512 Б суцільним масивом, тож випереджальне читання k секторів іде однією DISK_READ у k сусідніх слотів.
// Усі виклики — з однієї задачі (під мʼютексом UsbVolume).

struct SectorCacheStats {
  uint32_t reqs = 0;       // запитів одного сектора
  uint32_t unique = 0;     // унікальних секторів (бітова карта 2048 біт; при >~300 секторів недооцінка ≈10%)
  uint32_t wouldHit = 0;   // повторні звернення, що потрапили б у LRU-вікно з kShadowWays секторів (гіпотеза «повторне читання»)
  uint32_t seq = 0;        // запит = попередній + 1
  uint32_t same = 0;       // запит = попередній
  uint32_t jump = 0;       // інакше (випадковий перехід)
  uint32_t hits = 0;       // попадання (лише slots > 0)
  uint32_t misses = 0;     // промахи (лише slots > 0)
  uint32_t evictions = 0;  // витіснено зайнятих слотів
  uint32_t raSectors = 0;  // секторів дочитано наперед (понад запитаний)
};

class SectorCache {
 public:
  static constexpr uint32_t kBytes = kCh376DiskSecBytes;  // розмір сектора
  static constexpr uint8_t kMaxSlots = 8;
  static constexpr uint8_t kShadowWays = 8;               // вікно «тіні» (= типовий розмір кешу)
  static constexpr uint32_t kUniqBits = 2048;
  static constexpr uint32_t kNone = 0xFFFFFFFFu;

  SectorCache() = default;
  ~SectorCache() { close(); }
  SectorCache(const SectorCache&) = delete;
  SectorCache& operator=(const SectorCache&) = delete;

  // Виділяє heap (nothrow). slots 0..kMaxSlots; readAhead 1..slots (0/1 = без випередження). Повторний open() = close() + open().
  bool open(uint8_t slots, uint8_t readAhead);
  void close();  // звільняє heap; статистику лишає (читається після close)
  bool isOpen() const { return meta_ != nullptr; }
  uint8_t slots() const { return slots_; }
  uint8_t readAhead() const { return ra_; }

  // Усе вважається недійсним (перемонтування, виймання). Статистику не чіпає.
  void invalidate();
  void resetStats() { st_ = SectorCacheStats{}; }
  void restoreStats(const SectorCacheStats& s) { st_ = s; }  // при розширенні кешу лічильники переносяться
  const SectorCacheStats& stats() const { return st_; }

  // Облік запиту сектора (лічильники й «тінь»). Викликається на кожен запит одного сектора.
  void note(uint32_t lba);
  // Попадання: слот або -1. touch() оновлює давність.
  int find(uint32_t lba) const;
  uint8_t* slot(int i) { return data_ + (size_t)i * kBytes; }
  void touch(int i) { age_[i] = ++clock_; }
  void hit(int i) {  // попадання: давність + лічильник
    age_[i] = ++clock_;
    st_.hits++;
  }
  void miss() { st_.misses++; }
  // Скільки секторів читати за промах lba: 1, або до readAhead при послідовному доступі (lba = попередній запит + 1
  // або = кінець останнього завантаження), обрізано межею розділу (sectorCount) і першим вже кешованим сектором уперед.
  uint32_t planFetch(uint32_t lba, uint32_t sectorCount) const;
  // Перший слот суцільного забігу з k слотів, найменш потрібного (мін. максимальної давності; вільні слоти — давність 0).
  int victimRun(uint32_t k) const;
  // Після читання k секторів lba.. у слоти first..first+k-1: теги, давність, облік витіснень.
  void fill(int first, uint32_t lba, uint32_t k);

 private:
  struct Meta {
    uint32_t tag[kMaxSlots];
    uint32_t age[kMaxSlots];
    uint32_t shadow[kShadowWays];  // останні запитані сектори (кільце)
    uint32_t uniq[kUniqBits / 32];
  };
  Meta* meta_ = nullptr;
  uint32_t* tag_ = nullptr;
  uint32_t* age_ = nullptr;
  uint8_t* data_ = nullptr;
  uint8_t slots_ = 0, ra_ = 1;
  uint8_t shHead_ = 0;
  uint32_t clock_ = 0;
  uint32_t last_ = kNone;      // останній запитаний сектор
  uint32_t lastEnd_ = kNone;   // сектор одразу після останнього завантаженого діапазону
  bool seqHint_ = false;       // поточний запит послідовний (виставляє note())
  SectorCacheStats st_;
};

#endif  // ENABLE_USB_CH376

#include "media/usb_media.h"

#if ENABLE_USB_CH376

#include <string.h>

#include "util/log.h"

namespace {
constexpr const char* TAG = "usbmedia";
}  // namespace

bool UsbMedia::mount() {
  if (mounted_) return true;
  UsbVolume& v = UsbVolume::get();
  if (v.isOpen()) v.close();  // після Removed том лишається відкритим до unmount(): починаємо з чистого стану
  UsbVolInfo inf;
  if (!v.open(inf)) {
    fail_ = inf.fail;
    return false;  // причину вже залоговано в UsbVolume::open; том і порт закриті
  }
  fail_ = UsbOpenFail::None;
  mounted_ = true;
  serial_ = inf.serial;
  baud_ = inf.baud;
  openMs_ = inf.openMs;
  fsType_ = inf.fsType;
  partSectors_ = inf.partSectors;
  if (inf.baud < kUsbBaudFast)  // M4b4/п.9: резервний режим — відтворення все одно стартує, але з явним попередженням
    LOGW(TAG, "швидкість USB %u Бод < %u: РЕЗЕРВНИЙ режим (самоперевірка: %s). Потік 320 кбіт/с у запасі лише ≈2 кадри", (unsigned)inf.baud,
         (unsigned)kUsbBaudFast, selfTestName(inf.selfTest));
  LOGI(TAG, "том змонтовано: %s, серійний №%08X, %u Бод, відкриття %u мс", inf.fsType == 64 ? "exFAT" : inf.fsType == 32 ? "FAT32" : "FAT16",
       (unsigned)inf.serial, (unsigned)inf.baud, (unsigned)inf.openMs);
  return true;
}

// Спільний підсумок успішного відкриття: стан UsbMedia і логи (як у mount()).
static void logMounted(const UsbVolInfo& inf) {
  if (inf.baud < kUsbBaudFast)  // M4b4/п.9: резервний режим — відтворення все одно стартує, але з явним попередженням
    LOGW(TAG, "швидкість USB %u Бод < %u: РЕЗЕРВНИЙ режим (самоперевірка: %s). Потік 320 кбіт/с у запасі лише ≈2 кадри", (unsigned)inf.baud,
         (unsigned)kUsbBaudFast, selfTestName(inf.selfTest));
  LOGI(TAG, "том змонтовано: %s, серійний №%08X, %u Бод, відкриття %u мс", inf.fsType == 64 ? "exFAT" : inf.fsType == 32 ? "FAT32" : "FAT16",
       (unsigned)inf.serial, (unsigned)inf.baud, (unsigned)inf.openMs);
}

bool UsbMedia::mountBegin() {
  if (mounted_) return true;
  UsbVolume& v = UsbVolume::get();
  if (v.isOpen()) v.close();
  UsbVolInfo inf;
  if (!v.openBegin(inf)) {
    fail_ = inf.fail;
    return false;
  }
  fail_ = UsbOpenFail::None;
  return true;
}

IMedia::MountPoll UsbMedia::mountStep(uint32_t nowMs) {
  UsbVolume& v = UsbVolume::get();
  if (!v.opening()) return mounted_ ? MountPoll::Done : MountPoll::Failed;
  UsbVolInfo inf;
  const OpenPoll r = v.openStep(nowMs, inf);
  if (r == OpenPoll::Pending) return MountPoll::Pending;
  if (r == OpenPoll::Failed) {
    fail_ = inf.fail;
    return MountPoll::Failed;
  }
  fail_ = UsbOpenFail::None;
  mounted_ = true;
  serial_ = inf.serial;
  baud_ = inf.baud;
  openMs_ = inf.openMs;
  fsType_ = inf.fsType;
  partSectors_ = inf.partSectors;
  logMounted(inf);
  return MountPoll::Done;
}

void UsbMedia::mountAbort() { UsbVolume::get().openAbort(); }

void UsbMedia::unmount() {
  UsbVolume::get().close();
  mounted_ = false;
  serial_ = 0;
}

// Відкритий том, що перестав відповідати → один Removed (далі mounted()==false). Без очікування мʼютекса: під час читання
// пристрій явно живий. Фізичне закриття тому робить власник (джерело) після зупинки сесії — тут лише логічний стан.
MediaEvent UsbMedia::poll(uint32_t) {
  if (!mounted_) return MediaEvent::None;
  if (UsbVolume::get().presentNoWait()) return MediaEvent::None;
  mounted_ = false;
  LOGW(TAG, "носій зник (помилка читання Disconnected): Removed");
  return MediaEvent::Removed;
}

int UsbMedia::openDir(const char* path) { return mounted_ ? UsbVolume::get().dirOpen(path) : -1; }

int UsbMedia::nextEntry(int h, MediaEntry& e) {
  if (!mounted_) return -1;
  return UsbVolume::get().dirNext(h, e.name, sizeof e.name, e.nameOk, e.isDir, e.hidden);
}

void UsbMedia::closeDir(int h) { UsbVolume::get().dirClose(h); }

void UsbMedia::info(MediaInfo& o) {
  o = MediaInfo{};
  o.present = mounted_;
  o.mounted = mounted_;
  o.cdPin = -1;
  o.cdRaw = -1;
  o.cardType = "USB";
  o.fs = fsType_ == 64 ? "exFAT" : fsType_ == 32 ? "FAT32" : fsType_ == 16 ? "FAT16" : "-";
  o.sizeMb = partSectors_ / 2048;
  o.mhz = 0;
}

// Профіль скану: what 0 = почати, 1 = зупинити, 2 = вивести. Час у мс/мкс цілими (без float у логу).
void UsbMedia::scanProfile(int what, uint32_t wallMs, uint32_t busyMs) {
  UsbVolume& v = UsbVolume::get();
  if (what == 0) { v.profBegin(); return; }
  if (what == 1) { v.profEnd(); return; }
  const UsbScanProf& p = v.prof();
  if (p.on) { LOGI(TAG, "профіль: скан ще триває, цифри проміжні"); }
  const uint32_t nextMs = p.nextUs / 1000, openMs = p.openDirUs / 1000, closeMs = p.closeUs / 1000, lockMs = p.lockWaitUs / 1000;
  const uint32_t media = nextMs + openMs + closeMs;
  const uint32_t logic = busyMs > media + lockMs ? busyMs - media - lockMs : 0;
  const uint32_t idle = wallMs > busyMs ? wallMs - busyMs : 0;
  auto pct = [&](uint32_t ms) { return wallMs ? (unsigned)((uint64_t)ms * 100 / wallMs) : 0u; };
  // Рядки короткі (≤ ~90 символів): консольний буфер логу ≈ 200 Б, довші обрізаються.
  LOGI(TAG, "профіль: сектори %u, команд чіпа %u (%u.%02u сект./команду), викликів %u", (unsigned)p.sectors, (unsigned)p.cmds,
       p.cmds ? (unsigned)(p.sectors / p.cmds) : 0u, p.cmds ? (unsigned)((uint64_t)p.sectors * 100 / p.cmds % 100) : 0u, (unsigned)p.calls);
  LOGI(TAG, "профіль: openDir %u: сер. %u мкс, макс. %u мкс", (unsigned)p.openDirN, p.openDirN ? (unsigned)(p.openDirUs / p.openDirN) : 0u,
       (unsigned)p.openDirMaxUs);
  LOGI(TAG, "профіль: openNext %u: записів %u, кінців %u, помилок %u", (unsigned)p.nextN, (unsigned)p.entries, (unsigned)p.endN, (unsigned)p.errN);
  LOGI(TAG, "профіль: openNext сер. %u мкс, макс. %u мкс; closeDir %u", p.nextN ? (unsigned)(p.nextUs / p.nextN) : 0u, (unsigned)p.nextMaxUs,
       (unsigned)p.closeN);
  LOGI(TAG, "профіль: мʼютекс: %u захоплень, очікування %u мс, макс. %u мкс", (unsigned)p.lockN, (unsigned)lockMs, (unsigned)p.lockWaitMaxUs);
  LOGI(TAG, "профіль: стіна %u мс: openNext %u (%u%%), openDir %u (%u%%), мʼютекс %u (%u%%)", (unsigned)wallMs, (unsigned)nextMs, pct(nextMs),
       (unsigned)openMs, pct(openMs), (unsigned)lockMs, pct(lockMs));
  LOGI(TAG, "профіль: логіка скану %u (%u%%), простій loop %u (%u%%), closeDir %u (%u%%)", (unsigned)logic, pct(logic), (unsigned)idle, pct(idle),
       (unsigned)closeMs, pct(closeMs));
  // M4c1: звернення до секторів (гіпотеза «повторне читання»). unique — бітова карта, недооцінка ≈10% при >300 секторів.
  const uint32_t rep = p.cReqs > p.cUnique ? p.cReqs - p.cUnique : 0;
  LOGI(TAG, "сектори: запитів %u, унікальних≈%u, повторних≈%u, у LRU-вікні 8: %u", (unsigned)p.cReqs, (unsigned)p.cUnique, (unsigned)rep,
       (unsigned)p.cWould);
  LOGI(TAG, "сектори: послідовних %u, тих самих %u, стрибків %u", (unsigned)p.cSeq, (unsigned)p.cSame, (unsigned)p.cJump);
  LOGI(TAG, "кеш %s: слотів %u, попадань %u, промахів %u, витіснень %u, наперед %u", p.cacheOn ? "є" : "немає", (unsigned)p.cSlots,
       (unsigned)p.cHits, (unsigned)p.cMiss, (unsigned)p.cEvict, (unsigned)p.cRa);
  LOGI(TAG, "getName пропущено: %u файлів (суфікс 8.3 не MP3)", (unsigned)p.nameSkips);
}

// M4c1: кеш секторів і фільтр імен діють лише між scanBegin() і scanEnd(); у відтворенні їх немає.
void UsbMedia::scanBegin() { UsbVolume::get().scanBegin(); }

void UsbMedia::scanEnd() { UsbVolume::get().scanEnd(); }

void UsbMedia::ioLock() {
  locked_ = UsbVolume::get().lockFor(kUsbLockWaitMs);
}

void UsbMedia::ioUnlock() {
  if (!locked_) return;
  locked_ = false;
  UsbVolume::get().unlock();
}

#endif  // ENABLE_USB_CH376


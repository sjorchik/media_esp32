#include "media/sd_media.h"

#include <Arduino.h>
#include <SPI.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "SdFat.h"
#include "config.h"
#include "pins.h"
#include "util/log.h"

// Без UTF-8 SdFat спотворює не-ASCII імена: -DUSE_UTF8_LONG_NAMES=1 (platformio.ini).
// Якщо SdFatConfig.h перевизначає макрос без #ifndef — зупиняємо збірку, а не ховаємо проблему.
#if defined(USE_UTF8_LONG_NAMES) && !USE_UTF8_LONG_NAMES
#error "SdFat: потрібен USE_UTF8_LONG_NAMES=1 (див. build_flags або SdFatConfig.h у .pio/libdeps)"
#endif

namespace {

constexpr const char* TAG = "sd";
constexpr int kDirHandles = kScanMaxDepth + 1;

SPIClass s_spi(VSPI);
SdFs s_sd;
FsFile s_dirs[kDirHandles];
FsFile s_file;

bool s_mounted = false, s_present = false;
bool s_blocked = false;      // після відмонтування через помилки: чекати фізичного виймання картки
volatile bool s_lostPending = false;  // подія Removed ще не видана (ставить і задача mp3 через noteError)
uint8_t s_speed = 0;         // індекс у kSdSpiMhz
uint8_t s_errStreak = 0;
uint32_t s_errTotal = 0;
uint32_t s_lastOkAt = 0;

// card-detect
bool s_cdRaw = false, s_cdStable = false;
uint32_t s_cdSince = 0, s_nextMountAt = 0;
bool s_mountFailLogged = false;
// опитування (SD_CD_PIN < 0)
uint32_t s_pollAt = 0;
uint8_t s_pollFails = 0;

// SdFat не потокобезпечний: усі звернення до s_sd/s_file/s_dirs та SPI — під цим рекурсивним мʼютексом.
// Власники: app (скан, mount, poll-probe) і задача mp3 (читання файлу під час відтворення).
SemaphoreHandle_t s_mtx = nullptr;
constexpr TickType_t kLockTicks = pdMS_TO_TICKS(kSdLockWaitMs);
struct Guard {
  bool ok;
  explicit Guard(TickType_t t = kLockTicks) : ok(s_mtx && xSemaphoreTakeRecursive(s_mtx, t) == pdTRUE) {}
  ~Guard() { if (ok) xSemaphoreGiveRecursive(s_mtx); }
  Guard(const Guard&) = delete;
  Guard& operator=(const Guard&) = delete;
};

inline bool due(uint32_t now, uint32_t t) { return (int32_t)(now - t) >= 0; }
inline bool cdActive() { return digitalRead(SD_CD_PIN) == (SD_CD_ACTIVE_LOW ? LOW : HIGH); }

void closeAll() {
  for (auto& d : s_dirs) if (d.isOpen()) d.close();
  if (s_file.isOpen()) s_file.close();
}

bool probe() {  // читання сектора 0: картка відповідає?
  uint8_t buf[512];
  return s_sd.card()->readSector(0, buf);
}

bool beginAt(uint8_t idx) {
  SdSpiConfig cfg(PIN_SD_CS, SHARED_SPI, SD_SCK_MHZ(kSdSpiMhz[idx]), &s_spi);
  if (!s_sd.begin(cfg)) {
    // card()==nullptr → картка не пройшла ініціалізацію (CMD0/ACMD41: живлення/проводка/CS);
    // card()!=nullptr і errorCode=0 → картка жива, але ФС не знайдена (формат/розділ)
    SdCard* c = s_sd.card();
    if (!c) LOGW(TAG, "begin(%u МГц): картка не ініціалізувалась (живлення/проводка/CS)", (unsigned)kSdSpiMhz[idx]);
    else LOGW(TAG, "begin(%u МГц): errorCode=0x%02X errorData=0x%02X%s", (unsigned)kSdSpiMhz[idx], (unsigned)c->errorCode(),
              (unsigned)c->errorData(), c->errorCode() ? "" : " — ФС не знайдена (формат/розділ)");
    return false;
  }
  FsFile root = s_sd.open("/", O_RDONLY);
  if (!root.isOpen()) {
    LOGW(TAG, "begin(%u МГц) ок, але корінь не відкрито", (unsigned)kSdSpiMhz[idx]);
    return false;
  }
  root.close();
  return true;
}

bool mountInternal() {
  if (s_mounted) return true;
  for (uint8_t i = 0; i < kSdSpiSteps; i++) {  // нова картка: з максимальної частоти, відкат при невдачі
    if (beginAt(i)) {
      s_speed = i;
      s_mounted = true;
      s_errStreak = 0;
      s_lastOkAt = millis();
      LOGI(TAG, "змонтовано, SPI %u МГц", (unsigned)kSdSpiMhz[i]);
      return true;
    }
  }
  s_speed = 0;
  return false;
}

void unmountInternal(bool lost) {
  closeAll();  // усі хендли недійсні; читання одразу повертають помилку
  s_mounted = false;
  s_speed = 0;
  if (lost) { s_lostPending = true; s_blocked = true; }
}

void stepDown() {
  closeAll();
  if (s_speed + 1 < kSdSpiSteps && beginAt(s_speed + 1)) {
    s_speed++;
    LOGW(TAG, "відкат частоти SPI до %u МГц", (unsigned)kSdSpiMhz[s_speed]);
    return;
  }
  LOGW(TAG, "відкат неможливий — відмонтування");
  unmountInternal(true);
}

void noteError(const char* what) {
  s_errTotal++;
  s_errStreak++;
  LOGW(TAG, "помилка (%s), підряд %u/%u", what, (unsigned)s_errStreak, (unsigned)kSdErrUnmount);
  if (s_errStreak >= kSdErrUnmount) {
    LOGW(TAG, "%u помилок підряд — відмонтування", (unsigned)kSdErrUnmount);
    unmountInternal(true);
  } else if (s_errStreak % kSdErrStepDown == 0) {
    stepDown();
  }
}

void okOp() {
  s_errStreak = 0;
  s_lastOkAt = millis();
}

}  // namespace

// ============================================================ SdMedia

bool SdMedia::begin() {
  if (!s_mtx) s_mtx = xSemaphoreCreateRecursiveMutex();
  if (!s_mtx) { LOGE(TAG, "не вдалося створити мʼютекс SdFat"); return false; }
  s_spi.begin(PIN_SD_SCK, PIN_SD_MISO, PIN_SD_MOSI, PIN_SD_CS);
  if constexpr (SD_CD_PIN >= 0) {
    pinMode(SD_CD_PIN, INPUT_PULLUP);
    s_cdRaw = s_cdStable = cdActive();  // стартовий стан без антидребезгу
  }
  return true;
}

bool SdMedia::mounted() const { return s_mounted; }
bool SdMedia::present() const { return s_present; }

bool SdMedia::mount() {
  Guard g;
  return g.ok && mountInternal();
}

void SdMedia::unmount() {
  Guard g;
  if (g.ok) unmountInternal(false);
}

// Під час відтворення poll() не торкається SPI: з CD-піном присутність — по GPIO; без нього probe()
// виконується лише коли немає активності (читання файлу оновлює s_lastOkAt) і під мʼютексом.
MediaEvent SdMedia::poll(uint32_t now) {
  if (s_lostPending) {  // відмонтування через помилки
    s_lostPending = false;
    s_present = false;
    return MediaEvent::Removed;
  }
  if constexpr (SD_CD_PIN >= 0) {
    bool raw = cdActive();
    if (raw != s_cdRaw) { s_cdRaw = raw; s_cdSince = now; }
    if (raw != s_cdStable && (uint32_t)(now - s_cdSince) >= kSdDebounceMs) {
      s_cdStable = raw;
      if (!raw) { s_blocked = false; s_mountFailLogged = false; }
    }
    if (!s_cdStable) {
      s_present = false;
      if (s_mounted) {
        Guard g;  // читання mp3 уже завершується помилкою (≤ ~300 мс), мʼютекс звільниться
        if (!g.ok) return MediaEvent::None;  // повторимо на наступному poll
        if (s_mounted) { unmountInternal(false); return MediaEvent::Removed; }
      }
      return MediaEvent::None;
    }
    s_present = true;
    if (s_mounted || s_blocked || !due(now, s_nextMountAt)) return MediaEvent::None;
    Guard g;
    if (!g.ok) return MediaEvent::None;
    if (mountInternal()) return MediaEvent::Inserted;
    s_nextMountAt = now + kSdRemountMs;
    if (!s_mountFailLogged) { s_mountFailLogged = true; LOGW(TAG, "картка є (CD), але mount не вдався; повтор кожні %u мс", (unsigned)kSdRemountMs); }
    return MediaEvent::None;
  } else {
    if (!due(now, s_pollAt)) return MediaEvent::None;
    s_pollAt = now + (s_blocked ? 4 * kSdPollMs : kSdPollMs);
    if (s_mounted && (uint32_t)(now - s_lastOkAt) < kSdPollMs) { s_pollFails = 0; return MediaEvent::None; }  // є активність
    Guard g;
    if (!g.ok) return MediaEvent::None;  // SPI зайнятий читанням файлу: картка явно жива
    if (s_mounted) {
      if (probe()) { okOp(); s_pollFails = 0; return MediaEvent::None; }
      if (++s_pollFails >= kSdPollFails) {
        s_pollFails = 0;
        unmountInternal(false);
        s_present = false;
        return MediaEvent::Removed;
      }
      return MediaEvent::None;
    }
    if (mountInternal()) {
      if (s_blocked) { unmountInternal(false); return MediaEvent::None; }  // та сама збійна картка: мовчки
      s_present = true;
      return MediaEvent::Inserted;
    }
    s_blocked = false;  // не відповідає = картки немає
    s_present = false;
    return MediaEvent::None;
  }
}

int SdMedia::openDir(const char* path) {
  Guard g;
  if (!g.ok || !s_mounted) return -1;
  for (int i = 0; i < kDirHandles; i++) {
    if (s_dirs[i].isOpen()) continue;
    s_dirs[i] = s_sd.open(path, O_RDONLY);
    if (!s_dirs[i].isOpen() || !s_dirs[i].isDir()) {
      s_dirs[i].close();
      if (!probe()) noteError("open dir");
      return -1;
    }
    return i;
  }
  return -1;
}

void SdMedia::closeDir(int h) {
  Guard g;
  if (g.ok && h >= 0 && h < kDirHandles) s_dirs[h].close();
}

int SdMedia::nextEntry(int h, MediaEntry& e) {
  Guard g;
  if (!g.ok || !s_mounted || h < 0 || h >= kDirHandles || !s_dirs[h].isOpen()) return -1;
  FsFile f;
  if (!f.openNext(&s_dirs[h], O_RDONLY)) {
    if (probe()) { okOp(); return 0; }  // справжній кінець каталогу
    noteError("dir");
    return -1;
  }
  size_t n = f.getName(e.name, sizeof e.name);
  e.nameOk = n > 0 && n < sizeof e.name;
  if (!e.nameOk) e.name[0] = 0;
  e.isDir = f.isDir();
  e.hidden = f.isHidden();
  f.close();
  okOp();
  return 1;
}

void SdMedia::info(MediaInfo& o) {
  o = MediaInfo{};
  o.present = s_present;
  o.mounted = s_mounted;
  o.cdPin = SD_CD_PIN;
  o.cdRaw = (SD_CD_PIN >= 0) ? (int)s_cdRaw : -1;
  o.errStreak = s_errStreak;
  o.errTotal = s_errTotal;
  o.cardType = "-";
  o.fs = "-";
  if (!s_mounted) return;
  Guard g;
  if (!g.ok || !s_mounted) return;
  o.mhz = kSdSpiMhz[s_speed];
  uint8_t t = s_sd.card()->type();  // 1=SD1, 2=SD2, 3=SDHC/SDXC
  o.cardType = t == 1 ? "SD1" : t == 2 ? "SD2" : t == 3 ? "SDHC/SDXC" : "?";
  uint8_t ft = s_sd.fatType();      // 16, 32, 64=exFAT
  o.fs = ft == 64 ? "exFAT" : ft == 32 ? "FAT32" : ft == 16 ? "FAT16" : "?";
  o.sizeMb = s_sd.card()->sectorCount() / 2048;
}

// ============================================================ SdFileReader
// Після M3b читач використовує задача mp3 (під час відтворення app файл не чіпає).

bool SdFileReader::isOpen() const { return s_file.isOpen(); }

void SdFileReader::close() {
  Guard g;
  if (g.ok && s_file.isOpen()) s_file.close();
}

bool SdFileReader::open(const char* path) {
  close();
  failed_ = false;
  pos_ = size_ = 0;
  Guard g;
  if (!g.ok || !s_mounted) { failed_ = true; return false; }
  s_file = s_sd.open(path, O_RDONLY);
  if (!s_file.isOpen() || !s_file.isFile()) {
    s_file.close();
    failed_ = true;
    return false;
  }
  size_ = (uint32_t)s_file.fileSize();
  return true;
}

size_t SdFileReader::read(void* dst, size_t n) {
  Guard g;
  if (!g.ok || !s_mounted || !s_file.isOpen()) { failed_ = true; return 0; }  // носій зник: без очікувань
  int r = s_file.read(dst, n);
  if (r < 0 || (r == 0 && pos_ < size_)) {
    failed_ = true;
    noteError("read");
    return 0;
  }
  pos_ += (uint32_t)r;
  okOp();
  return (size_t)r;
}

bool SdFileReader::seek(uint32_t p) {
  Guard g;
  if (!g.ok || !s_mounted || !s_file.isOpen()) { failed_ = true; return false; }
  if (!s_file.seekSet(p)) return false;
  pos_ = p;
  return true;
}

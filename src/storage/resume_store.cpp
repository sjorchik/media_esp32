#include "storage/resume_store.h"

#include <string.h>

#include "storage/nvs_store.h"
#include "util/log.h"

namespace {

constexpr const char* TAG = "rsm";
constexpr const char* kKeys[2] = {"rsm_sd", "rsm_usb"};

uint8_t s_buf[Resume::kBlobMax];  // єдиний статичний буфер блоба (≈ 272 Б); лише loopTask
uint32_t s_crc[2] = {0, 0};       // CRC останнього блоба, що лежить у NVS (запис або читання)
bool s_have[2] = {false, false};

inline int ix(ResumeStore::Src s) { return s == ResumeStore::Src::Usb ? 1 : 0; }

inline uint32_t blobCrc(const uint8_t* b, size_t n) {  // CRC зберігається в останніх 4 байтах блоба
  return (uint32_t)b[n - 4] | ((uint32_t)b[n - 3] << 8) | ((uint32_t)b[n - 2] << 16) | ((uint32_t)b[n - 1] << 24);
}

}  // namespace

const char* ResumeStore::key(Src s) { return kKeys[ix(s)]; }

ResumeStore::Load ResumeStore::load(Src s, Resume::Record& r, Resume::ParseErr* why) {
  const int i = ix(s);
  if (why) *why = Resume::ParseErr::Ok;
  size_t n = NvsStore::blobLen(kKeys[i]);
  if (n == 0) {
    s_have[i] = false;
    return Load::None;
  }
  size_t got = 0;
  Resume::ParseErr e = Resume::ParseErr::Short;
  if (n <= sizeof s_buf && NvsStore::getBlob(kKeys[i], s_buf, sizeof s_buf, got)) e = Resume::parse(s_buf, got, r);
  else if (n > sizeof s_buf) e = Resume::ParseErr::Len;
  if (e != Resume::ParseErr::Ok) {
    if (why) *why = e;
    s_have[i] = false;  // у NVS сміття: наступний запис його перезапише
    LOGW(TAG, "%s: запис пошкоджено (%s), %u Б", kKeys[i], Resume::errName(e), (unsigned)n);
    return Load::Bad;
  }
  s_crc[i] = blobCrc(s_buf, got);
  s_have[i] = true;
  return Load::Ok;
}

bool ResumeStore::save(Src s, const char* path, uint32_t off, uint32_t ms) {
  const int i = ix(s);
  size_t n = Resume::serialize(path, path ? strlen(path) : 0, off, ms, s_buf, sizeof s_buf);
  if (!n) {
    LOGW(TAG, "%s: шлях порожній або довший за %u Б — не зберігається", kKeys[i], (unsigned)Resume::kPathMax);
    return false;
  }
  uint32_t crc = blobCrc(s_buf, n);
  if (s_have[i] && s_crc[i] == crc) return true;  // не змінилось — флеш не чіпаємо
  if (!NvsStore::putBlob(kKeys[i], s_buf, n)) {
    s_have[i] = false;  // стан NVS невідомий — наступна спроба напише знову
    LOGW(TAG, "%s: помилка запису в NVS (%u Б)", kKeys[i], (unsigned)n);
    return false;
  }
  s_crc[i] = crc;
  s_have[i] = true;
  return true;
}

bool ResumeStore::erase(Src s) {
  const int i = ix(s);
  s_have[i] = false;
  bool ok = NvsStore::remove(kKeys[i]);
  if (!ok) LOGW(TAG, "%s: не вдалося стерти", kKeys[i]);
  return ok;
}

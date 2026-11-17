#include "usb/usb_volume_diag.h"

#include "config.h"

#if ENABLE_USB_CH376

#include <Arduino.h>
#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include <new>

#include "media/id3.h"
#include "usb/ch376_sdfat.h"
#include "usb/usb_volume.h"
#include "util/log.h"

namespace {

constexpr const char* TAG = "usb";

bool toLong(const char* s, long& v) {
  if (!s || !*s) return false;
  char* e = nullptr;
  v = strtol(s, &e, 10);
  return *e == 0;
}

uint32_t kbps10(uint32_t bytes, uint32_t us) {
  return us ? (uint32_t)((uint64_t)bytes * 10000000ull / (1024ull * us)) : 0u;
}

void memLine(const char* when) {
  const UBaseType_t hw = uxTaskGetStackHighWaterMark(nullptr);  // ESP-IDF FreeRTOS: уже в байтах (множник ×4 завищував значення)
  LOGI(TAG, "mem %s: heap вільно %u, найбільший блок %u | стек %s: мін. вільно %u Б", when, (unsigned)ESP.getFreeHeap(),
       (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT), pcTaskGetName(nullptr), (unsigned)hw);
}

bool needVol() {
  if (UsbVolume::get().isOpen()) return true;
  LOGW(TAG, "том не відкрито: спершу DBG,usb,mount");
  return false;
}

void goneLine() {
  if (!UsbVolume::get().present())
    LOGW(TAG, "пристрій ВІДСУТНІЙ (флешку витягнуто або збій): DBG,usb,umount, після повернення — DBG,usb,mount");
}

// Довге імʼя ріжемо шматками ≤ kUsbLogChunk байтів по межі UTF-8 символу (рядок логу ≤ 192 Б).
void logName(const char* head, const char* name) {
  const size_t len = strlen(name);
  size_t p = 0;
  bool first = true;
  do {
    size_t n = len - p < kUsbLogChunk ? len - p : kUsbLogChunk;
    if (p + n < len)
      while (n > 1 && ((uint8_t)name[p + n] & 0xC0) == 0x80) n--;
    char piece[kUsbLogChunk + 1];
    memcpy(piece, name + p, n);
    piece[n] = 0;
    LOGI(TAG, "%s%s%s", first ? head : "      … ", piece, p + n < len ? " …" : "");
    p += n;
    first = false;
  } while (p < len);
}

// ---- mount / umount ----
void cmdMount() {
  UsbVolume& v = UsbVolume::get();
  memLine("до open");
  UsbVolInfo r;
  if (!v.open(r)) {
    memLine("після відмови");
    return;
  }
  LOGI(TAG, "mount: ГОТОВО за %u мс, %u Бод (самоперевірка: %s)", (unsigned)r.openMs, (unsigned)r.baud, selfTestName(r.selfTest));
  LOGI(TAG, "mount: розділ %u: початок LBA %u, %u секторів (%u МБ)", (unsigned)r.partIndex, (unsigned)r.partStart,
       (unsigned)r.partSectors, (unsigned)(r.partSectors / 2048u));
  LOGI(TAG, "mount: ФС %s, мітка '%s', кластер %u Б", r.fsType == 64 ? "exFAT" : (r.fsType == 32 ? "FAT32" : (r.fsType == 16 ? "FAT16" : "?")),
       r.label, (unsigned)r.clusterBytes);
  memLine("після open");
}

void cmdUmount() {
  UsbVolume& v = UsbVolume::get();
  memLine("до close");
  const uint32_t t0 = millis();
  v.close();
  LOGI(TAG, "umount: том закрито, чіп скинуто, порт закрито (%u мс)", (unsigned)(millis() - t0));
  memLine("після close");
}

// ---- vls ----
struct LsCtx {
  uint32_t dirs = 0, files = 0;
};

void lsCb(const UsbDirEntry& e, void* c) {
  LsCtx* x = static_cast<LsCtx*>(c);
  if (e.isDir) x->dirs++;
  else x->files++;
  char head[40];
  snprintf(head, sizeof head, "%c%c %10u  ", e.isDir ? 'D' : 'F', e.hidden ? 'h' : ' ', (unsigned)e.size);
  if (!e.nameOk) LOGI(TAG, "%s<імʼя не прочитано або задовге>", head);
  else logName(head, e.name);
}

void cmdVls(const char* dir, uint32_t n) {
  if (!needVol()) return;
  memLine("до vls");
  LsCtx x;
  uint32_t cnt = 0;
  bool more = false;
  const UsbLsRes r = UsbVolume::get().listDir(dir, n, lsCb, &x, cnt, more);
  switch (r) {
    case UsbLsRes::Ok:
      LOGI(TAG, "vls %s: записів %u (каталогів %u, файлів %u)%s", dir, (unsigned)cnt, (unsigned)x.dirs, (unsigned)x.files,
           more ? ", Є ЩЕ (збільш N)" : "");
      break;
    case UsbLsRes::OpenFail: LOGW(TAG, "vls: каталог '%s' не відкрито", dir); break;
    case UsbLsRes::NotDir: LOGW(TAG, "vls: '%s' — не каталог", dir); break;
    case UsbLsRes::Gone: goneLine(); break;
    case UsbLsRes::NoMemory: LOGW(TAG, "vls: немає памʼяті"); break;
    case UsbLsRes::NoVolume: break;
  }
  memLine("після vls");
}

// ---- vread ----
void cmdVread(const char* path, uint32_t kb) {
  if (!needVol()) return;
  UsbVolume& v = UsbVolume::get();
  UsbFileReader& rd = v.reader();
  uint8_t* buf = static_cast<uint8_t*>(malloc(kUsbIoChunk));
  if (!buf) {
    LOGW(TAG, "vread: немає памʼяті");
    return;
  }
  memLine("до vread");
  v.blockDev()->disk().resetStats();
  if (!rd.open(path)) {
    LOGW(TAG, "vread: файл '%s' не відкрито", path);
    goneLine();
    free(buf);
    return;
  }
  const uint64_t want = (uint64_t)kb * 1024ull;
  const uint32_t target = want > rd.size() ? rd.size() : (uint32_t)want;
  LOGI(TAG, "vread: %u Б із %u, шматок %u Б, %u Бод", (unsigned)target, (unsigned)rd.size(), (unsigned)kUsbIoChunk,
       (unsigned)v.openInfo()->baud);
  uint32_t got = 0, k = 0, mn = 0xFFFFFFFFu, mx = 0, sum = 0;
  bool cut = false;
  const uint32_t t0 = micros();
  while (got < target) {
    const size_t w = target - got < kUsbIoChunk ? (size_t)(target - got) : kUsbIoChunk;
    const uint32_t ts = micros();
    const size_t n = rd.read(buf, w);
    const uint32_t dt = (uint32_t)(micros() - ts);
    if (dt < mn) mn = dt;
    if (dt > mx) mx = dt;
    sum += dt;
    got += (uint32_t)n;
    k++;
    if (n < w) break;
    if ((uint32_t)(micros() - t0) >= kUsbReadMeasMs * 1000u) {
      cut = true;
      break;
    }
  }
  const uint32_t tot = (uint32_t)(micros() - t0);
  const bool bad = rd.failed();
  rd.close();
  const uint32_t k10 = kbps10(got, tot);
  LOGI(TAG, "vread: %u Б за %u мс = %u.%u КБ/с%s, шматків %u", (unsigned)got, (unsigned)(tot / 1000u), (unsigned)(k10 / 10u),
       (unsigned)(k10 % 10u), cut ? " (ПЕРЕРВАНО за стелею часу)" : "", (unsigned)k);
  LOGI(TAG, "vread: час шматка мін/сер/макс %u/%u/%u мкс", (unsigned)(k ? mn : 0), (unsigned)(k ? sum / k : 0), (unsigned)mx);
  const BlockDevStats& s = v.blockDev()->stats();
  LOGI(TAG, "vread: блоковий пристрій: викликів %u, секторів %u, команд %u, збоїв %u; записів SdFat %u", (unsigned)s.calls,
       (unsigned)s.sectors, (unsigned)s.cmds, (unsigned)s.errors, (unsigned)v.sdDevice()->writeAttempts());
  if (bad || (got < target && !cut)) {
    LOGW(TAG, "vread: ПОМИЛКА читання на %u Б", (unsigned)got);
    goneLine();
  }
  free(buf);
  memLine("після vread");
}

// ---- vseq ----
void cmdVseq(const char* path, uint32_t kb) {
  if (!needVol()) return;
  UsbVolume& v = UsbVolume::get();
  UsbFileReader& rd = v.reader();
  uint8_t* buf = static_cast<uint8_t*>(malloc(kCh376SeqChunk));
  if (!buf) {
    LOGW(TAG, "vseq: немає памʼяті");
    return;
  }
  v.blockDev()->disk().resetStats();
  if (!rd.open(path)) {
    LOGW(TAG, "vseq: файл '%s' не відкрито", path);
    goneLine();
    free(buf);
    return;
  }
  const uint64_t want = kb == 0 ? (uint64_t)rd.size() : (uint64_t)kb * 1024ull;
  const uint32_t target = want > rd.size() ? rd.size() : (uint32_t)want;
  LOGI(TAG, "vseq %s: %u Б, шматок %u Б / %u мкс (≈40 КБ/с), %u Бод, ≈%u с", path, (unsigned)target, (unsigned)kCh376SeqChunk,
       (unsigned)kCh376SeqPeriodUs, (unsigned)v.openInfo()->baud, (unsigned)((uint64_t)target / kCh376SeqChunk * kCh376SeqPeriodUs / 1000000u));
  uint32_t got = 0, k = 0, worst = 0, sum = 0, over = 0, late = 0, nextRep = 30000;
  int32_t maxLag = 0;
  bool cut = false;
  const uint32_t t0 = micros();
  while (got < target) {
    const uint32_t due = k * kCh376SeqPeriodUs;
    for (;;) {
      const uint32_t el = (uint32_t)(micros() - t0);
      if (el >= due) break;
      if (due - el > 1500) delay(1);
    }
    const size_t w = target - got < kCh376SeqChunk ? (size_t)(target - got) : kCh376SeqChunk;
    const uint32_t ts = micros();
    const size_t n = rd.read(buf, w);
    const uint32_t te = micros();
    const uint32_t dt = te - ts;
    if (dt > worst) worst = dt;
    if (dt > kCh376SeqPeriodUs) over++;
    sum += dt;
    got += (uint32_t)n;
    k++;
    const int32_t lag = (int32_t)((uint32_t)(te - t0) - k * kCh376SeqPeriodUs);  // >0: шматок готовий пізніше за початок наступного кадру
    if (lag > maxLag) maxLag = lag;
    if (lag > 0) late++;
    if (n < w) break;
    const uint32_t el = (uint32_t)(te - t0) / 1000u;
    if (el >= kUsbSeqMeasMs) {
      cut = true;
      break;
    }
    if (el >= nextRep) {
      LOGI(TAG, "vseq: %u с, %u з %u Б, макс. відставання %d мкс", (unsigned)(el / 1000u), (unsigned)got, (unsigned)target, (int)maxLag);
      nextRep += 30000;
    }
  }
  const uint32_t tot = (uint32_t)(micros() - t0);
  const bool bad = rd.failed();
  rd.close();
  const BlockDevStats& s = v.blockDev()->stats();
  LOGI(TAG, "vseq: шматків %u, %u Б за %u мс%s", (unsigned)k, (unsigned)got, (unsigned)(tot / 1000u), cut ? " (ПЕРЕРВАНО за стелею часу)" : "");
  LOGI(TAG, "vseq: читання шматка сер/макс %u/%u мкс (період %u), довших за період: %u", (unsigned)(k ? sum / k : 0), (unsigned)worst,
       (unsigned)kCh376SeqPeriodUs, (unsigned)over);
  LOGI(TAG, "vseq: макс. відставання %d мкс (допуск %u), запізнілих %u, збоїв чіпа %u", (int)maxLag, (unsigned)kCh376SeqLagOkUs,
       (unsigned)late, (unsigned)s.errors);
  LOGI(TAG, "vseq: викликів %u, секторів %u, команд %u (%u.%02u команди на шматок)", (unsigned)s.calls, (unsigned)s.sectors,
       (unsigned)s.cmds, (unsigned)(k ? s.cmds / k : 0), (unsigned)(k ? (s.cmds * 100u / k) % 100u : 0));
  if (bad || (got < target && !cut)) {
    LOGW(TAG, "vseq: ПОМИЛКА читання на %u Б", (unsigned)got);
    goneLine();
  } else {
    LOGI(TAG, "vseq: потік 40 КБ/с %s", (maxLag <= (int32_t)kCh376SeqLagOkUs && !cut) ? "ТРИМАЄТЬСЯ" : "НЕ ТРИМАЄТЬСЯ");
  }
  free(buf);
}

// ---- vtag ----
void cmdVtag(const char* path) {
  if (!needVol()) return;
  UsbFileReader& rd = UsbVolume::get().reader();
  // Скретч, теги й діагностика — одним блоком heap (на стеку їх нема).
  constexpr size_t kScr = Id3::kId3ScratchMin;
  uint8_t* scr = static_cast<uint8_t*>(malloc(kScr));
  Id3::Tags* tags = new (std::nothrow) Id3::Tags;
  Id3::Info* inf = new (std::nothrow) Id3::Info;
  if (!scr || !tags || !inf) {
    LOGW(TAG, "vtag: немає памʼяті");
  } else if (!rd.open(path)) {
    LOGW(TAG, "vtag: файл '%s' не відкрито", path);
    goneLine();
  } else {
    tags->clear();
    const uint32_t t0 = micros();
    const uint8_t found = Id3::parse(rd, scr, kScr, *tags, inf);
    const uint32_t dt = (uint32_t)(micros() - t0);
    LOGI(TAG, "vtag: розмір %u Б, знайдено: %s%s, %u мс, прочитано %u Б, v2.%u кадрів %u тег %u Б%s", (unsigned)rd.size(),
         (found & Id3::kFoundV2) ? "ID3v2 " : "", (found & Id3::kFoundV1) ? "ID3v1 (з кінця файлу)" : "", (unsigned)(dt / 1000u),
         (unsigned)inf->readBytes, (unsigned)inf->ver, (unsigned)inf->frames, (unsigned)inf->tagBytes, inf->cut ? " [урізано]" : "");
    logName("vtag: виконавець: ", tags->artist);
    logName("vtag: назва: ", tags->title);
    logName("vtag: альбом: ", tags->album);
    if (rd.failed()) {
      LOGW(TAG, "vtag: помилка читання");
      goneLine();
    }
    rd.close();
  }
  delete inf;
  delete tags;
  free(scr);
}

void usage() {
  LOGW(TAG, "DBG,usb,mount | umount | vls[,<каталог>[,N]] | vread,<шлях>[,<КБ>] | vseq,<шлях>[,<КБ>, 0 = весь файл] | vtag,<шлях>");
}

}  // namespace

bool UsbVolDiag::handle(const proto::Msg& m) {
  if (m.argc < 2) return false;
  const char* s = m.args[1];
  if (!strcasecmp(s, "mount")) {
    if (m.argc != 2) usage();
    else cmdMount();
  } else if (!strcasecmp(s, "umount")) {
    if (m.argc != 2) usage();
    else cmdUmount();
  } else if (!strcasecmp(s, "vls")) {
    long n = (long)kUsbLsDef;
    if (m.argc > 4 || (m.argc == 4 && (!toLong(m.args[3], n) || n < 1 || n > (long)kUsbLsMax))) {
      LOGW(TAG, "DBG,usb,vls[,<каталог>[,N 1..%u]]", (unsigned)kUsbLsMax);
      return true;
    }
    cmdVls(m.argc >= 3 ? m.args[2] : "/", (uint32_t)n);
  } else if (!strcasecmp(s, "vread") || !strcasecmp(s, "vseq")) {
    const bool seq = !strcasecmp(s, "vseq");
    long kb = (long)(seq ? kUsbSeqKbDef : kUsbReadKbDef);
    if (m.argc < 3 || m.argc > 4 || (m.argc == 4 && (!toLong(m.args[3], kb) || kb < (seq ? 0 : 1) || kb > (long)kUsbKbMax))) {
      LOGW(TAG, "DBG,usb,%s,<шлях>[,<КБ %d..%u>]", s, seq ? 0 : 1, (unsigned)kUsbKbMax);
      return true;
    }
    if (seq) cmdVseq(m.args[2], (uint32_t)kb);
    else cmdVread(m.args[2], (uint32_t)kb);
  } else if (!strcasecmp(s, "vtag")) {
    if (m.argc != 3) usage();
    else cmdVtag(m.args[2]);
  } else {
    return false;
  }
  return true;
}

#endif  // ENABLE_USB_CH376
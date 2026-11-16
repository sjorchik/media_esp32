#include "usb/ch376_diag_disk.h"

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

#include "usb/ch376_disk.h"
#include "usb/ch376_uart.h"
#include "usb/disk_probe.h"
#include "util/log.h"

namespace {

constexpr const char* TAG = "usb";

// Один шар секторів на весь драйвер. Звʼязок — той самий, що в ch376_uart (статичний обʼєкт).
Ch376Disk& disk() {
  static Ch376Disk d(ch376_uart::link());
  return d;
}

bool toLong(const char* s, long& v, int base) {
  if (!s || !*s) return false;
  char* e = nullptr;
  v = strtol(s, &e, base);
  return *e == 0;
}

// «<lba>» або «<lba>+<зсув байт>»: зсув переводиться в сектори + залишок (старт не з межі сектора).
bool parseStart(const char* s, uint32_t& base, uint32_t& inOff) {
  if (!s || !*s) return false;
  char buf[24];
  strncpy(buf, s, sizeof buf - 1);
  buf[sizeof buf - 1] = 0;
  char* plus = strchr(buf, '+');
  long lba = 0, off = 0;
  if (plus) {
    *plus = 0;
    if (!toLong(plus + 1, off, 10) || off < 0 || off > 1000000) return false;
  }
  if (!toLong(buf, lba, 10) || lba < 0) return false;
  base = (uint32_t)lba + (uint32_t)(off / 512);
  inOff = (uint32_t)(off % 512);
  return true;
}

void hexLine(const uint8_t* d, size_t n, char* b, size_t cap) {
  size_t p = 0;
  for (size_t i = 0; i < n && p + 4 < cap; i++) {
    int w = snprintf(b + p, cap - p, i ? " %02X" : "%02X", (unsigned)d[i]);
    if (w < 0) break;
    p += (size_t)w;
  }
}

// КБ/с ×10 (те саме, що в ch376_diag: байти / (1024 · мкс)).
uint32_t kbps10(uint32_t bytes, uint32_t us) {
  return us ? (uint32_t)((uint64_t)bytes * 10000000ull / (1024ull * us)) : 0u;
}

bool needStarted() {
  if (ch376_uart::started()) return true;
  LOGW(TAG, "драйвер не запущено: спершу DBG,usb,init[,<бод>|auto]");
  return false;
}

void errLine(const char* what, DiskErr e) {
  LOGW(TAG, "%s: %s (статус 0x%02X)", what, diskErrName(e), (unsigned)disk().lastStatus());
  if (e == DiskErr::Disconnected || e == DiskErr::NoDisk) LOGW(TAG, "флешку не знайдено або відʼєднано; після повернення — DBG,usb,init");
}

// Пам'ять для логу: heap (вільно / найбільший блок) і запас стека поточної задачі.
void memLine(const char* when) {
  const UBaseType_t hw = uxTaskGetStackHighWaterMark(nullptr);
  LOGI(TAG, "mem %s: heap вільно %u, найбільший блок %u | стек %s: мін. вільно %u Б", when, (unsigned)ESP.getFreeHeap(),
       (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT), pcTaskGetName(nullptr), (unsigned)(hw * 4u));
}

const char* waitMode() { return disk().useInt() && ch376_uart::fs().intUsable() ? "INT" : "опитування"; }

// Читач секторів для розбору структур: один сектор через Ch376Disk.
class DiskReader : public SectorReader {
 public:
  bool readSector(uint32_t lba, uint8_t* dst) override {
    last_ = disk().readSectors(lba, 1, dst);
    return last_ == DiskErr::Ok;
  }
  DiskErr lastErr() const { return last_; }

 private:
  DiskErr last_ = DiskErr::Ok;
};

// DBG,usb,rawread,<lba>[,<n>]: сирі сектори, перші 64 Б першого сектора, контрольна сума FNV-1a, час.
void cmdRawRead(uint32_t lba, uint32_t n) {
  if (!needStarted()) return;
  uint32_t cap = 0;
  DiskErr e = disk().capacity(cap);
  if (e != DiskErr::Ok) return errLine("ємність", e);
  if ((uint64_t)lba + n > cap) {
    LOGW(TAG, "rawread: LBA %u+%u поза ємністю (%u секторів)", (unsigned)lba, (unsigned)n, (unsigned)cap);
    return;
  }
  uint8_t* buf = static_cast<uint8_t*>(malloc((size_t)n * kCh376DiskSecBytes));  // тимчасово на heap, звільняється нижче
  if (!buf) {
    LOGW(TAG, "rawread: немає памʼяті на %u Б", (unsigned)(n * kCh376DiskSecBytes));
    return;
  }
  memLine("до");
  disk().resetStats();
  const uint32_t t0 = micros();
  e = disk().readSectors(lba, n, buf);
  const uint32_t dt = (uint32_t)(micros() - t0);
  if (e != DiskErr::Ok) {
    errLine("rawread", e);
    free(buf);
    return;
  }
  uint32_t h = 2166136261u;  // FNV-1a по всіх прочитаних байтах
  for (uint32_t i = 0; i < n * kCh376DiskSecBytes; i++) {
    h ^= buf[i];
    h *= 16777619u;
  }
  char hb[32 * 3 + 1];
  hexLine(buf, 32, hb, sizeof hb);
  LOGI(TAG, "rawread %u+%u: %u Б за %u мкс, FNV-1a %08X", (unsigned)lba, (unsigned)n, (unsigned)(n * 512u),
       (unsigned)dt, (unsigned)h);
  LOGI(TAG, "rawread 0..31: %s", hb);
  hexLine(buf + 32, 32, hb, sizeof hb);
  LOGI(TAG, "rawread 32..63: %s", hb);
  free(buf);
  memLine("після");
}

// DBG,usb,part: розбір MBR / захисного GPT / BPB / exFAT і підсумок придатності.
void cmdPart() {
  if (!needStarted()) return;
  uint32_t cap = 0;
  DiskErr e = disk().capacity(cap);
  if (e != DiskErr::Ok) return errLine("ємність", e);
  LOGI(TAG, "part: ємність %u секторів (%u МБ)", (unsigned)cap, (unsigned)(cap / 2048u));
  uint8_t* sc = static_cast<uint8_t*>(malloc(kCh376DiskSecBytes));
  if (!sc) {
    LOGW(TAG, "part: немає памʼяті");
    return;
  }
  DiskReader rd;
  ProbeReport r;
  const bool ok = probeDisk(rd, sc, r);
  free(sc);
  if (!ok) {
    LOGW(TAG, "part: сектор 0 не прочитано (%s)", diskErrName(rd.lastErr()));
    return;
  }
  if (r.mbr) {
    LOGI(TAG, "part: MBR, розділів %u, захисний GPT %s, заголовок GPT %s", (unsigned)r.partCount,
         r.protectiveGpt ? "так" : "ні", r.gptHeader ? "так" : "ні");
    for (int i = 0; i < 4; i++) {
      if (r.parts[i].type == 0) continue;
      LOGI(TAG, "part: розділ %d тип 0x%02X, початок LBA %u, секторів %u", i + 1, (unsigned)r.parts[i].type,
           (unsigned)r.parts[i].startLba, (unsigned)r.parts[i].sectors);
    }
  } else {
    LOGI(TAG, "part: таблиці розділів немає (суцільна флешка)");
  }
  if (r.protectiveGpt) LOGI(TAG, "part: GPT поки НЕ підтримано; розібрано перший розділ GPT (якщо знайдено)");
  if (r.kind == VolKind::ExFat) {
    LOGI(TAG, "part: том з LBA %u: exFAT, кластерів %u, корінь кластер %u, сектор %u Б",
         (unsigned)r.volStart, (unsigned)r.ex.clusterCount, (unsigned)r.ex.rootCluster,
         (unsigned)(1u << r.ex.bytesPerSecShift));
  } else if (r.kind == VolKind::Fat12 || r.kind == VolKind::Fat16 || r.kind == VolKind::Fat32) {
    LOGI(TAG, "part: том з LBA %u: %s, Б/сектор %u, reserved %u (повне поле; чіп бачив би лише молодший байт)",
         (unsigned)r.volStart, volKindName(r.kind), (unsigned)r.bpb.bytesPerSec, (unsigned)r.bpb.reserved);
    LOGI(TAG, "part: секторів/кластер %u, FAT %u × %u, кластерів %u, мітка '%s'", (unsigned)r.bpb.secPerClus,
         (unsigned)r.bpb.numFats, (unsigned)r.bpb.fatSz, (unsigned)r.bpb.clusters, r.bpb.label);
  } else {
    LOGI(TAG, "part: том з LBA %u: %s", (unsigned)r.volStart, volKindName(r.kind));
  }
  LOGI(TAG, "part: придатність до майбутнього читача FAT: %s", r.readable ? "ТАК (FAT, 512 Б/сектор)" : "НІ");
  memLine("після");
}

// Спільний режим послідовного читання для rawseq/rawpre: споживання потоку «як у плеєра».
struct SeqCfg {
  uint32_t base;      // перший сектор (lba + зсув/512)
  uint32_t inOff;     // зсув першого байта в першому секторі, 0..511
  uint32_t chunk;     // байтів на кадр
  uint32_t periodUs;  // період кадру, мкс
  uint32_t preBytes;  // випереджальний пакет, Б (0 — без нього)
  uint32_t preEvery;  // кадрів між пакетами (1 — щокадру)
};

void waitUntil(uint32_t t0, uint32_t dueUs) {
  for (;;) {
    const uint32_t el = (uint32_t)(micros() - t0);
    if (el >= dueUs) return;
    if (dueUs - el > 1500) delay(1);  // крок пейсингу, не гаряча петля очікування чіпа
  }
}

void runSeq(const char* title, const SeqCfg& c, uint32_t kb) {
  if (!needStarted()) return;
  uint32_t cap = 0;
  DiskErr e = disk().capacity(cap);
  if (e != DiskErr::Ok) return errLine("ємність", e);
  const uint32_t total = kb * 1024u;
  const uint32_t frames = (total + c.chunk - 1u) / c.chunk;
  const uint64_t endSec = ((uint64_t)c.base * 512u + c.inOff + (uint64_t)(frames + 2u) * c.chunk + c.preBytes) / 512u + 2u;
  if (endSec > cap) {
    LOGW(TAG, "%s: за межами ємності (%u секторів)", title, (unsigned)cap);
    return;
  }
  disk().resetStats();
  LOGI(TAG, "%s: %u КБ, шматок %u Б / %u мкс, %u Бод, сект./команду %u, очікування %s", title, (unsigned)kb,
       (unsigned)c.chunk, (unsigned)c.periodUs, (unsigned)ch376_uart::driver().baud(), (unsigned)disk().secPerCmd(),
       waitMode());
  if (c.preBytes) LOGI(TAG, "%s: випереджальний пакет %u Б раз на %u кадр.", title, (unsigned)c.preBytes, (unsigned)c.preEvery);

  uint32_t loaded = 0;              // секторів від base уже в буфері
  uint32_t hist[5] = {0, 0, 0, 0, 0};  // розподіл секторів на кадр: 0, 1, 2, 3, інше
  uint32_t sumUs = 0, worstUs = 0, late = 0, secTotal = 0, framesDone = 0;
  int32_t maxLag = 0;
  DiskErr fe = DiskErr::Ok;
  bool failed = false;
  const uint32_t t0 = micros();
  for (uint32_t k = 0; k < frames && !failed; k++) {
    waitUntil(t0, k * c.periodUs);
    // Вимоги: кадр k (до його початку, дедлайн 0 для k=0) і кадр k+1 (дедлайн початку k+1).
    struct Req {
      uint64_t bytes;
      uint32_t dl;
    } reqs[2];
    int nr = 0;
    if (k == 0) reqs[nr++] = Req{(uint64_t)c.inOff + c.chunk, 0};
    uint64_t need = (uint64_t)c.inOff + (uint64_t)(k + 2) * c.chunk;
    if (c.preBytes && k % c.preEvery == 0) need += c.preBytes;
    reqs[nr++] = Req{need, (k + 1) * c.periodUs};

    uint32_t secsThis = 0;
    const uint32_t ts = micros();
    for (int r = 0; r < nr && !failed; r++) {
      const uint32_t needSec = (uint32_t)((reqs[r].bytes + 511u) / 512u);
      if (needSec <= loaded) continue;
      const uint32_t n = needSec - loaded;
      e = disk().readSectors(c.base + loaded, n, nullptr);
      if (e != DiskErr::Ok) {
        failed = true;
        fe = e;
        break;
      }
      loaded = needSec;
      secsThis += n;
      const uint32_t te = micros();
      const int32_t lag = (int32_t)(te - (t0 + reqs[r].dl));
      if (lag > maxLag) maxLag = lag;
      if (lag > 0) late++;
    }
    if (failed) break;
    const uint32_t dt = (uint32_t)(micros() - ts);
    sumUs += dt;
    if (dt > worstUs) worstUs = dt;
    secTotal += secsThis;
    hist[secsThis <= 3 ? secsThis : 4]++;
    framesDone++;
  }
  const uint32_t tot = (uint32_t)(micros() - t0);
  const DiskStats& s = disk().stats();
  const uint32_t avgUs = framesDone ? sumUs / framesDone : 0;
  LOGI(TAG, "%s: кадрів %u, секторів %u (сер. %u.%02u на кадр), %u мс", title, (unsigned)framesDone, (unsigned)secTotal,
       (unsigned)(framesDone ? secTotal / framesDone : 0),
       (unsigned)(framesDone ? (secTotal * 100u / framesDone) % 100u : 0), (unsigned)(tot / 1000u));
  LOGI(TAG, "%s: розподіл секторів/кадр: 0:%u 1:%u 2:%u 3:%u інше:%u", title, (unsigned)hist[0], (unsigned)hist[1],
       (unsigned)hist[2], (unsigned)hist[3], (unsigned)hist[4]);
  LOGI(TAG, "%s: читання на кадр сер/макс %u/%u мкс = %u%% періоду %u мкс", title, (unsigned)avgUs, (unsigned)worstUs,
       (unsigned)(c.periodUs ? avgUs * 100u / c.periodUs : 0), (unsigned)c.periodUs);
  LOGI(TAG, "%s: відставання макс %d мкс (допуск %u), запізнілих %u, помилок %u", title, (int)maxLag,
       (unsigned)kCh376SeqLagOkUs, (unsigned)late, (unsigned)s.errors);
  LOGI(TAG, "%s: блоків %u, команд %u, блок мін/сер/макс %u/%u/%u мкс", title, (unsigned)s.blocks, (unsigned)s.cmds,
       (unsigned)(s.blocks ? s.blkMinUs : 0), (unsigned)(s.blocks ? s.blkSumUs / s.blocks : 0), (unsigned)s.blkMaxUs);
  LOGI(TAG, "%s: на блок сер.: очікування %u, статус %u, дані %u мкс", title,
       (unsigned)(s.blocks ? s.waitUs / s.blocks : 0), (unsigned)(s.blocks ? s.statUs / s.blocks : 0),
       (unsigned)(s.blocks ? s.xferUs / s.blocks : 0));
  if (failed) errLine(title, fe);
  else
    LOGI(TAG, "%s: потік 40 КБ/с %s", title,
         (maxLag <= (int32_t)kCh376SeqLagOkUs && !failed) ? "ТРИМАЄТЬСЯ" : "НЕ ТРИМАЄТЬСЯ");
}

// DBG,usb,rawspeed,<lba>,<КБ>[,<N>]: послідовне читання N секторів на команду, без обробки даних.
void cmdRawSpeed(uint32_t lba, uint32_t kb, uint32_t n) {
  if (!needStarted()) return;
  uint32_t cap = 0;
  DiskErr e = disk().capacity(cap);
  if (e != DiskErr::Ok) return errLine("ємність", e);
  const uint32_t secs = kb * 2u;  // КБ·1024/512
  if ((uint64_t)lba + secs > cap) {
    LOGW(TAG, "rawspeed: %u КБ від LBA %u виходить за ємність (%u секторів)", (unsigned)kb, (unsigned)lba, (unsigned)cap);
    return;
  }
  disk().resetStats();
  const uint32_t prevSpc = disk().secPerCmd();  // N — параметр цієї команди; після неї повертаємо налаштування rawcfg
  disk().setSecPerCmd(n);
  uint32_t done = 0;
  const uint32_t t0 = micros();
  bool cut = false;
  while (done < secs) {
    const uint32_t k = (secs - done) < n ? (secs - done) : n;
    e = disk().readSectors(lba + done, k, nullptr);
    if (e != DiskErr::Ok) break;
    done += k;
    if ((uint32_t)(micros() - t0) >= kCh376DiagMeasMs * 1000u) {
      cut = true;
      break;
    }
  }
  const uint32_t us = (uint32_t)(micros() - t0);
  disk().setSecPerCmd(prevSpc);
  const DiskStats& s = disk().stats();
  const uint32_t bytes = done * kCh376DiskSecBytes;
  const uint32_t kb10 = kbps10(bytes, us);
  LOGI(TAG, "rawspeed %u+%u: %u КБ за %u мс = %u.%u КБ/с, N=%u сект./команду, %u Бод, очікування %s%s",
       (unsigned)lba, (unsigned)secs, (unsigned)(bytes / 1024u), (unsigned)(us / 1000u), (unsigned)(kb10 / 10u),
       (unsigned)(kb10 % 10u), (unsigned)n, (unsigned)ch376_uart::driver().baud(), waitMode(),
       cut ? " (ПЕРЕРВАНО за стелею часу)" : "");
  LOGI(TAG, "rawspeed: команд %u, команда сер/макс %u/%u мкс, блок мін/сер/макс %u/%u/%u мкс", (unsigned)s.cmds,
       (unsigned)(s.cmds ? s.cmdSumUs / s.cmds : 0), (unsigned)s.cmdMaxUs, (unsigned)(s.blocks ? s.blkMinUs : 0),
       (unsigned)(s.blocks ? s.blkSumUs / s.blocks : 0), (unsigned)s.blkMaxUs);
  LOGI(TAG, "rawspeed: на блок сер.: очікування %u, статус %u, дані %u мкс | опитувань %u, INT %u, запасних %u",
       (unsigned)(s.blocks ? s.waitUs / s.blocks : 0), (unsigned)(s.blocks ? s.statUs / s.blocks : 0),
       (unsigned)(s.blocks ? s.xferUs / s.blocks : 0), (unsigned)s.polls, (unsigned)s.intWaits, (unsigned)s.intMisses);
  if (e != DiskErr::Ok) errLine("rawspeed", e);
}

// DBG,usb,rawcfg[,settle,<0..16>|skip,<0|1>|wait,<int|poll>|spc,<1..255>]: поточні налаштування або зміна.
void printCfg() {
  LOGI(TAG, "rawcfg: очікування %s, пауза після статусу %u б.інт., пропуск статусу %s, сект./команду %u",
       waitMode(), (unsigned)disk().settleBytes(), disk().skipStatus() ? "так" : "ні", (unsigned)disk().secPerCmd());
}

void cmdRawCfg(const proto::Msg& m) {
  if (m.argc == 2) {
    printCfg();
    return;
  }
  if (m.argc != 4) {
    LOGW(TAG, "DBG,usb,rawcfg[,settle,<0..16>|skip,<0|1>|wait,<int|poll>|spc,<1..255>]");
    return;
  }
  long v = 0;
  const char* k = m.args[2];
  bool ok = false;
  if (!strcasecmp(k, "wait")) {
    if (!strcasecmp(m.args[3], "int")) { disk().setUseInt(true); ok = true; }
    else if (!strcasecmp(m.args[3], "poll")) { disk().setUseInt(false); ok = true; }
  } else if (!strcasecmp(k, "settle") && toLong(m.args[3], v, 10) && v >= 0 && v <= 16) {
    disk().setSettleBytes((uint32_t)v);
    ok = true;
  } else if (!strcasecmp(k, "skip") && toLong(m.args[3], v, 10) && (v == 0 || v == 1)) {
    disk().setSkipStatus(v == 1);
    ok = true;
  } else if (!strcasecmp(k, "spc") && toLong(m.args[3], v, 10) && v >= 1 && v <= (long)kCh376DiskMaxSectors) {
    disk().setSecPerCmd((uint32_t)v);
    ok = true;
  }
  if (!ok) {
    LOGW(TAG, "rawcfg: некоректний аргумент (DBG,usb,rawcfg[,settle,<0..16>|skip,<0|1>|wait,<int|poll>|spc,<1..255>])");
    return;
  }
  printCfg();
}

}  // namespace

namespace Ch376DiagDisk {

bool handle(const proto::Msg& m) {
  if (m.argc < 2) return false;
  const char* s = m.args[1];
  long v = 0;
  if (!strcasecmp(s, "rawread") && (m.argc == 3 || m.argc == 4)) {
    uint32_t base = 0, off = 0;
    long n = 1;
    if (!parseStart(m.args[2], base, off) || off != 0 ||
        (m.argc == 4 && (!toLong(m.args[3], n, 10) || n < 1 || n > (long)kCh376RawReadSecMax))) {
      LOGW(TAG, "DBG,usb,rawread,<LBA>[,<секторів 1..%u>]", (unsigned)kCh376RawReadSecMax);
      return true;
    }
    cmdRawRead(base, (uint32_t)n);
    return true;
  }
  if (!strcasecmp(s, "part") && m.argc == 2) {
    cmdPart();
    return true;
  }
  if (!strcasecmp(s, "rawspeed") && (m.argc == 4 || m.argc == 5)) {
    long lba = 0, kb = 0, n = (long)kCh376DiskSecPerCmdDef;
    if (!toLong(m.args[2], lba, 10) || lba < 0 || !toLong(m.args[3], kb, 10) || kb < 1 ||
        kb > (long)kCh376RawKbMax || (m.argc == 5 && (!toLong(m.args[4], n, 10) || n < 1 || n > (long)kCh376DiskMaxSectors))) {
      LOGW(TAG, "DBG,usb,rawspeed,<LBA>,<КБ 1..%u>[,<сект./команду 1..%u>]", (unsigned)kCh376RawKbMax,
           (unsigned)kCh376DiskMaxSectors);
      return true;
    }
    cmdRawSpeed((uint32_t)lba, (uint32_t)kb, (uint32_t)n);
    return true;
  }
  if (!strcasecmp(s, "rawseq") && m.argc >= 4 && m.argc <= 6) {
    SeqCfg c{};
    uint32_t base = 0, off = 0;
    long kb = 0;
    c.chunk = kCh376RawChunkDef;
    c.periodUs = kCh376RawPeriodDefUs;
    c.preEvery = 1;
    if (!parseStart(m.args[2], base, off) || !toLong(m.args[3], kb, 10) || kb < 1 || kb > (long)kCh376RawKbMax) {
      LOGW(TAG, "DBG,usb,rawseq,<LBA>[+<зсув байт>],<КБ 1..%u>[,<байт/кадр>[,<період мс>]]", (unsigned)kCh376RawKbMax);
      return true;
    }
    if (m.argc >= 5) {
      if (!toLong(m.args[4], v, 10) || v < 1 || v > 4096) {
        LOGW(TAG, "rawseq: байтів на кадр 1..4096");
        return true;
      }
      c.chunk = (uint32_t)v;
    }
    if (m.argc == 6) {
      char* e = nullptr;
      const double ms = strtod(m.args[5], &e);
      if (e == m.args[5] || *e != 0 || ms < 1.0 || ms > 1000.0) {
        LOGW(TAG, "rawseq: період мс 1..1000 (наприклад 26.1)");
        return true;
      }
      c.periodUs = (uint32_t)(ms * 1000.0 + 0.5);
    }
    c.base = base;
    c.inOff = off;
    runSeq("rawseq", c, (uint32_t)kb);
    return true;
  }
  if (!strcasecmp(s, "rawpre") && m.argc == 6) {
    SeqCfg c{};
    uint32_t base = 0, off = 0;
    long kb = 0, pkt = 0, every = 0;
    if (!parseStart(m.args[2], base, off) || !toLong(m.args[3], kb, 10) || kb < 1 || kb > (long)kCh376RawKbMax ||
        !toLong(m.args[4], pkt, 10) || pkt < 1 || pkt > (long)kCh376RawPreUnitMax || !toLong(m.args[5], every, 10) ||
        every < 1 || every > 16) {
      LOGW(TAG, "DBG,usb,rawpre,<LBA>[+<зсув>],<КБ>,<пакет Б 1..%u>,<кадрів 1..16>", (unsigned)kCh376RawPreUnitMax);
      return true;
    }
    c.base = base;
    c.inOff = off;
    c.chunk = kCh376RawChunkDef;
    c.periodUs = kCh376RawPeriodDefUs;
    c.preBytes = (uint32_t)pkt;
    c.preEvery = (uint32_t)every;
    runSeq("rawpre", c, (uint32_t)kb);
    return true;
  }
  if (!strcasecmp(s, "rawcfg") && (m.argc == 2 || m.argc == 4)) {
    cmdRawCfg(m);
    return true;
  }
  return false;
}

}  // namespace Ch376DiagDisk

#endif  // ENABLE_USB_CH376

#include "usb/disk_probe.h"

#include "config.h"

#if ENABLE_USB_CH376

#include <string.h>

// Розбір за специфікаціями FAT (Microsoft), exFAT і UEFI (GPT). Лише читання полів; запису немає.

namespace {

uint16_t rd16(const uint8_t* p) { return (uint16_t)(p[0] | (p[1] << 8)); }
uint32_t rd32(const uint8_t* p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
uint64_t rd64(const uint8_t* p) { return (uint64_t)rd32(p) | ((uint64_t)rd32(p + 4) << 32); }

bool sig55AA(const uint8_t* s) { return s[510] == 0x55 && s[511] == 0xAA; }

bool isPow2(uint32_t v) { return v && !(v & (v - 1)); }

}  // namespace

const char* volKindName(VolKind k) {
  switch (k) {
    case VolKind::None: return "нема";
    case VolKind::Fat12: return "FAT12";
    case VolKind::Fat16: return "FAT16";
    case VolKind::Fat32: return "FAT32";
    case VolKind::ExFat: return "exFAT";
    case VolKind::Unknown: return "невідомо";
  }
  return "?";
}

VolKind fatKindFromClusters(uint32_t c) {
  if (c < 4085u) return VolKind::Fat12;
  if (c < 65525u) return VolKind::Fat16;
  return VolKind::Fat32;
}

bool parseMbr(const uint8_t* s, PartEntry* out, uint8_t& count) {
  count = 0;
  if (!sig55AA(s)) return false;
  for (int i = 0; i < 4; i++) {
    const uint8_t* e = s + 0x1BE + 16 * i;
    out[i].type = e[4];
    out[i].startLba = rd32(e + 8);
    out[i].sectors = rd32(e + 12);
    if (out[i].type != 0) count++;
  }
  return true;
}

bool parseBpb(const uint8_t* s, BpbInfo& b) {
  b = BpbInfo{};
  if (!sig55AA(s)) return false;
  if (!(s[0] == 0xEB && s[2] == 0x90) && s[0] != 0xE9) return false;  // перехід на завантажувач
  const uint16_t bps = rd16(s + 11);
  const uint16_t res = rd16(s + 14);
  const uint16_t rootEnt = rd16(s + 17);
  const uint16_t tot16 = rd16(s + 19);
  const uint16_t fat16 = rd16(s + 22);
  const uint8_t spc = s[13];
  const uint8_t nf = s[16];
  const uint32_t tot32 = rd32(s + 32);
  const uint32_t fat32 = rd32(s + 36);
  if (!(bps == 512 || bps == 1024 || bps == 2048 || bps == 4096)) return false;
  if (!isPow2(spc) || spc > 128) return false;
  if (nf < 1 || nf > 2) return false;
  if (res == 0) return false;
  const uint32_t fatSz = fat16 ? fat16 : fat32;
  const uint32_t tot = tot16 ? tot16 : tot32;
  if (fatSz == 0 || tot == 0) return false;
  const uint32_t rootSec = ((uint32_t)rootEnt * 32u + bps - 1u) / bps;
  const uint64_t meta = (uint64_t)res + (uint64_t)nf * fatSz + rootSec;
  if (meta >= tot) return false;
  const uint32_t dataSec = tot - (uint32_t)meta;
  const uint32_t clus = dataSec / spc;
  b.bytesPerSec = bps;
  b.reserved = res;
  b.secPerClus = spc;
  b.numFats = nf;
  b.rootEntries = rootEnt;
  b.totSec = tot;
  b.fatSz = fatSz;
  b.dataStart = (uint32_t)meta;
  b.clusters = clus;
  const VolKind k = fatKindFromClusters(clus);
  const uint8_t* lab = s + (k == VolKind::Fat32 ? 0x47 : 0x2B);
  if (k == VolKind::Fat32) b.rootClus = rd32(s + 44);
  b.serial = rd32(s + (k == VolKind::Fat32 ? 0x43 : 0x27));  // M4b4: BS_VolID
  int n = 11;
  while (n > 0 && (lab[n - 1] == ' ' || lab[n - 1] == 0)) n--;
  for (int i = 0; i < n; i++) {
    const uint8_t c = lab[i];
    b.label[i] = (c >= 0x20 && c < 0x7F) ? (char)c : '?';
  }
  b.label[n] = 0;
  b.valid = true;
  return true;
}

bool parseExFat(const uint8_t* s, ExFatInfo& e) {
  e = ExFatInfo{};
  if (!sig55AA(s) || memcmp(s + 3, "EXFAT   ", 8) != 0) return false;
  for (int i = 0x0B; i < 0x40; i++)
    if (s[i] != 0) return false;  // MustBeZero
  e.partOffset = rd64(s + 0x40);
  e.volLength = rd64(s + 0x48);
  e.fatOffset = rd32(s + 0x50);
  e.fatLength = rd32(s + 0x54);
  e.heapOffset = rd32(s + 0x58);
  e.clusterCount = rd32(s + 0x5C);
  e.rootCluster = rd32(s + 0x60);
  e.serial = rd32(s + 0x64);  // M4b4: VolumeSerialNumber
  e.bytesPerSecShift = s[0x6C];
  e.secPerClusShift = s[0x6D];
  e.numFats = s[0x6E];
  if (e.bytesPerSecShift < 9 || e.bytesPerSecShift > 12) return false;
  if (e.secPerClusShift + e.bytesPerSecShift > 25) return false;
  if (e.numFats < 1 || e.numFats > 2) return false;
  if (e.clusterCount == 0 || e.rootCluster < 2) return false;
  e.valid = true;
  return true;
}

bool parseGptHeader(const uint8_t* s, uint32_t& entriesLba, uint32_t& count, uint32_t& entrySize) {
  if (memcmp(s, "EFI PART", 8) != 0) return false;
  const uint64_t el = rd64(s + 0x48);
  const uint32_t c = rd32(s + 0x50);
  const uint32_t z = rd32(s + 0x54);
  if (el == 0 || el > 0xFFFFFFFFull) return false;
  if (z < 128 || z > 512 || (z & 7u)) return false;
  if (c == 0 || c > 1024) return false;
  entriesLba = (uint32_t)el;
  count = c;
  entrySize = z;
  return true;
}

bool parseGptFirst(const uint8_t* e, uint32_t esz, uint32_t& start, uint32_t& sectors) {
  for (uint32_t off = 0; off + esz <= 512u; off += esz) {
    bool empty = true;
    for (int j = 0; j < 16; j++)
      if (e[off + j] != 0) empty = false;  // тип розділу (GUID)
    if (empty) continue;
    const uint64_t first = rd64(e + off + 0x20);
    const uint64_t last = rd64(e + off + 0x28);
    if (first == 0 || last < first || first > 0xFFFFFFFFull || last > 0xFFFFFFFFull) return false;
    start = (uint32_t)first;
    sectors = (uint32_t)(last - first + 1);
    return true;
  }
  return false;
}

namespace {

// Розбір тому на lba: exFAT, потім BPB. Сектор буфер s перезаписує.
void classifyVolume(SectorReader& rd, uint8_t* s, uint32_t lba, ProbeReport& o) {
  o.volStart = lba;
  if (!rd.readSector(lba, s)) {
    o.kind = VolKind::Unknown;
    return;
  }
  if (parseExFat(s, o.ex)) {
    o.kind = VolKind::ExFat;
    return;
  }
  if (parseBpb(s, o.bpb)) {
    o.kind = fatKindFromClusters(o.bpb.clusters);
    o.readable = o.bpb.bytesPerSec == 512;
    return;
  }
  o.kind = VolKind::Unknown;
}

}  // namespace

bool probeDisk(SectorReader& rd, uint8_t* s, ProbeReport& o) {
  o = ProbeReport{};
  if (!rd.readSector(0, s)) return false;

  // Суцільна флешка: exFAT або FAT без таблиці розділів.
  if (parseExFat(s, o.ex)) {
    o.kind = VolKind::ExFat;
    o.volStart = 0;
    return true;
  }
  if (parseBpb(s, o.bpb)) {
    o.kind = fatKindFromClusters(o.bpb.clusters);
    o.readable = o.bpb.bytesPerSec == 512;
    o.volStart = 0;
    return true;
  }

  // MBR. Без 55AA і таблиці — нічого розбирати.
  uint8_t cnt = 0;
  PartEntry parts[4];
  if (!parseMbr(s, parts, cnt)) return true;
  o.mbr = true;
  o.partCount = cnt;
  for (int i = 0; i < 4; i++) {
    o.parts[i] = parts[i];
    if (parts[i].type == 0xEE) o.protectiveGpt = true;
  }

  uint32_t volLba = 0;
  bool haveVol = false;
  if (o.protectiveGpt) {
    uint32_t entLba = 0, n = 0, esz = 0;
    if (rd.readSector(1, s) && parseGptHeader(s, entLba, n, esz)) {
      o.gptHeader = true;
      if (rd.readSector(entLba, s)) {
        uint32_t st = 0, sec = 0;
        if (parseGptFirst(s, esz, st, sec)) {
          o.gptFirstFound = true;
          volLba = st;
          haveVol = true;
        }
      }
    }
  } else {
    for (int i = 0; i < 4 && !haveVol; i++) {
      if (parts[i].type != 0) {
        volLba = parts[i].startLba;
        haveVol = true;
      }
    }
  }
  if (!haveVol) {
    o.kind = VolKind::None;
    return true;
  }
  classifyVolume(rd, s, volLba, o);
  return true;
}

#endif  // ENABLE_USB_CH376


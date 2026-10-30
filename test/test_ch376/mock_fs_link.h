#pragma once
#include <stdint.h>
#include <string.h>

#include <string>
#include <vector>

#include "config.h"
#include "usb/ch376_link.h"

// Mock CH376S для файлового шару (M4b1): дерево каталогів у памʼяті (імена 8.3, вміст — детермінований патерн),
// SET_FILE_NAME / FILE_OPEN (шлях, "*", "/") / FILE_ENUM_GO / BYTE_LOCATE / BYTE_READ / BYTE_RD_GO / RD_USB_DATA0 /
// GET_FILE_SIZE / FILE_CLOSE / GET_STATUS. Час керований (readByte/yield/waitIntLow його зсувають); INT# = «є видима подія».
struct FsNode {
  std::string name;  // великі літери, «NAME.EXT»
  bool dir;
  int parent;
  uint32_t size;
  uint8_t attr;
};

inline uint8_t fsPattern(int id, uint32_t off) { return (uint8_t)(off * 31u + (uint32_t)id * 7u + (off >> 8)); }

class MockFsLink : public Ch376Link {
 public:
  std::vector<FsNode> nodes;
  bool intPin = true;           // hasIntPin()
  uint32_t latencyUs = 0;       // затримка події після команди
  // Збої
  int ejectAtBlock = -1;        // на цьому блоці (з 1) флешку витягнуто: подія 0x16, далі всі команди → 0x82
  int errAtBlock = -1;          // на цьому блоці одна подія 0x1F
  int truncAtBlock = -1;        // RD_USB_DATA0 цього блоку: довжина 64, але лише 10 байтів
  int badLenAtBlock = -1;       // RD_USB_DATA0 цього блоку: довжина 0xFF
  bool silentStatus = false;    // GET_STATUS не відповідає
  size_t blockLen = 64;         // максимальний блок BYTE_READ (реальний чіп віддає до 255)
  uint8_t lastCode = 0;
  bool dupStatus = false;       // чіп шле статус двічі (дубль іде за відповіддю на GET_STATUS)
  bool prefixSize = false;      // GET_FILE_SIZE: перед 4 байтами іде копія статусу
  uint32_t byteUs = 0;          // byteTimeUs()
  int openStatus = 0;           // >0: FILE_OPEN повертає цей код
  bool ejected = false;
  // Журнал
  std::vector<std::vector<uint8_t>> frames;
  std::vector<std::string> names;  // SET_FILE_NAME (як надіслано)
  int cmdCount[256] = {0};

  // Стан чіпа
  int cwd = 0, openFile = -1;
  uint32_t fpos = 0, remain = 0;
  bool inRead = false;
  int blockCnt = 0;
  uint8_t buf[256];
  size_t bufLen = 0;
  std::string pend;
  std::vector<int> enumList;
  size_t enumIdx = 0;

  uint32_t nowV = 0;
  struct Ev { uint32_t at; uint8_t code; };
  std::vector<Ev> events;
  std::vector<uint8_t> rx;

  MockFsLink() { nodes.push_back(FsNode{"", true, -1, 0, kCh376AttrDir}); }

  int addDir(int parent, const char* n) {
    nodes.push_back(FsNode{n, true, parent, 0, kCh376AttrDir});
    return (int)nodes.size() - 1;
  }
  int addFile(int parent, const char* n, uint32_t size, uint8_t attr = 0x20) {
    nodes.push_back(FsNode{n, false, parent, size, attr});
    return (int)nodes.size() - 1;
  }

  // ---- Ch376Link
  void write(const uint8_t* d, size_t n) override {
    frames.emplace_back(d, d + n);
    if (n < 3 || d[0] != 0x57 || d[1] != 0xAB) return;
    cmdCount[d[2]]++;
    switch (d[2]) {
      case 0x22:  // GET_STATUS
        if (!silentStatus) {
          const uint8_t c = takeEvent();
          rx.push_back(c);
          if (dupStatus) rx.push_back(c);
        }
        break;
      case 0x2F: {  // SET_FILE_NAME
        pend.clear();
        for (size_t i = 3; i < n && d[i]; i++) pend.push_back((char)d[i]);
        names.push_back(pend);
        break;
      }
      case 0x32: fileOpen(); break;
      case 0x33: enumNext(); break;
      case 0x27: readData0(); break;
      case 0x0C:
        if (openFile >= 0 && d[3] == 0x68) {
          const uint32_t sz = nodes[(size_t)openFile].size;
          if (prefixSize) rx.push_back(lastCode);
          for (int i = 0; i < 4; i++) rx.push_back((uint8_t)(sz >> (8 * i)));
        }
        break;
      case 0x39:
        if (ejected) { post(0x82); break; }
        if (openFile < 0) { post(0xB4); break; }
        {
          const uint32_t off = d[3] | (d[4] << 8) | (d[5] << 16) | ((uint32_t)d[6] << 24);
          const uint32_t sz = nodes[(size_t)openFile].size;
          fpos = off > sz ? sz : off;
        }
        post(0x14);
        break;
      case 0x3A:
        if (ejected) { post(0x82); break; }
        if (openFile < 0) { post(0xB4); break; }
        remain = d[3] | (d[4] << 8);
        inRead = true;
        bufLen = 0;
        serve();
        break;
      case 0x3B:
        if (ejected) { post(0x82); break; }
        if (!inRead) { post(0x14); break; }
        fpos += (uint32_t)bufLen;
        remain -= (uint32_t)bufLen;
        bufLen = 0;
        serve();
        break;
      case 0x36:
        if (ejected) { post(0x82); break; }
        openFile = -1;
        inRead = false;
        remain = 0;
        post(0x14);
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
  bool abortRequested() const override { return false; }
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
    if (k < n) nowV += tmoUs;  // не дочекались — минув тайм-аут бездіяльності
    return k;
  }

 private:
  bool visible() const {
    for (const Ev& e : events)
      if (e.at <= nowV) return true;
    return false;
  }
  void post(uint8_t c) { events.push_back(Ev{nowV + latencyUs, c}); }
  uint8_t takeEvent() {
    for (size_t i = 0; i < events.size(); i++)
      if (events[i].at <= nowV) {
        const uint8_t c = events[i].code;
        events.erase(events.begin() + (long)i);
        lastCode = c;
        return c;
      }
    return kCh376IntNone;
  }

  void fileOpen() {
    if (ejected) { post(0x82); return; }
    if (openStatus > 0) { post((uint8_t)openStatus); return; }
    std::string nm = pend;
    int base = cwd;
    if (!nm.empty() && nm[0] == '/') {
      base = 0;
      nm.erase(0, 1);
    }
    if (nm.empty()) {
      cwd = base;
      post(0x41);
      return;
    }
    if (openFile >= 0) { post(0xB4); return; }
    if (nm == "*") {
      enumList.clear();
      enumIdx = 0;
      if (base != 0) {
        enumList.push_back(-1);  // "."
        enumList.push_back(-2);  // ".."
      }
      if (base == 0 && volLabel) enumList.push_back(-3);
      for (size_t i = 1; i < nodes.size(); i++)
        if (nodes[i].parent == base) enumList.push_back((int)i);
      enumNext();
      return;
    }
    for (size_t i = 1; i < nodes.size(); i++) {
      if (nodes[i].parent != base || nodes[i].name != nm) continue;
      if (nodes[i].dir) {
        cwd = (int)i;
        post(0x41);
      } else {
        openFile = (int)i;
        fpos = 0;
        post(0x14);
      }
      return;
    }
    post(0x42);
  }

  void enumNext() {
    if (ejected) { post(0x82); return; }
    if (enumIdx >= enumList.size()) { post(0x42); return; }
    const int id = enumList[enumIdx++];
    uint8_t r[32];
    memset(r, 0, sizeof r);
    memset(r, ' ', 11);
    if (id == -1) r[0] = '.';
    else if (id == -2) { r[0] = '.'; r[1] = '.'; }
    else if (id == -3) { memcpy(r, "MYDISK     ", 11); r[11] = kCh376AttrVolume; }
    else {
      const FsNode& n = nodes[(size_t)id];
      const size_t dot = n.name.find('.');
      const std::string b = n.name.substr(0, dot);
      const std::string e = dot == std::string::npos ? "" : n.name.substr(dot + 1);
      memcpy(r, b.data(), b.size());
      memcpy(r + 8, e.data(), e.size());
      r[11] = n.attr;
      for (int i = 0; i < 4; i++) r[28 + i] = (uint8_t)(n.size >> (8 * i));
    }
    if (id < 0 && id != -3) r[11] = kCh376AttrDir;
    memcpy(buf, r, 32);
    bufLen = 32;
    post(0x1D);
  }

  void serve() {
    const uint32_t sz = nodes[(size_t)openFile].size;
    if (remain == 0 || fpos >= sz) {
      inRead = false;
      bufLen = 0;
      post(0x14);
      return;
    }
    blockCnt++;
    if (blockCnt == ejectAtBlock) {
      ejected = true;
      openFile = -1;
      post(0x16);
      return;
    }
    if (blockCnt == errAtBlock) {
      post(0x1F);
      return;
    }
    uint32_t n = remain < blockLen ? remain : (uint32_t)blockLen;
    if (sz - fpos < n) n = sz - fpos;
    for (uint32_t i = 0; i < n; i++) buf[i] = fsPattern(openFile, fpos + i);
    bufLen = n;
    post(0x1D);
  }

  void readData0() {
    if (blockCnt == badLenAtBlock && bufLen) { rx.push_back(0xFF); return; }
    if (blockCnt == truncAtBlock && bufLen) {
      rx.push_back(64);
      for (int i = 0; i < 10; i++) rx.push_back(buf[i]);
      return;
    }
    rx.push_back((uint8_t)bufLen);
    for (size_t i = 0; i < bufLen; i++) rx.push_back(buf[i]);
  }

 public:
  bool volLabel = false;  // у кореневому переліку є запис мітки тому
};

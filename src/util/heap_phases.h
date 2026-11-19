#pragma once
#include <stdint.h>

// M4c1. Мінімум heap по фазах життєвого циклу джерел (DBG,heap,phases | DBG,heap,reset). Для кожної фази зберігаються
// кількість замірів, мінімум вільного heap (free) і мінімум найбільшого вільного блоку (largest), що спостерігались.
// Заміри — на межах фаз (mark) і періодично під час скану (SdSource, kHeapSamplePeriodMs); без затримок і блокувань.
// record() — чиста логіка (тестується в native); mark() бере числа з heap_caps і існує лише в Arduino-збірці.
// Статична памʼять: kN × 12 Б (≈ 0,13 КБ).
namespace HeapPhase {

enum Phase : uint8_t {
  SdAct,      // активація SD (SourceManager::select навколо activate)
  BtAct,      // активація BT
  UsbAct,     // активація USB (open драйвера й тому + старт скану)
  UsbDrv,     // UsbVolume::open: драйвер UART2 і сеанс підняті
  UsbVol,     // UsbVolume::open: том SdFat змонтовано
  ScanStart,  // початок скану (після виділення кеша й пулу)
  ScanPeak,   // періодично під час скану
  ScanEnd,    // кінець або переривання скану (після звільнення кеша й пулу)
  Play,       // відтворення (старт треку, періодично)
  Deact,      // деактивація джерела (навколо deactivate)
  UsbClose,   // UsbVolume::close (до і після звільнення)
  kN
};

struct Rec {
  uint32_t n;           // кількість замірів
  uint32_t minFree;     // мінімум free, Б (0xFFFFFFFF, поки замірів немає)
  uint32_t minLargest;  // мінімум largest, Б
};

const char* name(Phase p);
void record(Phase p, uint32_t freeB, uint32_t largestB);  // чиста логіка
const Rec& get(Phase p);
void reset();

#ifdef ARDUINO
void mark(Phase p);   // заміряти heap зараз і записати у фазу p
void print();         // DBG,heap,phases: по рядку на фазу (короткі рядки)
#else
inline void mark(Phase) {}
#endif

}  // namespace HeapPhase

#pragma once
#include <stddef.h>
#include <stdint.h>

// Консоль UART0: приймає кадри $BODY*XX і прості рядки NAME,арг,… у той самий роутер, що й S3.
namespace Console {

// M5c5: лічильники приймання (пише лише задача console; DBG,console читає в ній же).
struct Stats {
  uint32_t rxCap;      // розмір RX-буфера драйвера, Б
  uint32_t rxPeak;     // макс. заповнення, яке бачила задача, Б
  uint32_t rxHigh;     // скільки спостережень заповнення ≥ kConsoleRxHighPct % ємності
  uint32_t bytes;      // прочитано байтів
  uint32_t lines;      // оброблено рядків
  uint32_t longLines;  // задовгих рядків (> kConsoleLineMax-1 Б), відкинуто
  uint32_t longBytes;  // байтів, відкинутих у задовгих рядках
  uint32_t maxLen;     // найдовший прийнятий рядок, Б
};

void begin();
Stats stats();
void resetStats();

}  // namespace Console

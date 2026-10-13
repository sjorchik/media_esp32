#pragma once
#include <stddef.h>
#include <stdint.h>

// Лінк із S3: UART1, задачі rx/tx, HB, вихідні черги з пріоритетом.
namespace Link {

// Hi: STATE/ERR/READY/MEDIA/BT. Lo: TRACK/FOLDER/META (не губляться: чекають місця).
// Time: подія TIME — один слот, нове значення замінює непереслане (лишається останнє).
enum class Pri : uint8_t { Hi, Lo, Time };

struct Stats {
  uint32_t good, bad;
  uint32_t dropped;         // = lostHi + lostLo
  uint32_t lostHi, lostLo;  // втрати після очікування kTxWaitMs
  uint32_t timeCoalesced;   // скільки TIME злито (не вважається втратою)
  uint32_t txMaxUs;         // макс. час відправки одного кадру (UART1 + дзеркало UART0)
  uint32_t mirrorMaxUs;     // з нього — час дзеркала в UART0
  uint32_t txItems;
  bool alive;
};

void begin();
// frame — повний кадр $…*XX\n. Для Hi/Lo при повній черзі чекає до kTxWaitMs, потім
// видаляє найстаріший елемент тієї ж черги і рахує втрату. Для Time не блокує.
void send(const char* frame, size_t len, Pri pri);
Stats stats();
void resetTxStats();

}  // namespace Link
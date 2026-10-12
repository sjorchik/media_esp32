#pragma once
#include <stddef.h>
#include <stdint.h>

// Лінк із S3: UART1, задачі rx/tx, HB, вихідні черги з пріоритетом.
namespace Link {

struct Stats { uint32_t good, bad, dropped; bool alive; };

void begin();
// frame — повний кадр $…*XX\n. high=true: STATE/ERR/READY/MEDIA/BT; false: META/TIME/TRACK/FOLDER.
// Не блокує; при переповненні черги видаляє найстаріший елемент тієї ж черги.
void send(const char* frame, size_t len, bool high);
Stats stats();

}  // namespace Link
#pragma once
#include <stdint.h>

#include "proto/frame.h"

// Розбір і застосування аргументів команд контракту (M6b). Без Arduino/FreeRTOS — збирається в native.
namespace proto {

// Лише десяткові цифри (контракт, розд. 2): без пробілів, знаку, префіксів. Порожнє/нечислове/задовге → false.
bool parseUInt(const char* s, long& v);
// Те саме, але допускає один провідний '-' (без '+'). Для LVL.
bool parseSInt(const char* s, long& v);

enum class HelloVerdict : uint8_t { BadArg, ReadyOnly, Full };
// HELLO,<ver>: BadArg — порожнє/нечислове; ReadyOnly — чужа версія (лише READY); Full — версія збігається.
HelloVerdict helloVerdict(const char* arg, int ourVer, long& ver);

// LVL,<bt|sd|usb>,<дБ>: ціле kLevelMinDb..kLevelMaxDb. false → нічого не застосовано; true → level::setDb.
bool applyLvl(const Msg& m);

}  // namespace proto

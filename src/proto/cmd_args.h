#pragma once
#include <stdint.h>

#include "proto/frame.h"

// Розбір і застосування аргументів команд контракту (M6b). Без Arduino/FreeRTOS — збирається в native.
namespace proto {

// Лише десяткові цифри (контракт, розд. 2): без пробілів, знаку, префіксів. Порожнє/нечислове/задовге → false.
bool parseUInt(const char* s, long& v);
// Те саме, але допускає один провідний '-' (без '+'). Для EQ і LVL.
bool parseSInt(const char* s, long& v);

enum class HelloVerdict : uint8_t { BadArg, ReadyOnly, Full };
// HELLO,<ver>: BadArg — порожнє/нечислове; ReadyOnly — чужа версія (лише READY); Full — версія збігається.
HelloVerdict helloVerdict(const char* arg, int ourVer, long& ver);

// EQ,<g1>..<g5>: рівно 5 цілих kGainMinDb..kGainMaxDb. Усе або нічого: false → нічого не застосовано.
// true → eq::setAllDb. Можна з будь-якої задачі (eq: атомарні слова).
bool applyEq(const Msg& m);
// LVL,<bt|sd|usb>,<дБ>: ціле kLevelMinDb..kLevelMaxDb. false → нічого не застосовано; true → level::setDb.
bool applyLvl(const Msg& m);

}  // namespace proto

#pragma once
#include <stddef.h>
#include <stdint.h>

// Кадр $<BODY>*<XX>\n. Модуль без залежностей від Arduino (тестується на ПК).
namespace proto {

inline constexpr size_t kMaxFrame = 255;              // '$' + BODY + '*XX' + LF
inline constexpr size_t kMaxBody  = kMaxFrame - 5;    // 250
inline constexpr size_t kMaxStore = kMaxFrame - 2;    // BODY*XX без '$' і LF = 253
inline constexpr size_t kMaxArgs  = 8;

uint8_t xorSum(const char* s, size_t n);

// ---- Кодувальник ----
struct Arg {
  enum Kind : uint8_t { Num, Str, Text } kind;
  long num;
  const char* str;
  static Arg N(long v) { return Arg{Num, v, nullptr}; }          // число
  static Arg S(const char* s) { return Arg{Str, 0, s ? s : ""}; }  // токен (екранується, не скорочується)
  static Arg T(const char* s) { return Arg{Text, 0, s ? s : ""}; } // текст (екранується і скорочується)
};

// Формує повний кадр у out ($…*XX\n + NUL). Повертає довжину без NUL, 0 при помилці.
// cap має бути ≥ kMaxFrame+1. Текстові поля скорочуються по межі UTF-8-символу
// (справедливий розподіл: короткі поля лишаються цілими).
size_t encodeFrame(char* out, size_t cap, const char* name, const Arg* args, size_t n);

// ---- Потоковий парсер ----
class FrameParser {
 public:
  typedef void (*Callback)(char* body, size_t len, void* ctx);  // body NUL-завершений, змінюваний
  FrameParser(Callback cb, void* ctx) : cb_(cb), ctx_(ctx) {}
  void feed(uint8_t b);
  void feed(const uint8_t* d, size_t n) { while (n--) feed(*d++); }
  void reset() { state_ = Idle; len_ = 0; }
  uint32_t good() const { return good_; }
  uint32_t bad() const { return bad_; }

 private:
  enum State : uint8_t { Idle, Body, Overflow };
  void finish();
  Callback cb_;
  void* ctx_;
  State state_ = Idle;
  size_t len_ = 0;
  uint32_t good_ = 0, bad_ = 0;
  char buf_[kMaxStore + 1];
};

// ---- Розбір тіла ----
struct Msg {
  char* name;
  char* args[kMaxArgs];
  uint8_t argc;
  bool tooMany;
};

void percentDecode(char* s);               // на місці
bool splitBody(char* body, Msg& m);        // на місці; false якщо імʼя порожнє

}  // namespace proto
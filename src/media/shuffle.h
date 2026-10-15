#pragma once
#include <stddef.h>
#include <stdint.h>

// Рушій shuffle (M5a). Чистий модуль: без SD, FreeRTOS і Arduino. Генератор випадкових чисел підставляється
// (на залізі — esp_random()). Памʼять під перестановку й історію надає викликач (без динамічних виділень).
//
//  Shuffle      — перестановка індексів 0..n-1 (Fisher–Yates), вказівник кола k: елементи [0,k) «зіграні».
//  TrackHistory — кільце останніх відтворених треків з курсором (PREV/NEXT по історії).
//  ShufNav      — політика вибору треку (NEXT/PREV/FNEXT/FPREV/кінець треку/пропуск биттого) поверх двох вище.

class Shuffle {
 public:
  static constexpr uint16_t kNone = 0xFFFF;
  using Rng = uint32_t (*)();  // 32 випадкові біти

  // Привʼязка буфера на n елементів (n=0 або buf=nullptr — порожньо). Скидає коло (нове перемішування — begin/next).
  void attach(uint16_t* buf, uint16_t n, Rng rng);

  // Нове коло. first<n: він ставиться на позицію 0 і вважається зіграним (k=1), решта перемішується;
  // інакше — повне перемішування, k=0.
  void begin(uint16_t first);

  // Наступний трек кола (k++). Коло вичерпано → нове перемішування; його перший елемент ≠ last
  // (за відсутності last — ≠ останньому елементу попереднього кола), якщо n ≥ 2. kNone, якщо порожньо.
  uint16_t next(uint16_t last = kNone);

  // Ручний стрибок: незіграний трек t міняється місцями з елементом на вказівнику й стає «наступним зіграним»
  // (true); уже зіграний — без змін (false). Коло ще не розпочато → begin(t).
  bool jump(uint16_t t);

  bool isPlayed(uint16_t t) const;
  bool check() const;  // перестановка валідна (значення < n; суми та суми квадратів збігаються з 0..n-1)

  bool attached() const { return p_ && n_; }
  bool started() const { return started_; }
  bool atEnd() const { return started_ && k_ >= n_; }  // усі елементи кола зіграні
  uint16_t size() const { return n_; }
  uint16_t played() const { return k_; }
  uint16_t at(uint16_t i) const { return (p_ && i < n_) ? p_[i] : kNone; }
  uint32_t rounds() const { return rounds_; }  // скільки разів коло відновлювалось після вичерпання

 private:
  uint32_t rnd();
  uint16_t below(uint16_t m);  // 0..m-1 без зсуву (m ≥ 1)
  void fy(uint16_t from);      // Fisher–Yates по [from, n)

  uint16_t* p_ = nullptr;
  Rng rng_ = nullptr;
  uint16_t n_ = 0, k_ = 0;
  bool started_ = false;
  uint32_t rounds_ = 0;
  uint32_t xs_ = 2463534242u;  // запасний xorshift32, якщо rng не задано
};

class TrackHistory {
 public:
  static constexpr uint16_t kNone = 0xFFFF;

  void init(uint16_t* buf, uint16_t cap);
  void clear();

  // Новий трек у голову. Повтор поточного запису ігнорується; після кроків назад «майбутнє» відсікається;
  // переповнення витісняє найстаріший запис.
  void push(uint16_t t);

  bool empty() const { return cnt_ == 0; }
  bool hasBack() const { return cnt_ && cur_ > 0; }
  bool hasFwd() const { return cnt_ && (uint16_t)(cur_ + 1) < cnt_; }
  uint16_t peekBack() const { return hasBack() ? at((uint16_t)(cur_ - 1)) : kNone; }
  uint16_t peekFwd() const { return hasFwd() ? at((uint16_t)(cur_ + 1)) : kNone; }
  void back() { if (hasBack()) cur_--; }
  void fwd() { if (hasFwd()) cur_++; }

  uint16_t count() const { return cnt_; }
  uint16_t cursor() const { return cur_; }
  uint16_t capacity() const { return cap_; }
  uint16_t current() const { return cnt_ ? at(cur_) : kNone; }
  uint16_t at(uint16_t i) const { return (b_ && i < cnt_) ? b_[(uint16_t)((base_ + i) % cap_)] : kNone; }  // 0 = найстаріший

 private:
  uint16_t* b_ = nullptr;
  uint16_t cap_ = 0, base_ = 0, cnt_ = 0, cur_ = 0;
};

enum class HAct : uint8_t { Push, Back, Fwd, None };  // що зробити з історією, коли трек успішно запущено

struct Pick {
  uint16_t to;  // індекс треку (з 0)
  HAct act;
  char why;     // N next, P prev, R перезапуск, F/B fnext/fprev, p play, e кінець->наступний, r repeat-1,
                // z кінець (stop), x пропуск помилки, d DBG,play, S shuffle-next, H prev по історії, h вперед по історії
  bool stop;    // лише для end(): зупинитись (to = поточна позиція після зупинки)
};

class ShufNav {
 public:
  void init(uint16_t* histBuf, uint16_t histCap, Shuffle::Rng rng);

  // Плейлист готовий (після скану) / недійсний. Обидва скидають коло й історію; режим on() зберігається.
  void attach(uint16_t* perm, uint16_t total);
  void detach();

  bool setOn(bool on);              // true, якщо режим змінився; вимкнення скидає ознаку «коло завершено»
  void startAt(uint16_t cur);       // SHUF,1: поточний — перший у колі й зіграний (звук не переривається)
  uint16_t initialTrack();          // стартовий трек після скану: при on() — випадковий (зіграний), інакше 0

  Pick next(uint16_t cur);
  Pick prev(uint16_t cur, bool restart);       // restart: позиція > kPrevRestartMs → перезапуск поточного
  Pick jump(uint16_t to, char why);            // FNEXT/FPREV/DBG,play: to визначає викликач
  Pick play(uint16_t cur);                     // PLAY зі stop
  Pick skip(uint16_t cur);                     // автопропуск биттого
  Pick end(uint16_t cur, uint8_t rep);         // природний кінець треку: REP 0/1/2
  void commit(HAct a, uint16_t t);             // трек успішно запущено

  bool on() const { return on_; }
  bool active() const { return on_ && total_ > 0; }
  bool ended() const { return ended_; }
  const Shuffle& eng() const { return sh_; }
  const TrackHistory& hist() const { return hi_; }

 private:
  uint16_t nextIdx(uint16_t i) const { return (uint16_t)(i + 1 >= total_ ? 0 : i + 1); }
  uint16_t prevIdx(uint16_t i) const { return (uint16_t)(i == 0 ? (total_ ? total_ - 1 : 0) : i - 1); }

  Shuffle sh_;
  TrackHistory hi_;
  Shuffle::Rng rng_ = nullptr;
  uint16_t total_ = 0;
  bool on_ = false;
  bool ended_ = false;  // REP=0: коло завершилось → stop; нове коло готується при наступному PLAY/NEXT
};

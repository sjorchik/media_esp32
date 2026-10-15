#include "media/shuffle.h"

// ============================================================ Shuffle

uint32_t Shuffle::rnd() {
  if (rng_) return rng_();
  xs_ ^= xs_ << 13;
  xs_ ^= xs_ >> 17;
  xs_ ^= xs_ << 5;
  return xs_;
}

// Рівномірне 0..m-1: відкидаємо перші 2^32 mod m значень. Ліміт спроб захищає від «застряглого» генератора.
uint16_t Shuffle::below(uint16_t m) {
  if (m <= 1) return 0;
  const uint32_t t = (0u - (uint32_t)m) % m;
  uint32_t r = rnd();
  for (int i = 0; r < t && i < 32; i++) r = rnd();
  return (uint16_t)(r % m);
}

void Shuffle::fy(uint16_t from) {
  for (uint16_t i = (uint16_t)(n_ - 1); i > from; i--) {
    uint16_t j = (uint16_t)(from + below((uint16_t)(i - from + 1)));
    uint16_t tmp = p_[i];
    p_[i] = p_[j];
    p_[j] = tmp;
  }
}

void Shuffle::attach(uint16_t* buf, uint16_t n, Rng rng) {
  p_ = buf;
  n_ = buf ? n : 0;
  rng_ = rng;
  k_ = 0;
  started_ = false;
  rounds_ = 0;
}

void Shuffle::begin(uint16_t first) {
  if (!attached()) {
    k_ = 0;
    started_ = false;
    return;
  }
  for (uint16_t i = 0; i < n_; i++) p_[i] = i;
  if (first < n_) {
    p_[0] = first;
    p_[first] = 0;  // обмін 0 ↔ first; решта перемішується
    fy(1);
    k_ = 1;
  } else {
    fy(0);
    k_ = 0;
  }
  started_ = true;
}

uint16_t Shuffle::next(uint16_t last) {
  if (!attached()) return kNone;
  if (!started_) begin(kNone);
  if (k_ >= n_) {  // коло вичерпано → нове; перший ≠ останньому зіграному
    uint16_t lp = (last < n_) ? last : p_[n_ - 1];
    begin(kNone);
    rounds_++;
    if (n_ >= 2 && p_[0] == lp) {
      uint16_t j = (uint16_t)(1 + below((uint16_t)(n_ - 1)));
      uint16_t tmp = p_[0];
      p_[0] = p_[j];
      p_[j] = tmp;
    }
  }
  return p_[k_++];
}

bool Shuffle::jump(uint16_t t) {
  if (!attached() || t >= n_) return false;
  if (!started_) {
    begin(t);
    return true;
  }
  uint16_t idx = 0;
  while (idx < n_ && p_[idx] != t) idx++;
  if (idx >= n_ || idx < k_) return false;  // уже зіграний (або пошкоджена перестановка)
  uint16_t tmp = p_[k_];
  p_[k_] = p_[idx];
  p_[idx] = tmp;
  k_++;
  return true;
}

bool Shuffle::isPlayed(uint16_t t) const {
  if (!attached() || !started_) return false;
  for (uint16_t i = 0; i < k_; i++)
    if (p_[i] == t) return true;
  return false;
}

bool Shuffle::check() const {
  if (!attached()) return true;
  if (!started_) return true;
  uint64_t s = 0, q = 0;
  for (uint16_t i = 0; i < n_; i++) {
    if (p_[i] >= n_) return false;
    s += p_[i];
    q += (uint64_t)p_[i] * p_[i];
  }
  const uint64_t m = n_;
  return s == m * (m - 1) / 2 && q == (m - 1) * m * (2 * m - 1) / 6;
}

// ============================================================ TrackHistory

void TrackHistory::init(uint16_t* buf, uint16_t cap) {
  b_ = buf;
  cap_ = buf ? cap : 0;
  clear();
}

void TrackHistory::clear() {
  base_ = cnt_ = cur_ = 0;
}

void TrackHistory::push(uint16_t t) {
  if (!b_ || !cap_) return;
  if (cnt_ && at(cur_) == t) return;  // повтор/перезапуск поточного
  if (cnt_) cnt_ = (uint16_t)(cur_ + 1);  // після кроків назад «майбутнє» відсікається
  if (cnt_ >= cap_) {                     // переповнення: витісняємо найстаріший
    base_ = (uint16_t)((base_ + 1) % cap_);
    cnt_--;
  }
  b_[(uint16_t)((base_ + cnt_) % cap_)] = t;
  cnt_++;
  cur_ = (uint16_t)(cnt_ - 1);
}

// ============================================================ ShufNav

void ShufNav::init(uint16_t* histBuf, uint16_t histCap, Shuffle::Rng rng) {
  hi_.init(histBuf, histCap);
  rng_ = rng;
  sh_.attach(nullptr, 0, rng);
  total_ = 0;
  ended_ = false;  // режим on_ зберігається (SHUF міг прийти до begin)
}

void ShufNav::attach(uint16_t* perm, uint16_t total) {
  sh_.attach(perm, total, rng_);
  hi_.clear();
  total_ = perm ? total : 0;
  ended_ = false;
}

void ShufNav::detach() { attach(nullptr, 0); }

bool ShufNav::setOn(bool on) {
  if (on == on_) return false;
  on_ = on;
  ended_ = false;
  return true;
}

void ShufNav::startAt(uint16_t cur) {
  if (sh_.attached()) sh_.begin(cur);
}

uint16_t ShufNav::initialTrack() {
  if (!active()) return 0;
  uint16_t t = sh_.next(Shuffle::kNone);
  return t == Shuffle::kNone ? 0 : t;
}

Pick ShufNav::next(uint16_t cur) {
  ended_ = false;
  if (!active()) return Pick{nextIdx(cur), HAct::Push, 'N', false};
  if (hi_.hasFwd()) return Pick{hi_.peekFwd(), HAct::Fwd, 'h', false};  // після PREV — вперед по історії
  return Pick{sh_.next(cur), HAct::Push, 'S', false};
}

Pick ShufNav::prev(uint16_t cur, bool restart) {
  ended_ = false;
  if (restart) return Pick{cur, HAct::None, 'R', false};
  if (!active()) return Pick{prevIdx(cur), HAct::Push, 'P', false};
  if (!hi_.empty() && hi_.current() != cur)  // історія розійшлась із показаним треком: повертаємось до запису
    return Pick{hi_.current(), HAct::None, 'H', false};
  if (hi_.hasBack()) return Pick{hi_.peekBack(), HAct::Back, 'H', false};
  uint16_t to = prevIdx(cur);  // історія порожня (або вичерпана): попередній за каталогом
  sh_.jump(to);
  return Pick{to, HAct::Push, 'P', false};
}

Pick ShufNav::jump(uint16_t to, char why) {
  ended_ = false;
  if (active()) sh_.jump(to);
  return Pick{to, HAct::Push, why, false};
}

Pick ShufNav::play(uint16_t cur) {
  bool fresh = ended_;
  ended_ = false;
  if (active() && fresh) return Pick{sh_.next(cur), HAct::Push, 'p', false};  // нове коло, перший ≠ cur
  return Pick{cur, HAct::Push, 'p', false};
}

Pick ShufNav::skip(uint16_t cur) {
  if (active()) return Pick{sh_.next(cur), HAct::Push, 'x', false};
  return Pick{nextIdx(cur), HAct::Push, 'x', false};
}

Pick ShufNav::end(uint16_t cur, uint8_t rep) {
  if (rep == 1) return Pick{cur, HAct::None, 'r', false};
  if (active()) {
    if (hi_.hasFwd()) return Pick{hi_.peekFwd(), HAct::Fwd, 'h', false};
    if (rep == 0 && sh_.atEnd()) {  // коло завершено: stop; нове коло — при наступному PLAY/NEXT
      ended_ = true;
      return Pick{cur, HAct::None, 'z', true};
    }
    return Pick{sh_.next(cur), HAct::Push, 'e', false};
  }
  if (rep == 2 || (uint32_t)cur + 1 < total_) return Pick{nextIdx(cur), HAct::Push, 'e', false};
  return Pick{0, HAct::None, 'z', true};  // кінець списку: stop, поточний = перший
}

void ShufNav::commit(HAct a, uint16_t t) {
  switch (a) {
    case HAct::Push: hi_.push(t); break;
    case HAct::Back: hi_.back(); break;
    case HAct::Fwd: hi_.fwd(); break;
    default: break;
  }
}

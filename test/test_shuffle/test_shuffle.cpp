#include <stdint.h>
#include <string.h>
#include <unity.h>

#include "media/shuffle.h"

// Детермінований генератор (xorshift32)
static uint32_t s_state = 12345u;
static uint32_t rng() {
  s_state ^= s_state << 13;
  s_state ^= s_state >> 17;
  s_state ^= s_state << 5;
  return s_state;
}

static const uint16_t kNone = Shuffle::kNone;
static uint16_t s_perm[64];
static uint16_t s_hist[32];

void setUp(void) { s_state = 12345u; }
void tearDown(void) {}

// Симуляція програвача: запускає вибір і фіксує його в історії
struct Sim {
  ShufNav n;
  uint16_t cur = 0;
  void init(uint16_t total, uint16_t histCap = 32) {
    n.init(s_hist, histCap, rng);
    n.attach(s_perm, total);
  }
  void go(const Pick& p) {
    n.commit(p.act, p.to);
    cur = p.to;
  }
};

static void test_permutation_valid() {
  Shuffle s;
  for (uint16_t n = 1; n <= 40; n++) {
    s.attach(s_perm, n, rng);
    s.begin(kNone);
    TEST_ASSERT_TRUE(s.check());
    bool seen[64] = {false};
    for (uint16_t i = 0; i < n; i++) {
      TEST_ASSERT_TRUE(s.at(i) < n);
      TEST_ASSERT_FALSE(seen[s.at(i)]);
      seen[s.at(i)] = true;
    }
  }
}

static void test_uniform_small() {  // n=3: усі 6 перестановок зустрічаються приблизно рівно
  Shuffle s;
  s.attach(s_perm, 3, rng);
  int cnt[6] = {0};
  for (int i = 0; i < 6000; i++) {
    s.begin(kNone);
    int code = s.at(0) * 9 + s.at(1) * 3 + s.at(2);
    static const int codes[6] = {0 * 9 + 1 * 3 + 2, 0 * 9 + 2 * 3 + 1, 1 * 9 + 0 * 3 + 2,
                                 1 * 9 + 2 * 3 + 0, 2 * 9 + 0 * 3 + 1, 2 * 9 + 1 * 3 + 0};
    for (int k = 0; k < 6; k++)
      if (codes[k] == code) cnt[k]++;
  }
  for (int k = 0; k < 6; k++) TEST_ASSERT_TRUE(cnt[k] > 800 && cnt[k] < 1200);
}

static void test_no_repeat_in_circle() {
  Shuffle s;
  const uint16_t n = 20;
  s.attach(s_perm, n, rng);
  for (int c = 0; c < 50; c++) {
    bool seen[n] = {false};
    for (uint16_t i = 0; i < n; i++) {
      uint16_t v = s.next();
      TEST_ASSERT_TRUE(v < n);
      TEST_ASSERT_FALSE(seen[v]);
      seen[v] = true;
    }
    TEST_ASSERT_TRUE(s.atEnd());
    TEST_ASSERT_TRUE(s.check());
  }
}

static void test_boundary_not_equal() {
  const uint16_t sizes[] = {2, 3, 5, 20};
  for (uint16_t n : sizes) {
    Shuffle s;
    s.attach(s_perm, n, rng);
    uint16_t last = kNone;
    for (int i = 0; i < (int)n * 200; i++) {
      uint16_t v = s.next();
      if (i > 0 && i % n == 0) TEST_ASSERT_TRUE(v != last);  // перший нового кола ≠ останньому
      last = v;
    }
  }
  // явний last (поточний ≠ p[n-1])
  Shuffle s;
  s.attach(s_perm, 2, rng);
  for (int i = 0; i < 200; i++) {
    s.begin(kNone);
    s.next();
    s.next();
    uint16_t v = s.next(0);
    TEST_ASSERT_TRUE(v != 0);
  }
}

static void test_single_and_empty() {
  Shuffle s;
  s.attach(s_perm, 1, rng);
  for (int i = 0; i < 5; i++) TEST_ASSERT_EQUAL_UINT16(0, s.next());  // N=1: повтор того ж
  s.attach(s_perm, 0, rng);
  TEST_ASSERT_EQUAL_UINT16(kNone, s.next());
  TEST_ASSERT_FALSE(s.attached());
  s.attach(nullptr, 5, rng);
  TEST_ASSERT_EQUAL_UINT16(kNone, s.next());
  ShufNav n;
  n.init(s_hist, 8, rng);
  n.attach(s_perm, 0);
  TEST_ASSERT_FALSE(n.active());
  n.setOn(true);
  TEST_ASSERT_FALSE(n.active());
}

static void test_enable_sets_current_first() {
  Sim m;
  m.init(10);
  TEST_ASSERT_TRUE(m.n.setOn(true));
  m.n.startAt(7);
  TEST_ASSERT_EQUAL_UINT16(7, m.n.eng().at(0));
  TEST_ASSERT_EQUAL_UINT16(1, m.n.eng().played());
  TEST_ASSERT_TRUE(m.n.eng().check());
  bool seen[10] = {false};
  seen[7] = true;
  for (int i = 0; i < 9; i++) {
    Pick p = m.n.next(m.cur);
    TEST_ASSERT_TRUE(!seen[p.to]);  // 7 не повторюється в цьому колі
    seen[p.to] = true;
    m.go(p);
  }
}

static void test_disable_returns_catalog_order() {
  Sim m;
  m.init(10);
  m.n.setOn(true);
  m.n.startAt(3);
  TEST_ASSERT_TRUE(m.n.setOn(false));
  Pick p = m.n.next(6);
  TEST_ASSERT_EQUAL_UINT16(7, p.to);
  TEST_ASSERT_EQUAL_INT('N', p.why);
  TEST_ASSERT_EQUAL_UINT16(0, m.n.next(9).to);
  TEST_ASSERT_EQUAL_UINT16(9, m.n.prev(0, false).to);
  TEST_ASSERT_EQUAL_UINT16(5, m.n.prev(6, false).to);
  TEST_ASSERT_EQUAL_UINT16(6, m.n.prev(6, true).to);  // перезапуск
}

static void test_initial_track() {
  Sim m;
  m.init(10);
  TEST_ASSERT_EQUAL_UINT16(0, m.n.initialTrack());  // shuffle вимкнено
  m.n.attach(s_perm, 10);
  m.n.setOn(true);
  uint16_t t = m.n.initialTrack();
  TEST_ASSERT_TRUE(t < 10);
  TEST_ASSERT_EQUAL_UINT16(1, m.n.eng().played());
  TEST_ASSERT_EQUAL_UINT16(t, m.n.eng().at(0));
}

static void test_fjump() {
  Sim m;
  m.init(10);
  m.n.setOn(true);
  m.n.startAt(0);
  m.go(Pick{0, HAct::Push, 'p', false});
  // стрибок на незіграний
  Pick p = m.n.jump(7, 'F');
  TEST_ASSERT_EQUAL_UINT16(7, p.to);
  TEST_ASSERT_TRUE(p.act == HAct::Push);
  m.go(p);
  TEST_ASSERT_EQUAL_UINT16(2, m.n.eng().played());
  TEST_ASSERT_EQUAL_UINT16(7, m.n.eng().at(1));
  TEST_ASSERT_TRUE(m.n.eng().check());
  // стрибок на зіграний: коло не змінюється, але трек іде в історію
  p = m.n.jump(0, 'B');
  TEST_ASSERT_EQUAL_UINT16(0, p.to);
  m.go(p);
  TEST_ASSERT_EQUAL_UINT16(2, m.n.eng().played());
  TEST_ASSERT_EQUAL_UINT16(3, m.n.hist().count());
  // решта кола — рівно непрограні
  bool seen[10] = {false};
  seen[0] = seen[7] = true;
  for (int i = 0; i < 8; i++) {
    Pick q = m.n.next(m.cur);
    TEST_ASSERT_TRUE(!seen[q.to]);
    seen[q.to] = true;
    m.go(q);
  }
  TEST_ASSERT_TRUE(m.n.eng().atEnd());
}

static void test_history_back_forward() {
  Sim m;
  m.init(10);
  m.n.setOn(true);
  m.n.startAt(4);
  m.go(Pick{4, HAct::Push, 'p', false});
  uint16_t order[6];
  order[0] = m.n.eng().at(0);
  for (int i = 1; i <= 5; i++) {
    Pick p = m.n.next(m.cur);
    m.go(p);
    order[i] = p.to;
    TEST_ASSERT_EQUAL_UINT16(m.n.eng().at(i), p.to);
  }
  for (int i = 4; i >= 2; i--) {  // PREV ×3 вглиб
    Pick p = m.n.prev(m.cur, false);
    TEST_ASSERT_EQUAL_UINT16(order[i], p.to);
    TEST_ASSERT_TRUE(p.act == HAct::Back);
    m.go(p);
  }
  for (int i = 3; i <= 5; i++) {  // NEXT ×3 вперед по історії
    Pick p = m.n.next(m.cur);
    TEST_ASSERT_EQUAL_UINT16(order[i], p.to);
    TEST_ASSERT_TRUE(p.act == HAct::Fwd);
    m.go(p);
  }
  Pick p = m.n.next(m.cur);  // далі — коло
  TEST_ASSERT_EQUAL_UINT16(m.n.eng().at(6), p.to);
  TEST_ASSERT_EQUAL_INT('S', p.why);
  m.go(p);
  TEST_ASSERT_EQUAL_UINT16(7, m.n.eng().played());
}

static void test_history_overflow() {
  Sim m;
  m.init(30, 4);  // кільце на 4
  m.n.setOn(true);
  m.n.startAt(0);
  m.go(Pick{0, HAct::Push, 'p', false});
  for (int i = 0; i < 10; i++) m.go(m.n.next(m.cur));
  TEST_ASSERT_EQUAL_UINT16(4, m.n.hist().count());
  for (int i = 0; i < 3; i++) {  // 3 кроки назад по історії
    Pick p = m.n.prev(m.cur, false);
    TEST_ASSERT_EQUAL_INT('H', p.why);
    m.go(p);
  }
  Pick p = m.n.prev(m.cur, false);  // історію вичерпано → попередній за каталогом
  TEST_ASSERT_EQUAL_INT('P', p.why);
  TEST_ASSERT_EQUAL_UINT16((m.cur + 29) % 30, p.to);
  TEST_ASSERT_TRUE(p.act == HAct::Push);
}

static void test_history_empty_prev_catalog() {
  Sim m;
  m.init(10);
  m.n.setOn(true);
  m.n.startAt(5);  // історія порожня
  Pick p = m.n.prev(5, false);
  TEST_ASSERT_EQUAL_UINT16(4, p.to);
  TEST_ASSERT_EQUAL_INT('P', p.why);
  m.go(p);
  TEST_ASSERT_EQUAL_UINT16(2, m.n.eng().played());  // 4 теж «зіграний»
}

static void test_history_records_without_shuffle() {
  Sim m;
  m.init(10);
  m.go(Pick{2, HAct::Push, 'p', false});  // історія пишеться й без shuffle
  m.go(m.n.jump(8, 'F'));
  m.go(m.n.next(m.cur));  // каталожний 9
  TEST_ASSERT_EQUAL_UINT16(3, m.n.hist().count());
  m.n.setOn(true);
  m.n.startAt(m.cur);
  Pick p = m.n.prev(m.cur, false);  // по історії (8), а не за каталогом (8 збіглося б — тож далі глибше)
  TEST_ASSERT_EQUAL_UINT16(8, p.to);
  m.go(p);
  p = m.n.prev(m.cur, false);
  TEST_ASSERT_EQUAL_UINT16(2, p.to);  // каталог дав би 7
}

static void test_history_mismatch() {
  Sim m;
  m.init(10);
  m.n.setOn(true);
  m.n.startAt(1);
  m.go(Pick{1, HAct::Push, 'p', false});
  m.go(m.n.next(m.cur));
  uint16_t head = m.cur;
  Pick p = m.n.prev(9, false);  // показаний трек (9) не збігається з історією
  TEST_ASSERT_EQUAL_UINT16(head, p.to);
  TEST_ASSERT_TRUE(p.act == HAct::None);
}

static void test_track_history_dedupe_truncate() {
  uint16_t b[4];
  TrackHistory h;
  h.init(b, 4);
  h.push(5);
  h.push(5);
  TEST_ASSERT_EQUAL_UINT16(1, h.count());
  h.push(6);
  h.push(7);
  h.back();
  h.back();
  TEST_ASSERT_EQUAL_UINT16(5, h.current());
  h.push(9);  // майбутнє (6,7) відсікається
  TEST_ASSERT_EQUAL_UINT16(2, h.count());
  TEST_ASSERT_FALSE(h.hasFwd());
  TEST_ASSERT_EQUAL_UINT16(5, h.peekBack());
}

static void test_end_semantics() {
  Sim m;
  m.init(5);
  m.n.setOn(true);
  m.n.startAt(2);
  m.go(Pick{2, HAct::Push, 'p', false});
  Pick p = m.n.end(m.cur, 1);  // REP=1
  TEST_ASSERT_EQUAL_UINT16(2, p.to);
  TEST_ASSERT_TRUE(p.act == HAct::None && !p.stop);
  for (int i = 0; i < 4; i++) {  // REP=0: до кінця кола
    p = m.n.end(m.cur, 0);
    TEST_ASSERT_TRUE(!p.stop);
    m.go(p);
  }
  p = m.n.end(m.cur, 0);  // коло завершено
  TEST_ASSERT_TRUE(p.stop);
  TEST_ASSERT_TRUE(m.n.ended());
  uint16_t last = m.cur;
  p = m.n.play(m.cur);  // PLAY: нове коло, перший ≠ останньому
  TEST_ASSERT_TRUE(p.to != last);
  TEST_ASSERT_FALSE(m.n.ended());
  m.go(p);
  // REP=2: коло безперервне, межа без збігу
  for (int i = 0; i < 100; i++) {
    uint16_t prev = m.cur;
    p = m.n.end(m.cur, 2);
    TEST_ASSERT_TRUE(!p.stop);
    TEST_ASSERT_TRUE(p.to != prev);
    m.go(p);
  }
  // NEXT після завершення кола теж готує нове коло
  Sim k;
  k.init(3);
  k.n.setOn(true);
  k.n.startAt(0);
  k.go(Pick{0, HAct::Push, 'p', false});
  k.go(k.n.end(k.cur, 0));
  k.go(k.n.end(k.cur, 0));
  TEST_ASSERT_TRUE(k.n.end(k.cur, 0).stop);
  uint16_t lst = k.cur;
  TEST_ASSERT_TRUE(k.n.next(lst).to != lst);
}

static void test_end_catalog() {
  Sim m;
  m.init(4);
  TEST_ASSERT_EQUAL_UINT16(1, m.n.end(0, 0).to);
  Pick p = m.n.end(3, 0);  // REP=0, останній → stop, поточний = перший
  TEST_ASSERT_TRUE(p.stop);
  TEST_ASSERT_EQUAL_UINT16(0, p.to);
  p = m.n.end(3, 2);  // REP=2: цикл
  TEST_ASSERT_TRUE(!p.stop);
  TEST_ASSERT_EQUAL_UINT16(0, p.to);
}

static void test_skip_takes_circle() {
  Sim m;
  m.init(6);
  m.n.setOn(true);
  m.n.startAt(3);
  m.go(Pick{3, HAct::Push, 'p', false});
  Pick p = m.n.skip(m.cur);
  TEST_ASSERT_EQUAL_UINT16(m.n.eng().at(1), p.to);
  TEST_ASSERT_EQUAL_INT('x', p.why);
}

static void test_n1_next() {
  Sim m;
  m.init(1);
  m.n.setOn(true);
  m.n.startAt(0);
  m.go(Pick{0, HAct::Push, 'p', false});
  TEST_ASSERT_EQUAL_UINT16(0, m.n.next(0).to);
  TEST_ASSERT_TRUE(m.n.end(0, 0).stop);
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_permutation_valid);
  RUN_TEST(test_uniform_small);
  RUN_TEST(test_no_repeat_in_circle);
  RUN_TEST(test_boundary_not_equal);
  RUN_TEST(test_single_and_empty);
  RUN_TEST(test_enable_sets_current_first);
  RUN_TEST(test_disable_returns_catalog_order);
  RUN_TEST(test_initial_track);
  RUN_TEST(test_fjump);
  RUN_TEST(test_history_back_forward);
  RUN_TEST(test_history_overflow);
  RUN_TEST(test_history_empty_prev_catalog);
  RUN_TEST(test_history_records_without_shuffle);
  RUN_TEST(test_history_mismatch);
  RUN_TEST(test_track_history_dedupe_truncate);
  RUN_TEST(test_end_semantics);
  RUN_TEST(test_end_catalog);
  RUN_TEST(test_skip_takes_circle);
  RUN_TEST(test_n1_next);
  return UNITY_END();
}

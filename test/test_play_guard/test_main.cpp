#include <stdint.h>
#include <unity.h>

#include <vector>

#include "config.h"
#include "core/play_guard.h"
#include "media/shuffle.h"

// M5c6: резюме у хвості/за кінцем треку, повтор і захист від зациклення для плейлиста з одного треку (SD і USB — той самий рушій SdSource).
// Модель повторює потік SdSource: стартовий вибір → startTrack (fixResume) → Eof → ShufNav::end → повтор з нуля / stop; mock-плеєр віддає Eof
// через (тривалість − позиція) мс, або одразу (EOF після seek) для «зламаного» треку.

void setUp() {}
void tearDown() {}

namespace {
constexpr uint32_t kSize = 10859325;  // розмір файлу (як на залізі)
constexpr uint32_t kDur = 267000;     // тривалість, мс
uint32_t rngFn() { static uint32_t x = 12345; x = x * 1664525u + 1013904223u; return x; }

struct Sess {
  uint32_t startMs, off, lenMs;
};

struct Sim {
  ShufNav nav;
  uint16_t perm[1] = {0}, hist[8] = {0};
  uint8_t rep;
  bool shuf;
  uint32_t instantMs = 0;  // >0: кожна сесія завершується за стільки мс (зламаний трек)
  uint32_t now = 0, endAt = 0;
  uint16_t cur = 0;
  bool stopped = false, tripped = false;
  int sanitized = 0;
  PlayGuard::EndLoopGuard g;
  std::vector<Sess> sess;

  Sim(uint8_t r, bool s) : rep(r), shuf(s) {
    nav.init(hist, 8, rngFn);
    nav.attach(perm, 1);
    nav.setOn(s);
    if (s) nav.startAt(0);
  }
  void startTrack(uint32_t off, uint32_t ms) {
    const PlayGuard::ResumeFix fx = PlayGuard::fixResume(off, ms, kSize, kResumeTailMs, kResumeTailGuard);
    if (fx.reset) {
      sanitized++;
      off = ms = 0;
    } else {
      off = fx.off;
      ms = fx.ms;
    }
    const uint32_t len = instantMs ? instantMs : (kDur > ms ? kDur - ms : 0);
    sess.push_back(Sess{now, off, len});
    endAt = now + len;
    g.start(now);
  }
  // finishScan: запуск зі збереженим резюме
  void begin(uint32_t off, uint32_t ms) {
    Pick p = nav.play(cur);
    cur = p.to;
    nav.commit(p.act, p.to);
    startTrack(off, ms);
  }
  // Один «кінець треку» (pollPlayer: Eof). false = далі нічого не відбувається.
  bool step() {
    if (stopped) return false;
    now = endAt + 25;  // затримка app ≈ одна ітерація loop
    if (g.onEnd(now, kShortEndMs, kShortEndRun)) {
      tripped = stopped = true;
      return false;
    }
    Pick p = nav.end(cur, rep);
    if (p.stop) {
      stopped = true;
      return false;
    }
    cur = p.to;
    nav.commit(p.act, p.to);
    startTrack(0, 0);  // повтор/наступний завжди з початку — старе резюме не застосовується
    return true;
  }
  void run(size_t maxSessions) {
    while (sess.size() < maxSessions && step()) {}
  }
};

// Резюме на відстані leftMs від кінця за швидкістю файлу
void resumeLeft(uint32_t leftMs, uint32_t& off, uint32_t& ms) {
  ms = leftMs >= kDur ? 0 : kDur - leftMs;
  off = (uint32_t)((uint64_t)kSize * ms / kDur);
}
}  // namespace

void test_fix_table() {
  using namespace PlayGuard;
  TEST_ASSERT_FALSE(fixResume(0, 0, kSize, 3000, 4096).reset);
  TEST_ASSERT_TRUE(fixResume(kSize - 1, 266999, kSize, 3000, 4096).reset);
  TEST_ASSERT_TRUE(fixResume(kSize, 267000, kSize, 3000, 4096).reset);
  TEST_ASSERT_TRUE(fixResume(kSize + 12345, 300000, kSize, 3000, 4096).reset);
  uint32_t off, ms;
  const uint32_t resetMs[] = {0, 100, 200, 1000, 2000, 2900};
  for (uint32_t l : resetMs) {
    resumeLeft(l, off, ms);
    TEST_ASSERT_TRUE(fixResume(off, ms, kSize, 3000, 4096).reset);
  }
  const uint32_t keepMs[] = {3100, 4000, 30000, 200000};
  for (uint32_t l : keepMs) {
    resumeLeft(l, off, ms);
    const ResumeFix f = fixResume(off, ms, kSize, 3000, 4096);
    TEST_ASSERT_FALSE(f.reset);
    TEST_ASSERT_EQUAL_UINT32(off, f.off);
    TEST_ASSERT_EQUAL_UINT32(ms, f.ms);
  }
  TEST_ASSERT_FALSE(fixResume(5000, 100, 0, 3000, 4096).reset);  // розмір невідомий — не чіпаємо
  TEST_ASSERT_TRUE(fixResume(kSize - 100000, 0, kSize, 3000, 4096).reset);   // ms=0: хвіст за найгіршою швидкістю
  TEST_ASSERT_FALSE(fixResume(kSize / 2, 0, kSize, 3000, 4096).reset);
}

void test_loop_guard() {
  PlayGuard::EndLoopGuard g;
  g.start(0);
  TEST_ASSERT_FALSE(g.onEnd(50, 200, 3));
  g.start(100);
  TEST_ASSERT_FALSE(g.onEnd(150, 200, 3));
  g.start(200);
  TEST_ASSERT_TRUE(g.onEnd(260, 200, 3));  // третя поспіль
  g.reset();
  g.start(0);
  TEST_ASSERT_FALSE(g.onEnd(10, 200, 3));
  g.start(20);
  TEST_ASSERT_FALSE(g.onEnd(1500, 200, 3));  // довга сесія скидає лічильник
  TEST_ASSERT_EQUAL_UINT8(0, g.run());
  g.start(2000);
  TEST_ASSERT_FALSE(g.onEnd(2010, 200, 3));
  g.start(2020);
  TEST_ASSERT_FALSE(g.onEnd(2030, 200, 3));  // лише 2 поспіль
  TEST_ASSERT_FALSE(g.onEnd(2040, 200, 3));  // без start() — ігнор
}

// Резюме: 0, size-1, size, size+N, останні 200 мс / 1 с / 3 с, а також далеко від кінця; REP 0/1/2 × SHUF 0/1
void test_single_track_matrix() {
  struct R { uint32_t off, ms; bool tail; const char* name; };
  std::vector<R> rs;
  rs.push_back({0, 0, false, "0"});
  rs.push_back({kSize - 1, kDur - 1, true, "size-1"});
  rs.push_back({kSize, kDur, true, "size"});
  rs.push_back({kSize + 5000, kDur + 100, true, "size+N"});
  uint32_t o, m;
  resumeLeft(200, o, m);  rs.push_back({o, m, true, "200мс"});
  resumeLeft(1000, o, m); rs.push_back({o, m, true, "1с"});
  resumeLeft(2900, o, m); rs.push_back({o, m, true, "<3с"});
  resumeLeft(10000, o, m); rs.push_back({o, m, false, "10с"});
  for (const R& r : rs) {
    for (uint8_t rep = 0; rep <= 2; rep++) {
      for (int sh = 0; sh <= 1; sh++) {
        Sim s(rep, sh != 0);
        s.begin(r.off, r.ms);
        s.run(5);
        TEST_ASSERT_FALSE(s.tripped);                       // зациклення не спрацьовує на нормальному треку
        TEST_ASSERT_TRUE(s.sess.size() >= 1);
        const Sess& f = s.sess[0];
        if (r.off == 0 || r.tail) {                         // хвіст/за кінцем/нуль → старт з початку, повна довжина
          TEST_ASSERT_EQUAL_UINT32(0, f.off);
          TEST_ASSERT_EQUAL_UINT32(kDur, f.lenMs);
        } else {
          TEST_ASSERT_EQUAL_UINT32(r.off, f.off);           // далеко від кінця: резюме застосовано
          TEST_ASSERT_EQUAL_UINT32(kDur - r.ms, f.lenMs);
        }
        for (size_t i = 1; i < s.sess.size(); i++) {        // повтор завжди з нуля, повної довжини
          TEST_ASSERT_EQUAL_UINT32(0, s.sess[i].off);
          TEST_ASSERT_EQUAL_UINT32(kDur, s.sess[i].lenMs);
        }
        if (rep == 0) {                                     // REP0: одна сесія, далі stop (SHUF 0/1)
          TEST_ASSERT_EQUAL((int)1, (int)s.sess.size());
          TEST_ASSERT_TRUE(s.stopped);
        } else {                                            // REP1/2: повтор того самого треку (довжина всіх = повна)
          TEST_ASSERT_EQUAL((int)5, (int)s.sess.size());
          TEST_ASSERT_FALSE(s.stopped);
        }
        TEST_ASSERT_EQUAL_UINT16(0, s.cur);
      }
    }
  }
}

// Зламаний трек: EOF одразу після seek → щоразу сесія ≈5 мс. REP1/2 — зупинка після 3-ї; REP0 — звичайний stop після першої.
void test_instant_eof_loop_stops() {
  for (uint8_t rep = 0; rep <= 2; rep++) {
    for (int sh = 0; sh <= 1; sh++) {
      Sim s(rep, sh != 0);
      s.instantMs = 5;
      s.begin(kSize, kDur);
      s.run(1000);
      TEST_ASSERT_TRUE(s.stopped);
      if (rep == 0) {
        TEST_ASSERT_EQUAL((int)1, (int)s.sess.size());
        TEST_ASSERT_FALSE(s.tripped);
      } else {
        TEST_ASSERT_EQUAL((int)3, (int)s.sess.size());   // рівно kShortEndRun миттєвих завершень
        TEST_ASSERT_TRUE(s.tripped);
      }
    }
  }
}

// Короткий, але реальний трек (>200 мс) зациклення не викликає
void test_short_real_track_not_a_loop() {
  Sim s(1, false);
  s.instantMs = 800;
  s.begin(0, 0);
  s.run(50);
  TEST_ASSERT_FALSE(s.tripped);
  TEST_ASSERT_EQUAL((int)50, (int)s.sess.size());
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_fix_table);
  RUN_TEST(test_loop_guard);
  RUN_TEST(test_single_track_matrix);
  RUN_TEST(test_instant_eof_loop_stops);
  RUN_TEST(test_short_real_track_not_a_loop);
  return UNITY_END();
}

// minimp3: ТІЛЬКИ тут визначається реалізація (MINIMP3_IMPLEMENTATION).
// -O2 для файлу: декодер у -Os помітно повільніший. Попередження сторонньої бібліотеки приглушені.
#pragma GCC optimize("O2")
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wunused-variable"
#pragma GCC diagnostic ignored "-Wunused-but-set-variable"
#pragma GCC diagnostic ignored "-Wmaybe-uninitialized"
#pragma GCC diagnostic ignored "-Wmisleading-indentation"
#pragma GCC diagnostic ignored "-Wimplicit-fallthrough"
#pragma GCC diagnostic ignored "-Wsign-compare"
#pragma GCC diagnostic ignored "-Wparentheses"
#pragma GCC diagnostic ignored "-Wstrict-aliasing"
#define MINIMP3_IMPLEMENTATION
#define MINIMP3_ONLY_MP3
#define MINIMP3_NO_SIMD
#include "third_party/minimp3/minimp3.h"
#pragma GCC diagnostic pop

#include "audio/mp3_player.h"

#include <Arduino.h>
#include <stddef.h>
#include <string.h>

#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "audio/audio_out.h"
#include "audio/pcm_arena.h"
#include "config.h"
#include "util/diag.h"
#include "util/log.h"

namespace {

constexpr const char* TAG = "mp3";

// Набір SD усередині PcmArena. Порядок важливий: стек останній і починається далі за kBtRingBytes,
// тож кільце BT (перші 16 КБ) його не зачіпає, а задача «mp3» живе припаркованою між сесіями.
struct SdSet {
  mp3dec_t dec;                                          // стан декодера ≈ 6,7 КБ
  alignas(4) uint8_t in[kDecInBytes];                    // вхідний буфер читання
  alignas(4) int16_t pcm[MINIMP3_MAX_SAMPLES_PER_FRAME]; // PCM-кадр (≤ 1152 × 2 × 2 Б)
  alignas(16) uint8_t stack[kDecStackBytes];             // статичний стек задачі (у ньому scratch minimp3 ≈ 16,3 КБ)
};
static_assert(sizeof(SdSet) <= kPcmArenaBytes, "SdSet не влізає в kPcmArenaBytes: збільшіть константу в config.h");
static_assert(offsetof(SdSet, stack) >= kBtRingBytes, "стек декодера не має перетинатися з BT-кільцем");

StaticTask_t s_tcb;
TaskHandle_t s_task = nullptr;

inline SdSet* set() { return reinterpret_cast<SdSet*>(g_pcmArena); }

enum : uint8_t { kReqNone = 0, kReqStart, kReqPause, kReqResume, kReqStop };

// Стан поточної сесії (лише задача «mp3»; app читає inLen/inPos для DBG)
struct Sess {
  uint32_t inPos, inLen, id3Skip, junk, badRun;
  bool eof, metaDone;
  uint32_t lastCh;
  uint64_t posUs, sumDecUs, sumFrameUs;
  int64_t lastT;       // мкс: кінець попереднього write()
  int32_t cushUs;      // оцінка запасу даних у DMA, мкс
  uint32_t fastRun;    // скільки кадрів підряд write() не блокував (мʼют/abort)
  uint32_t fEnd;       // файловий зсув кінця даних у вхідному буфері (зсув in[inPos] = fEnd - (inLen - inPos))
  uint32_t probeMs;    // тривалість, розрахована пробою першого кадру при відновленні
  bool probed, probeEst;
};
Sess sx;

inline uint32_t be32(const uint8_t* p) {
  return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

// Xing/Info/VBRI у першому кадрі. true = службовий кадр (звуку немає); frames = кількість кадрів (0 = невідомо).
bool vbrHeader(const uint8_t* f, int n, uint32_t& frames) {
  frames = 0;
  if (n < 40) return false;
  bool mpeg1 = ((f[1] >> 3) & 3) == 3;
  bool mono = ((f[3] >> 6) & 3) == 3;
  bool crc = (f[1] & 1) == 0;
  int off = 4 + (crc ? 2 : 0) + (mpeg1 ? (mono ? 17 : 32) : (mono ? 9 : 17));
  if (n >= off + 8 && (!memcmp(f + off, "Xing", 4) || !memcmp(f + off, "Info", 4))) {
    uint32_t fl = be32(f + off + 4);
    if ((fl & 1) && n >= off + 12) frames = be32(f + off + 8);
    return true;
  }
  if (n >= 54 && !memcmp(f + 36, "VBRI", 4)) {
    frames = be32(f + 50);
    return true;
  }
  return false;
}

struct DurInfo {
  uint32_t ms;
  bool est, meta;
};

// Тривалість за першим кадром: Xing/Info/VBRI, інакше оцінка за бітрейтом. meta = службовий кадр (звуку немає).
DurInfo calcDur(const uint8_t* fp, int fn, int smp, uint32_t hz, int kbps, uint32_t fileSize, uint32_t id3Skip) {
  DurInfo r{0, false, false};
  uint32_t vf = 0;
  r.meta = vbrHeader(fp, fn, vf);
  uint64_t d = 0;
  if (vf) {
    d = (uint64_t)vf * (uint32_t)smp * 1000ULL / hz;
  } else if (kbps > 0 && fileSize > id3Skip) {
    d = (uint64_t)(fileSize - id3Skip) * 8ULL / (uint32_t)kbps;
    r.est = true;
  }
  r.ms = (uint32_t)d;
  return r;
}

}  // namespace

// ============================================================ керування (app)

bool Mp3Player::start() {
  if (s_task) return true;
  if (!out_ || !rd_ || PcmArena::owner() != PcmArena::Owner::Sd) return false;
  st_ = St::Idle;
  req_ = kReqNone;
  s_task = xTaskCreateStaticPinnedToCore(&Mp3Player::taskEntry, "mp3", kDecStackBytes, this, kPrioDecoder,
                                         reinterpret_cast<StackType_t*>(set()->stack), &s_tcb, kCoreAudio);
  if (!s_task) {
    LOGE(TAG, "не вдалося створити задачу mp3");
    return false;
  }
  Diag::registerTask("mp3", s_task);
  LOGI(TAG, "задача mp3 створена: стек %u Б (статичний, у області), prio %d, ядро %d", (unsigned)kDecStackBytes,
       kPrioDecoder, kCoreAudio);
  return true;
}

void Mp3Player::startSessionAt(uint32_t off, uint32_t ms) {
  if (!s_task) return;
  memset(&stats_, 0, sizeof stats_);
  ev_ = Ev::None;
  rsmOff_ = off;
  rsmMs_ = ms;
  publishPos(off, off ? ms : 0);  // TIME одразу показує збережений час
  durMs_ = 0;
  durEst_ = false;
  req_ = kReqStart;
  st_ = St::Run;
  xTaskNotifyGive(s_task);
}

bool Mp3Player::pause() {
  if (!s_task) return false;
  if (st_ == St::Paused) return true;
  if (st_ != St::Run) return false;
  req_ = kReqPause;
  xTaskNotifyGive(s_task);
  uint32_t t0 = millis();
  while (st_ == St::Run && (uint32_t)(millis() - t0) < kDecStopWaitMs) vTaskDelay(1);
  return st_ == St::Paused;
}

void Mp3Player::resume() {
  if (!s_task || st_ != St::Paused) return;
  req_ = kReqResume;
  st_ = St::Run;
  xTaskNotifyGive(s_task);
}

void Mp3Player::stopSession() {
  if (!s_task || st_ == St::Idle) return;
  req_ = kReqStop;
  xTaskNotifyGive(s_task);
  uint32_t t0 = millis();
  while (st_ != St::Idle && (uint32_t)(millis() - t0) < kDecStopWaitMs) vTaskDelay(1);
  if (st_ != St::Idle) LOGE(TAG, "стоп сесії: задача не відповіла за %u мс", (unsigned)kDecStopWaitMs);
}

// Єдиний писар у кожен момент: app (сесія в Idle) або задача mp3 (Run) — передача власності через notify/req.
void Mp3Player::publishPos(uint32_t off, uint32_t ms) {
  posSeq_ = posSeq_ + 1;  // непарний: запис іде
  __sync_synchronize();
  posOff_ = off;
  posMs_ = ms;
  __sync_synchronize();
  posSeq_ = posSeq_ + 1;
}

bool Mp3Player::position(uint32_t& off, uint32_t& ms) const {
  for (int i = 0; i < 8; i++) {
    uint32_t a = posSeq_;
    if (a & 1u) continue;
    __sync_synchronize();
    uint32_t o = posOff_, m = posMs_;
    __sync_synchronize();
    if (posSeq_ == a) {
      off = o;
      ms = m;
      return true;
    }
  }
  return false;
}

Mp3Player::Ev Mp3Player::takeEvent() {
  Ev e = ev_;
  if (e != Ev::None) ev_ = Ev::None;
  return e;
}

void Mp3Player::stats(Mp3Stats& o) const {
  o = stats_;
  o.inFill = sx.inLen - sx.inPos;
  o.inCap = kDecInBytes;
}

uint8_t* Mp3Player::scratch(size_t& cap) {
  cap = kDecInBytes;
  return set()->in;
}

void Mp3Player::mark(uint8_t w) {
  where_ = w;
  whereMs_ = millis();
}

const char* Mp3Player::whereName(uint8_t w) {
  switch (w) {
    case kWIdle: return "idle(чекає notify)";
    case kWOpen: return "старт(теги/seek)";
    case kWWaitUnmute: return "чекає розмʼюту DAC";
    case kWDecode: return "декодує";
    case kWFill: return "читає файл";
    case kWWrite: return "write() в AudioOut";
    case kWParked: return "пауза";
    case kWDrain: return "хвіст тиші";
    case kWFinish: return "завершення";
    default: return "?";
  }
}

void Mp3Player::snap(Snap& o) const {
  const uint32_t now = millis();
  o.st = st_;
  o.ev = ev_;
  o.req = req_;
  o.where = where_;
  o.whereAgoMs = now - whereMs_;
  o.starts = nStart_;
  o.endEof = nEndEof_;
  o.endRead = nEndRead_;
  o.endDec = nEndDec_;
  o.endStop = nEndStop_;
  const uint32_t le = lastEndMs_;
  o.lastEndAgoMs = le ? now - le : 0xFFFFFFFFu;
  o.cushUs = sx.cushUs;
  o.inFill = sx.inLen - sx.inPos;
  o.posMs = posMs_;
}

// ============================================================ задача «mp3»

void Mp3Player::taskEntry(void* self) { static_cast<Mp3Player*>(self)->taskMain(); }

uint8_t Mp3Player::takeReq() { return __atomic_exchange_n(&req_, (uint8_t)kReqNone, __ATOMIC_ACQ_REL); }

void Mp3Player::taskMain() {
  for (;;) {
    mark(kWIdle);
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);  // Idle: блокуємось без витрат CPU
    uint8_t rq = takeReq();
    if (rq == kReqStart) runSession();
    else if (rq != kReqNone) st_ = St::Idle;  // Stop/Pause/Resume без сесії
  }
}

void Mp3Player::finish(Ev e) {
  switch (e) {  // M5c1: лічильники причин завершення
    case Ev::Eof: nEndEof_ = nEndEof_ + 1; break;
    case Ev::ReadFail: nEndRead_ = nEndRead_ + 1; break;
    case Ev::DecodeFail: nEndDec_ = nEndDec_ + 1; break;
    default: nEndStop_ = nEndStop_ + 1; break;
  }
  lastEndMs_ = millis() | 1u;
  mark(kWFinish);
#if SKIP_TEST_CMDS
  inj_ = 0;  // невикористана штучна помилка не переходить на наступну сесію
#endif
  ev_ = e;
  __sync_synchronize();
  st_ = St::Idle;  // app чекає саме на Idle (стоп) або бачить подію (кінець/помилка)
}

void Mp3Player::runSession() {
  SdSet* S = set();
  nStart_ = nStart_ + 1;
  mark(kWOpen);
  memset(&sx, 0, sizeof sx);
  memset(&S->dec, 0, sizeof S->dec);
  mp3dec_init(&S->dec);
  const uint32_t rOff = rsmOff_, rMs = rsmMs_;
  parseTags();  // теги на скретчі (вхідний буфер), далі файл знову з початку
  Ev e = Ev::ReadFail;
  if (rd_->seek(0)) {
    sx.fEnd = 0;
    if (skipId3() && (!rOff || seekResume(rOff, rMs))) e = decodeLoop();
  }
  if (e == Ev::Eof) drainSilence();
  finish(e);
}

// Докачує вхід до повного буфера (залишок зсувається на початок). false = помилка читання.
bool Mp3Player::fill() {
  SdSet* S = set();
  mark(kWFill);
  uint32_t avail = sx.inLen - sx.inPos;
  if (sx.inPos > 0) {
    if (avail) memmove(S->in, S->in + sx.inPos, avail);
    sx.inPos = 0;
    sx.inLen = avail;
  }
  while (sx.inLen < kDecInBytes && !sx.eof) {
    int64_t t = esp_timer_get_time();
    size_t n = rd_->read(S->in + sx.inLen, kDecInBytes - sx.inLen);
    uint32_t dt = (uint32_t)(esp_timer_get_time() - t);
    stats_.rdBusyUs += dt;
    if (dt > stats_.rdMaxUs) stats_.rdMaxUs = dt;
    if (n == 0) {
      if (rd_->failed()) return false;
      sx.eof = true;
      break;
    }
    stats_.rdBytes += (uint32_t)n;
    sx.inLen += (uint32_t)n;
    sx.fEnd += (uint32_t)n;
  }
  return true;
}

// Розбір ID3 (M3d): вхідний буфер SdSet — скретч (decodeLoop його ще не використовує); результат — у tags_.
void Mp3Player::parseTags() {
  Id3::Tags t;  // стек задачі
  Id3::Info inf;
  int64_t t0 = esp_timer_get_time();
  uint8_t mask = Id3::parse(*rd_, set()->in, kDecInBytes, t, &inf);
  tagUs_ = (uint32_t)(esp_timer_get_time() - t0);
  tagMask_ = mask;
  tagVer_ = inf.ver;
  memcpy(&tags_, &t, sizeof tags_);
  __sync_synchronize();
  tagSeq_ = tagSeq_ + 1;  // публікація: app бачить готові теги
}

// Лише пропуск ID3v2 за розміром (самі теги розбирає parseTags). false = помилка читання.
bool Mp3Player::skipId3() {
  SdSet* S = set();
  for (int k = 0; k < 4; k++) {  // кілька тегів підряд трапляються рідко
    if (!fill()) return false;
    if (sx.inLen - sx.inPos < 10) return true;
    const uint8_t* h = S->in + sx.inPos;
    if (memcmp(h, "ID3", 3) != 0) return true;
    if ((h[6] | h[7] | h[8] | h[9]) & 0x80) return true;  // не syncsafe: це не тег
    uint32_t sz = ((uint32_t)h[6] << 21) | ((uint32_t)h[7] << 14) | ((uint32_t)h[8] << 7) | h[9];
    uint32_t total = 10 + sz + ((h[5] & 0x10) ? 10 : 0);   // + футер, якщо є
    sx.id3Skip += total;
    if (sx.id3Skip > rd_->size()) sx.id3Skip = rd_->size();
    if (!rd_->seek(sx.id3Skip)) return false;
    sx.inPos = sx.inLen = 0;
    sx.fEnd = sx.id3Skip;
    sx.eof = false;
  }
  return true;
}

// Відновлення (M5b1): після skipId3 (у буфері початок аудіо). Спершу проба першого кадру — тривалість (Xing/VBRI),
// бо після seek його вже не буде; далі скидання декодера й seek на off. true = продовжувати (зсув може бути проігноровано).
bool Mp3Player::seekResume(uint32_t off, uint32_t ms) {
  SdSet* S = set();
  const uint32_t sz = rd_->size();
  if (off <= sx.id3Skip || off > sz || sz - off < kResumeTailGuard) {  // сміття або майже кінець → з початку
    publishPos(0, 0);
    return true;
  }
  mp3dec_frame_info_t info;
  memset(&info, 0, sizeof info);
  uint32_t avail = sx.inLen - sx.inPos;
  int smp = avail ? mp3dec_decode_frame(&S->dec, S->in + sx.inPos, (int)avail, S->pcm, &info) : 0;
  if (smp > 0 && info.frame_bytes > 0 && info.hz > 0) {
    DurInfo d = calcDur(S->in + sx.inPos + info.frame_offset, info.frame_bytes - info.frame_offset, smp, (uint32_t)info.hz,
                        info.bitrate_kbps, sz, sx.id3Skip);
    sx.probed = true;
    sx.probeMs = d.ms;
    sx.probeEst = d.est;
  }
  mp3dec_init(&S->dec);  // стан після проби не потрібен
  if (!rd_->seek(off)) return false;
  sx.inPos = sx.inLen = 0;
  sx.eof = false;
  sx.fEnd = off;
  sx.posUs = (uint64_t)ms * 1000ULL;
  publishPos(off, ms);
  LOGI(TAG, "відновлення: зсув %u Б, %u мс", (unsigned)off, (unsigned)ms);
  return true;
}

// Чекає розмʼюту DAC (менеджер розмʼючує після PLAY). false = надійшов запит app.
// ВАЖЛИВО: чекаємо й поки йде mute() (aborting): write() у цей час миттєво відкидає дані, і декодер (prio 5)
// без блокувань «промотав» би трек, не даючи app (prio 1) завершити mute() — це був живий лок на залізі.
bool Mp3Player::waitUnmuted() {
  const TickType_t tick = pdMS_TO_TICKS(2) ? pdMS_TO_TICKS(2) : 1;  // ніколи не 0 (busy-loop)
  if (out_->isMuted() || out_->aborting()) mark(kWWaitUnmute);
  while (out_->isMuted() || out_->aborting()) {
    if (req_ != kReqNone) return false;
    ulTaskNotifyTake(pdTRUE, tick);
    sx.fastRun = 0;
  }
  return true;
}

// Пауза: чекаємо Resume/Stop без витрат CPU. true = продовжувати, false = Stop.
bool Mp3Player::parkPaused() {
  st_ = St::Paused;
  mark(kWParked);
  for (;;) {
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    uint8_t rq = takeReq();
    if (rq == kReqResume) {
      st_ = St::Run;
      sx.lastT = 0;  // після паузи оцінку запасу DMA починаємо заново
      sx.fastRun = 0;
      return true;
    }
    if (rq == kReqStop) return false;
  }
}

// Віддає PCM-кадр у AudioOut (блокування на DMA = темп відтворення). Перериваємось на запит app.
void Mp3Player::writePcm(size_t bytes) {
  const uint8_t* p = reinterpret_cast<const uint8_t*>(set()->pcm);
  size_t off = 0;
  mark(kWWrite);
  while (off < bytes) {
    size_t w = out_->write(p + off, bytes - off, kPcmWriteTimeoutMs);
    if (w == 0) {
      stats_.writeTimeouts++;
      if (req_ != kReqNone) return;  // мʼют/пауза/стоп: залишок кадру не потрібен
      continue;
    }
    off += w;
  }
}

// Природний кінець: дописуємо тишу на глибину DMA, щоб мʼют не обрізав останні мілісекунди звуку.
void Mp3Player::drainSilence() {
  if (!sx.lastCh || out_->isMuted()) return;
  mark(kWDrain);
  SdSet* S = set();
  size_t bytes = (size_t)(kDmaFrames + kChunkFrames) * sx.lastCh * 2;
  if (bytes > sizeof S->pcm) bytes = sizeof S->pcm;
  memset(S->pcm, 0, bytes);
  writePcm(bytes);
}

Mp3Player::Ev Mp3Player::decodeLoop() {
  SdSet* S = set();
  mp3dec_frame_info_t info;
  for (;;) {
    // 1) запити app
    uint8_t rq = takeReq();
    if (rq == kReqStop) return Ev::None;
    if (rq == kReqPause && !parkPaused()) return Ev::None;

    // 2) поки DAC замʼючений — не декодуємо (write() відкидав би дані, і ми «промотали» б трек)
    mark(kWDecode);
    if (!waitUnmuted()) continue;
    mark(kWDecode);
#if SKIP_TEST_CMDS
    if (inj_) {  // M5c1: штучна помилка тим самим шляхом і в той самий момент (після розмʼюту), що й справжня
      Ev ie = (Ev)inj_;
      inj_ = 0;
      return ie;
    }
#endif

    // 3) вхід: докачуємо, коли лишилось менше за два максимальні кадри
    uint32_t avail = sx.inLen - sx.inPos;
    if (!sx.eof && avail < kDecInRefill) {
      if (!fill()) return Ev::ReadFail;
      mark(kWDecode);
      avail = sx.inLen - sx.inPos;
    }
    if (avail == 0) return stats_.frames ? Ev::Eof : Ev::DecodeFail;

    // 4) декодування одного кадру
    memset(&info, 0, sizeof info);
    int64_t t0 = esp_timer_get_time();
    int smp = mp3dec_decode_frame(&S->dec, S->in + sx.inPos, (int)avail, S->pcm, &info);
    uint32_t decUs = (uint32_t)(esp_timer_get_time() - t0);

    if (info.frame_bytes <= 0) {  // кадр неповний (кінець файлу) або не знайдено
      if (sx.eof) return stats_.frames ? Ev::Eof : Ev::DecodeFail;
      sx.inPos += 1;              // гарантований прогрес
      stats_.skipped++;
      if (!stats_.frames) sx.junk++;
      if (++sx.badRun > kDecBadRunMax) return Ev::DecodeFail;
      continue;
    }
    const uint8_t* fp = S->in + sx.inPos + info.frame_offset;
    int fn = info.frame_bytes - info.frame_offset;
    sx.inPos += (uint32_t)info.frame_bytes;

    if (smp <= 0) {               // биті дані / сміття: minimp3 уже пропустив їх, шукаємо наступний кадр
      stats_.skipped++;
      if (sx.badRun++ == 0 && stats_.frames) stats_.resyncs++;
      if (!stats_.frames) {
        sx.junk += (uint32_t)info.frame_bytes;
        if (sx.junk > kDecJunkMax) return Ev::DecodeFail;
      } else if (sx.badRun > kDecBadRunMax) {
        return Ev::DecodeFail;
      }
      continue;
    }
    sx.badRun = 0;
    if (info.frame_offset > 0 && stats_.frames) stats_.resyncs++;

    uint32_t hz = (uint32_t)info.hz;
    uint32_t ch = (uint32_t)info.channels;
    if (hz == 0 || (ch != 1 && ch != 2)) continue;

    // 5) перший кадр: тривалість (Xing/Info/VBRI, інакше оцінка за бітрейтом); після відновлення — з проби
    if (!sx.metaDone) {
      sx.metaDone = true;
      if (sx.probed) {
        durMs_ = sx.probeMs;
        durEst_ = sx.probeEst;
        infoSeq_ = infoSeq_ + 1;
      } else {
        DurInfo d = calcDur(fp, fn, smp, hz, info.bitrate_kbps, rd_->size(), sx.id3Skip);
        durEst_ = d.est;
        durMs_ = d.ms;
        infoSeq_ = infoSeq_ + 1;
        if (d.meta) continue;     // службовий кадр: звуку немає
      }
    }

    // 6) формат: зміна частоти/каналів → мʼют → setFormat → анмʼют (робить AudioOut::setFormat)
    if (out_->rate() != hz || out_->channels() != ch) {
      if (!out_->setFormat(hz, 16, (uint8_t)ch)) return Ev::DecodeFail;
      sx.lastT = 0;
    }

    // 7) вивід + оцінка underrun (запас даних у DMA, модель «дірявого відра», без довгого дрейфу годинника)
    size_t bytes = (size_t)smp * ch * 2;
    uint32_t frameUs = (uint32_t)((uint64_t)smp * 1000000ULL / hz);
    sx.lastCh = ch;
    int64_t tw = esp_timer_get_time();
    if (sx.lastT) {
      sx.cushUs -= (int32_t)(tw - sx.lastT);
      if (sx.cushUs < -(int32_t)kUnderTolUs) stats_.underruns++;
      if (sx.cushUs < 0) sx.cushUs = 0;
    } else {
      sx.cushUs = 0;
    }
    writePcm(bytes);
    int64_t te = esp_timer_get_time();
    int32_t c = sx.cushUs + (int32_t)frameUs - (int32_t)(te - tw);
    int32_t dma = (int32_t)((uint64_t)kDmaFrames * 1000000ULL / hz);
    if (c > dma) c = dma;
    if (c < 0) c = 0;
    sx.cushUs = c;
    sx.lastT = te;
    // Страховка: якщо write() не блокує (замʼючено/abort, гонка), віддаємо CPU, щоб app не голодував.
    if (te - tw < 500) {
      if (++sx.fastRun >= 4) {
        stats_.paced++;
        vTaskDelay(1);
      }
    } else {
      sx.fastRun = 0;
    }

    // 8) лічильники й позиція
    stats_.frames++;
    sx.posUs += frameUs;
    publishPos(sx.fEnd - (sx.inLen - sx.inPos), (uint32_t)(sx.posUs / 1000ULL));  // початок наступного кадру
    sx.sumDecUs += decUs;
    sx.sumFrameUs += frameUs;
    stats_.decAvgUs = (uint32_t)(sx.sumDecUs / stats_.frames);
    stats_.frameAvgUs = (uint32_t)(sx.sumFrameUs / stats_.frames);
    stats_.loadAvgPm = (uint32_t)(sx.sumDecUs * 1000ULL / sx.sumFrameUs);
    if (decUs > stats_.decMaxUs) stats_.decMaxUs = decUs;
    uint32_t pm = frameUs ? (uint32_t)((uint64_t)decUs * 1000ULL / frameUs) : 0;
    if (pm > stats_.loadMaxPm) stats_.loadMaxPm = pm;
    stats_.hz = hz;
    stats_.ch = ch;
    stats_.kbps = (uint32_t)info.bitrate_kbps;
    stats_.pcmBytes = (uint32_t)bytes;
  }
}

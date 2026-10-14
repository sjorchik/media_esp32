#include "sources/sd_source.h"

#include <Arduino.h>
#include <stdio.h>
#include <string.h>

#include "audio/pcm_arena.h"
#include "config.h"
#include "core/app.h"
#include "core/device_state.h"
#include "core/events.h"
#include "util/diag.h"
#include "util/log.h"
#include "util/utf8.h"

namespace {

constexpr const char* TAG = "sdsrc";
// Код з контракту. Якщо в proto::errc є відповідна константа — замінити.
constexpr const char* kErrTooMany = "too_many_files";
constexpr int kListPerSlice = 12;

portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;

inline bool due(uint32_t now, uint32_t t) { return (int32_t)(now - t) >= 0; }

const char* scanStName(Scanner::St s) {
  switch (s) {
    case Scanner::St::Running: return "running";
    case Scanner::St::Done: return "done";
    case Scanner::St::Overflow: return "overflow";
    case Scanner::St::Failed: return "failed";
    default: return "idle";
  }
}

}  // namespace

// ============================================================ життєвий цикл

bool SdSource::begin() {
  dec_.init(g_audio, media_.reader());
  bool ok = media_.begin();
  media_.poll(millis());  // початковий mount; події не шлемо — MEDIA піде з HELLO/GET
  DeviceState::setMedia(media_.mounted(), DeviceState::usb());
  LOGI(TAG, "begin: картка %s", media_.mounted() ? "є" : "немає");
  return ok;
}

Res SdSource::activate() {
  if (!PcmArena::acquire(PcmArena::Owner::Sd)) return Res::BadState;  // область ще у BT: порядок порушено
  if (!dec_.start()) {                                                // задача mp3 (створюється один раз)
    PcmArena::release(PcmArena::Owner::Sd);
    return Res::DecodeFail;
  }
  active_ = true;
  scanRetry_ = 0;
  if (!media_.mounted()) {
    st_ = PlayState::Idle;
    LOGI(TAG, "activate: носія немає");
    return Res::NoMedia;  // менеджер пошле STATE,sd,idle і ERR,no_media
  }
  startScan();
  return Res::Ok;
}

void SdSource::deactivate() {
  g_audio.mute();  // мʼют ДО зупинки (ідемпотентно: менеджер уже мʼютив) — без клацань і «хвостів»
  stopJobs();      // скан, DBG-читання, сесія декодера; після цього файл закрито
  active_ = false;
  st_ = PlayState::Idle;
  PcmArena::release(PcmArena::Owner::Sd);  // область вільна для BT
}

void SdSource::loop(uint32_t now) {
  service(now);
  if (!active_) return;
  if (st_ == PlayState::Scan) scanStep();
  else if (st_ == PlayState::Play || st_ == PlayState::Pause) pollPlayer(now);
}

void SdSource::service(uint32_t now) {
  MediaEvent ev = media_.poll(now);
  if (ev != MediaEvent::None) onMedia(ev);
  runDbg();
}

// ============================================================ hot-plug

void SdSource::onMedia(MediaEvent ev) {
  stopJobs();
  pl_.clear();
  scanned_ = false;
  scanRetry_ = 0;
  DeviceState::setMedia(media_.mounted(), DeviceState::usb());
  LOGI(TAG, "картку %s", ev == MediaEvent::Inserted ? "вставлено" : "вийнято");
  g_sources.onMediaChanged();  // MEDIA, refreshMedia() активного SD, STATE
  Diag::logHeap("sd-media");
}

Res SdSource::refreshMedia() {
  if (!active_) return Res::Ok;
  if (!media_.mounted()) {
    if (st_ == PlayState::Idle) return Res::Ok;  // помилку вже було
    stopJobs();
    st_ = PlayState::Idle;
    return Res::NoMedia;
  }
  if (st_ == PlayState::Idle) startScan();  // картку вставили при активному SD
  return Res::Ok;
}

void SdSource::stopJobs() {
  scanner_.abort();
  endSession();  // зупиняє декодер і закриває файл (це ж закриває й DBG,sdread)
  rdOpen_ = false;
  job_ = Dbg::None;
}

void SdSource::endSession() {
  if (dec_.sessionActive()) {
    g_audio.mute();  // тиша до зупинки продюсера
    dec_.stopSession();
  }
  dec_.takeEvent();
  if (media_.reader().isOpen()) media_.reader().close();
}

SourceStatus SdSource::status() const {
  bool pl = st_ == PlayState::Play || st_ == PlayState::Pause;
  return SourceStatus{st_, pl ? dec_.posMs() / 1000 : 0, pl ? dec_.durMs() / 1000 : 0};
}

// ============================================================ скан

void SdSource::startScan() {
  st_ = PlayState::Scan;
  scanned_ = true;
  cur_ = 0;
  scanner_.start(media_, pl_);
  LOGI(TAG, "скан розпочато");
}

void SdSource::scanStep() {
  Scanner::St r = scanner_.step(kScanSliceMs);
  if (r != Scanner::St::Running) finishScan(r);
}

void SdSource::finishScan(Scanner::St r) {
  const ScanStats& s = scanner_.stats();
  LOGI(TAG, "скан %s: %u мс (робота %u мс), треків=%u папок=%u (з треками %u), арена=%u/%u", scanStName(r),
       (unsigned)s.wallMs, (unsigned)s.busyMs, (unsigned)s.tracks, (unsigned)s.foldersTotal,
       (unsigned)s.foldersWithTracks, (unsigned)pl_.used(), (unsigned)Playlist::capacity());
  Diag::logHeap("sd-scan");
  if (r == Scanner::St::Failed) {
    if (media_.mounted() && scanRetry_ < kSdScanRetries) {  // частоту могло бути знижено — пробуємо ще
      scanRetry_++;
      LOGW(TAG, "помилка скану, повтор %u/%u", (unsigned)scanRetry_, (unsigned)kSdScanRetries);
      startScan();
      return;
    }
    st_ = PlayState::Idle;  // якщо носій зник — Removed повідомить hot-plug
    if (media_.mounted()) Events::err(proto::errc::kReadFail, "scan");
    return;
  }
  if (pl_.tracks() == 0) {
    st_ = PlayState::Idle;
    Events::err(proto::errc::kNoMedia, "empty");  // носій є, але mp3 немає
    return;
  }
  cur_ = 0;
  st_ = PlayState::Stop;
  emitTrackFolder();  // TRACK, FOLDER; далі STATE,sd,stop від менеджера
  if (r == Scanner::St::Overflow) Events::err(kErrTooMany);
}

void SdSource::emitTrackFolder() {
  Events::track(cur_ + 1, pl_.tracks());
  int f = pl_.folderOf(cur_);
  char nm[kEvFolderNameMax + 1] = {0};
  if (f >= 0) {
    char full[kMediaNameMax];
    if (pl_.folderName(f, full, sizeof full)) Utf8::copySafe(nm, sizeof nm, full);
  }
  Events::folder(nm, f >= 0 ? pl_.folderTracks(f) : 0);
}

void SdSource::emitInfo() {
  if (!active_ || !pl_.tracks()) return;
  if (st_ == PlayState::Stop || st_ == PlayState::Play || st_ == PlayState::Pause) emitTrackFolder();
  if (st_ == PlayState::Play || st_ == PlayState::Pause) sendTime();
}

// ============================================================ відтворення (M3b)

Res SdSource::nav() {  // NEXT/PREV/FNEXT/FPREV — M3c
  if (st_ == PlayState::Scan) return Res::BadState;
  if (st_ == PlayState::Idle) return media_.mounted() ? Res::BadState : Res::NoMedia;
  return Res::NotImpl;
}

void SdSource::sendTime() { Events::position(dec_.posMs() / 1000, dec_.durMs() / 1000); }

// ХУК M3c: тут буде перехід на наступний трек / repeat. Зараз порожній (state вже stop).
void SdSource::onTrackEnd() {}

// Відкриває файл і запускає сесію. Викликається з app, коли сесії декодера немає або вона зупиняється тут.
Res SdSource::startTrack(uint16_t n) {
  if (!active_ || !media_.mounted()) return Res::NoMedia;
  if (n >= pl_.tracks()) return Res::BadArg;
  char path[kScanPathMax + kMediaNameMax + 8];
  if (!pl_.trackPath(n, path, sizeof path)) return Res::BadArg;
  if (rdOpen_) {  // DBG,sdread не має ділити читач із відтворенням
    media_.reader().close();
    rdOpen_ = false;
    if (job_ == Dbg::Read) job_ = Dbg::None;
  }
  endSession();  // Play→Play: мʼют, зупинка, закриття; з Stop/Pause — просто закриття
  if (!media_.reader().open(path)) {
    st_ = PlayState::Stop;
    LOGW(TAG, "play: не відкрито #%u", (unsigned)n + 1);
    return media_.mounted() ? Res::ReadFail : Res::NoMedia;
  }
  if (n != cur_) {
    cur_ = n;
    emitTrackFolder();
  }
  LOGI(TAG, "play #%u %s (%u Б)", (unsigned)n + 1, path, (unsigned)media_.reader().size());
  dec_.startSession();
  st_ = PlayState::Play;
  nextTimeAt_ = millis() + kTimeEventMs;
  infoSeq_ = dec_.infoSeq();
  // state==play ⇒ DAC має звучати. Менеджер розмʼючує при зміні STATE, але для Play→Play STATE не міняється.
  g_audio.unmute();
  return Res::Ok;
}

Res SdSource::play() {
  if (st_ == PlayState::Scan) return Res::BadState;
  if (st_ == PlayState::Idle) return media_.mounted() ? Res::BadState : Res::NoMedia;
  if (st_ == PlayState::Play) return Res::Ok;
  if (st_ == PlayState::Pause) {  // позиція збережена: файл відкрито, задача чекає
    dec_.resume();                // розмʼют DAC зробить менеджер після повернення (задача його дочекається)
    st_ = PlayState::Play;
    nextTimeAt_ = millis() + kTimeEventMs;
    return Res::Ok;
  }
  return startTrack(cur_);        // Stop: поточний трек (на старті — перший)
}

Res SdSource::pause() {
  if (st_ == PlayState::Pause) return Res::Ok;
  if (st_ != PlayState::Play) return Res::BadState;
  g_audio.mute();  // спершу тиша, потім зупинка продюсера
  bool ok = dec_.pause();
  st_ = PlayState::Pause;
  if (ok) sendTime();  // один раз при паузі (якщо сесія вже скінчилась — подію кінця обробить pollPlayer)
  return Res::Ok;
}

Res SdSource::stop() {
  if (st_ == PlayState::Scan) return Res::BadState;
  if (st_ == PlayState::Play || st_ == PlayState::Pause) {
    endSession();
    st_ = PlayState::Stop;
  }
  return Res::Ok;
}

// Події задачі mp3 (кінець треку/помилка) і TIME раз на kTimeEventMs.
void SdSource::pollPlayer(uint32_t now) {
  Mp3Player::Ev ev = dec_.takeEvent();
  if (ev != Mp3Player::Ev::None) {
    if (media_.reader().isOpen()) media_.reader().close();
    st_ = PlayState::Stop;  // менеджер у publish() замʼютить DAC і надішле STATE
    switch (ev) {
      case Mp3Player::Ev::Eof:
        LOGI(TAG, "кінець треку #%u", (unsigned)cur_ + 1);
        onTrackEnd();
        break;
      case Mp3Player::Ev::ReadFail:
        LOGW(TAG, "помилка читання під час відтворення");
        Events::err(proto::errc::kReadFail, "play");
        break;
      default:
        LOGW(TAG, "помилка декодування");
        Events::err(proto::errc::kDecodeFail, "play");
        break;
    }
    return;
  }
  if (st_ != PlayState::Play) return;
  uint32_t seq = dec_.infoSeq();
  if (seq != infoSeq_ || due(now, nextTimeAt_)) {  // одразу, коли зʼявилась тривалість, далі раз на секунду
    infoSeq_ = seq;
    nextTimeAt_ = now + kTimeEventMs;
    sendTime();
  }
}

// ============================================================ DBG

bool SdSource::dbg(Dbg k, int arg) {
  bool ok = false;
  portENTER_CRITICAL(&s_mux);
  if (pend_ == Dbg::None && job_ == Dbg::None) {
    pend_ = k;
    pendArg_ = arg;
    ok = true;
  }
  portEXIT_CRITICAL(&s_mux);
  return ok;
}

void SdSource::runDbg() {
  if (job_ == Dbg::None) {
    Dbg k;
    int a;
    portENTER_CRITICAL(&s_mux);
    k = pend_;
    a = pendArg_;
    pend_ = Dbg::None;
    portEXIT_CRITICAL(&s_mux);
    if (k == Dbg::None) return;
    job_ = k;
    arg_ = a;
    listPos_ = 0;
    listEnd_ = 0;
    rdOpen_ = false;
  }
  switch (job_) {
    case Dbg::Info: dbgInfo(); job_ = Dbg::None; break;
    case Dbg::Scan: dbgScan(); job_ = Dbg::None; break;
    case Dbg::List: dbgList(); break;
    case Dbg::Read: dbgRead(); break;
    case Dbg::Play: dbgPlay(); job_ = Dbg::None; break;
    case Dbg::Decode: dbgDecode(); job_ = Dbg::None; break;
    default: job_ = Dbg::None; break;
  }
}

void SdSource::dbgInfo() {
  MediaInfo mi;
  media_.info(mi);
  if (mi.cdPin >= 0) LOGI(TAG, "sd: присутність=%d змонтовано=%d CD GPIO%d raw=%d", mi.present, mi.mounted, mi.cdPin, mi.cdRaw);
  else LOGI(TAG, "sd: присутність=%d змонтовано=%d (опитування %u мс)", mi.present, mi.mounted, (unsigned)kSdPollMs);
  LOGI(TAG, "sd: SPI=%u МГц тип=%s FS=%s обсяг=%u МБ помилок підряд=%u всього=%u", (unsigned)mi.mhz, mi.cardType, mi.fs,
       (unsigned)mi.sizeMb, (unsigned)mi.errStreak, (unsigned)mi.errTotal);
  LOGI(TAG, "sd: джерело active=%d state=%s плейлист=%u треків, арена %u/%u Б", active_, stateName(st_),
       (unsigned)pl_.tracks(), (unsigned)pl_.used(), (unsigned)Playlist::capacity());
}

void SdSource::dbgScan() {
  if (!scanned_) { LOGI(TAG, "скану ще не було (SRC,sd)"); return; }
  const ScanStats& s = scanner_.stats();
  LOGI(TAG, "scan[%s]: %u мс (робота %u мс) треків=%u папок=%u (з треками %u) записів=%u", scanStName(scanner_.state()),
       (unsigned)s.wallMs, (unsigned)s.busyMs, (unsigned)s.tracks, (unsigned)s.foldersTotal, (unsigned)s.foldersWithTracks,
       (unsigned)s.entries);
  LOGI(TAG, "scan: відкинуто: приховані=%u довгі=%u глибоко=%u каталоги_err=%u не_влізло=%u%s", (unsigned)s.hidden,
       (unsigned)s.longName, (unsigned)s.deepDirs, (unsigned)s.dirErr, (unsigned)s.dropped,
       s.overflow ? " (ПЕРЕПОВНЕННЯ, скан зупинено)" : "");
}

void SdSource::dbgList() {
  if (scanner_.state() == Scanner::St::Running) { LOGW(TAG, "list: триває скан"); job_ = Dbg::None; return; }
  uint16_t total = pl_.tracks();
  if (listPos_ == 0 && listEnd_ == 0) {  // ініціалізація
    listEnd_ = (arg_ <= 0 || arg_ > total) ? total : (uint16_t)arg_;
    LOGI(TAG, "list: треків=%u папок=%u арена=%u/%u, показано %u", (unsigned)total, (unsigned)pl_.folders(),
         (unsigned)pl_.used(), (unsigned)Playlist::capacity(), (unsigned)listEnd_);
    if (!listEnd_) { job_ = Dbg::None; return; }
  }
  char path[kScanPathMax];
  char name[kMediaNameMax];
  for (int k = 0; k < kListPerSlice && listPos_ < listEnd_; k++, listPos_++) {
    int f = pl_.folderOf(listPos_);
    if (f < 0 || !pl_.folderPath(f, path, sizeof path) || !pl_.trackName(listPos_, name, sizeof name)) {
      LOGW(TAG, "list: %u — помилка запису", (unsigned)listPos_ + 1);
      continue;
    }
    LOGI(TAG, "%4u [%s] %s", (unsigned)listPos_ + 1, path, name);
  }
  if (listPos_ >= listEnd_) job_ = Dbg::None;
}

void SdSource::dbgRead() {
  IFileReader& rd = media_.reader();
  if (!rdOpen_) {
    if (scanner_.state() == Scanner::St::Running) { LOGW(TAG, "sdread: триває скан"); job_ = Dbg::None; return; }
    if (!media_.mounted()) { LOGW(TAG, "sdread: носія немає"); job_ = Dbg::None; return; }
    if (!active_ || st_ != PlayState::Stop) {  // буфер читання — вхідний буфер PcmArena, власник Sd
      LOGW(TAG, "sdread: потрібен src=sd у стані stop (область PCM належить SD, сесії немає)");
      job_ = Dbg::None;
      return;
    }
    if (arg_ < 1 || arg_ > (int)pl_.tracks()) {
      LOGW(TAG, "sdread: номер 1..%u (спершу SRC,sd для скану)", (unsigned)pl_.tracks());
      job_ = Dbg::None;
      return;
    }
    char path[kScanPathMax + kMediaNameMax + 8];
    if (!pl_.trackPath((uint16_t)(arg_ - 1), path, sizeof path) || !rd.open(path)) {
      LOGW(TAG, "sdread: не відкрито #%d", arg_);
      job_ = Dbg::None;
      return;
    }
    LOGI(TAG, "sdread #%d %s (%u Б)", arg_, path, (unsigned)rd.size());
    rdOpen_ = true;
    rdBytes_ = 0;
    rdBusyUs_ = 0;
    rdT0_ = millis();
  }
  size_t cap = 0;
  uint8_t* buf = dec_.scratch(cap);
  uint32_t t = millis();
  do {
    uint32_t a = micros();
    size_t n = rd.read(buf, cap);
    rdBusyUs_ += micros() - a;
    if (n == 0) {
      uint32_t wall = millis() - rdT0_;
      uint32_t kbs = rdBusyUs_ ? (uint32_t)((uint64_t)rdBytes_ * 1000000ULL / 1024 / rdBusyUs_) : 0;
      uint32_t kbsWall = wall ? (uint32_t)((uint64_t)rdBytes_ * 1000ULL / 1024 / wall) : 0;
      LOGI(TAG, "sdread: %u Б%s, у читанні %u мс → %u КБ/с; з паузами порцій %u мс → %u КБ/с", (unsigned)rdBytes_,
           rd.failed() ? " (ПОМИЛКА)" : "", (unsigned)(rdBusyUs_ / 1000), (unsigned)kbs, (unsigned)wall, (unsigned)kbsWall);
      rd.close();
      rdOpen_ = false;
      job_ = Dbg::None;
      return;
    }
    rdBytes_ += n;
  } while ((uint32_t)(millis() - t) < kReadSliceMs);
}


// DBG,play,<n>: відтворити трек n із DBG,list (з будь-якого стану SD, окрім скану/idle).
void SdSource::dbgPlay() {
  if (!active_ || st_ == PlayState::Scan || st_ == PlayState::Idle) {
    LOGW(TAG, "play: потрібен src=sd зі скануванням, що завершилось (state=%s)", stateName(st_));
    return;
  }
  if (arg_ < 1 || arg_ > (int)pl_.tracks()) {
    LOGW(TAG, "play: номер 1..%u", (unsigned)pl_.tracks());
    return;
  }
  Res r = startTrack((uint16_t)(arg_ - 1));
  if (r != Res::Ok) Events::err(resCode(r), "dbgplay");
}

void SdSource::dbgDecode() {
  Mp3Stats s;
  dec_.stats(s);
  uint32_t posS = dec_.posMs() / 1000, durS = dec_.durMs() / 1000;
  LOGI(TAG, "decode: state=%s сесія=%d поз=%u с трив=%u с%s | %u Гц %u кан %u кбіт/с", stateName(st_),
       (int)dec_.state(), (unsigned)posS, (unsigned)durS, dec_.durEstimated() ? " (оцінка)" : "", (unsigned)s.hz,
       (unsigned)s.ch, (unsigned)s.kbps);
  LOGI(TAG, "decode: кадрів=%u пропущено=%u ресинхр.=%u | underrun=%u overrun=н/д (кільця немає) write_timeout=%u paced=%u",
       (unsigned)s.frames, (unsigned)s.skipped, (unsigned)s.resyncs, (unsigned)s.underruns, (unsigned)s.writeTimeouts,
       (unsigned)s.paced);
  LOGI(TAG, "decode: час кадру сер.=%u макс.=%u мкс; тривалість кадру сер.=%u мкс; навантаження сер.=%u.%u%% макс.=%u.%u%%",
       (unsigned)s.decAvgUs, (unsigned)s.decMaxUs, (unsigned)s.frameAvgUs, (unsigned)(s.loadAvgPm / 10),
       (unsigned)(s.loadAvgPm % 10), (unsigned)(s.loadMaxPm / 10), (unsigned)(s.loadMaxPm % 10));
  uint32_t kbs = s.rdBusyUs ? (uint32_t)((uint64_t)s.rdBytes * 1000000ULL / 1024 / s.rdBusyUs) : 0;
  LOGI(TAG, "decode: вхід %u/%u Б, PCM-кадр %u Б; читання %u КБ, %u КБ/с (у читанні), макс. виклик %u мкс",
       (unsigned)s.inFill, (unsigned)s.inCap, (unsigned)s.pcmBytes, (unsigned)(s.rdBytes / 1024), (unsigned)kbs,
       (unsigned)s.rdMaxUs);
  LOGI(TAG, "decode: область PCM: власник=%s, %u Б", PcmArena::ownerName(PcmArena::owner()), (unsigned)kPcmArenaBytes);
  Diag::logHeap("decode");
}

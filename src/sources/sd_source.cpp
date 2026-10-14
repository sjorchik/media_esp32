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

constexpr uint16_t kNoTag = 0xFFFF;

// FNV-1a по трьох полях (роздільник 0x1F) — компактний відбиток надісланого META
uint32_t metaHash(const char* a, const char* t, const char* al) {
  uint32_t h = 2166136261u;
  const char* f[3] = {a, t, al};
  for (int k = 0; k < 3; k++) {
    for (const uint8_t* p = (const uint8_t*)f[k]; *p; p++) h = (h ^ *p) * 16777619u;
    h = (h ^ 0x1Fu) * 16777619u;
  }
  return h;
}

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
  errRun_ = 0;
  tagTrack_ = kNoTag;  // плейлист міг змінитись
}

void SdSource::endSession() {
  if (dec_.sessionActive()) {
    g_audio.mute();  // тиша до зупинки продюсера
    dec_.stopSession();
  }
  dec_.takeEvent();
  takeTags(false);  // розбір міг завершитись між опитуваннями; cur_ ще відповідає сесії
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
  tagTrack_ = kNoTag;
  errRun_ = 0;
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
  emitTrackInfo();  // TRACK, FOLDER, META, TIME; далі STATE,sd,stop від менеджера
  if (r == Scanner::St::Overflow) Events::err(kErrTooMany);
}

// ============================================================ події треку (M3c)

void SdSource::sendTime() {
  SourceStatus s = status();  // у stop/idle → 0, 0
  Events::position(s.posS, s.durS);
}

void SdSource::emitFolder() {
  int f = pl_.folderOf(cur_);
  char nm[kEvFolderNameMax + 1] = {0};
  if (f >= 0) {
    char full[kMediaNameMax];
    if (pl_.folderName(f, full, sizeof full)) Utf8::copySafe(nm, sizeof nm, full);
  }
  Events::folder(nm, f >= 0 ? pl_.folderTracks(f) : 0);
}

// Найкраще відоме: теги, якщо вони розібрані саме для cur_; порожня назва → імʼя файлу без .mp3.
void SdSource::buildMeta(char* a, char* t, char* al) {
  a[0] = t[0] = al[0] = 0;
  if (tagTrack_ == cur_) {
    const Id3::Tags& g = dec_.tags();
    memcpy(a, g.artist, kEvMetaFieldMax + 1);
    memcpy(t, g.title, kEvMetaFieldMax + 1);
    memcpy(al, g.album, kEvMetaFieldMax + 1);
    a[kEvMetaFieldMax] = t[kEvMetaFieldMax] = al[kEvMetaFieldMax] = 0;
  }
  if (!t[0]) {
    char full[kMediaNameMax];
    if (pl_.trackName(cur_, full, sizeof full)) Utf8::copySafe(t, kEvMetaFieldMax + 1, full);
  }
}

// META одразу з відомим (при зміні треку — імʼя файлу; GET/HELLO — уже розібрані теги)
void SdSource::emitMeta() {
  char a[kEvMetaFieldMax + 1], t[kEvMetaFieldMax + 1], al[kEvMetaFieldMax + 1];
  buildMeta(a, t, al);
  shownHash_ = metaHash(a, t, al);
  Events::meta(a, t, al);
}

void SdSource::pushMeta() {
  char a[kEvMetaFieldMax + 1], t[kEvMetaFieldMax + 1], al[kEvMetaFieldMax + 1];
  buildMeta(a, t, al);
  uint32_t h = metaHash(a, t, al);
  if (h == shownHash_) return;  // нічого нового
  shownHash_ = h;
  Events::meta(a, t, al);
}

// Результат розбору тегів сесії (задача mp3 підняла tagSeq): привʼязуємо до cur_ і, якщо треба, шлемо повторний META.
void SdSource::takeTags(bool emit) {
  uint32_t seq = dec_.tagSeq();
  if (seq == tagSeqSeen_) return;
  tagSeqSeen_ = seq;
  tagTrack_ = cur_;
  LOGI(TAG, "теги #%u: v1=%d v2=%u за %u мкс", (unsigned)cur_ + 1, (dec_.tagMask() & Id3::kFoundV1) ? 1 : 0,
       (unsigned)dec_.tagVer(), (unsigned)dec_.tagUs());
  if (emit) pushMeta();
}

// Порядок за контрактом: TRACK, FOLDER, META, TIME (STATE додасть менеджер, якщо стан змінився)
void SdSource::emitTrackInfo() {
  Events::track(cur_ + 1, pl_.tracks());
  emitFolder();
  emitMeta();
  sendTime();
}

void SdSource::emitInfo() {
  if (!active_ || !pl_.tracks()) return;
  if (st_ == PlayState::Stop || st_ == PlayState::Play || st_ == PlayState::Pause) {
    takeTags(false);  // віддаємо вже розібрані теги
    emitTrackInfo();
  }
}

// Нова поточна позиція без запуску. Зміна треку → події; той самий трек → лише TIME (позиція скинута).
void SdSource::setCur(uint16_t n) {
  bool changed = n != cur_;
  cur_ = n;
  if (changed) emitTrackInfo();
  else sendTime();
}

// ============================================================ навігація й відтворення (M3b/M3c)

// Загальна передумова команд відтворення/навігації: scan → bad_state; idle → no_media (нема носія або mp3), інакше bad_state.
Res SdSource::ready() const {
  if (st_ == PlayState::Scan) return Res::BadState;
  if (st_ == PlayState::Idle) return (!media_.mounted() || !pl_.tracks()) ? Res::NoMedia : Res::BadState;
  return Res::Ok;
}

uint16_t SdSource::nextIdx(uint16_t i) const { return (uint16_t)(i + 1 >= pl_.tracks() ? 0 : i + 1); }
uint16_t SdSource::prevIdx(uint16_t i) const { return (uint16_t)(i == 0 ? pl_.tracks() - 1 : i - 1); }

void SdSource::note(char why, uint16_t to) {
  hist_[histHead_] = Hist{cur_, to, why};
  histHead_ = (uint8_t)((histHead_ + 1) % kNavHist);
  if (histCnt_ < kNavHist) histCnt_++;
  LOGI(TAG, "перехід '%c': #%u -> #%u", why, (unsigned)cur_ + 1, (unsigned)to + 1);
}

// NEXT/PREV/FNEXT/FPREV: циклічно; з stop/pause одразу починають відтворення. Ручна команда скидає лічильник помилок.
Res SdSource::nav(Nav k) {
  Res r = ready();
  if (r != Res::Ok) return r;
  const uint16_t total = pl_.tracks();
  uint16_t to = cur_;
  char why = 'N';
  switch (k) {
    case Nav::Next:
      to = nextIdx(cur_);
      break;
    case Nav::Prev:
      if ((st_ == PlayState::Play || st_ == PlayState::Pause) && dec_.posMs() > kPrevRestartMs) {
        why = 'R';  // перезапуск поточного: файл відкривається знову, скан не повторюється
      } else {
        to = prevIdx(cur_);
        why = 'P';
      }
      break;
    case Nav::FNext:
    case Nav::FPrev: {
      int f = pl_.folderOf(cur_);
      if (f < 0) return Res::BadState;
      uint32_t first = pl_.folderFirst(f);
      if (k == Nav::FNext) {  // папки — суцільні діапазони: наступна починається одразу за кінцем поточної
        uint32_t end = first + pl_.folderTracks(f);
        to = end >= total ? 0 : (uint16_t)end;
        why = 'F';
      } else {                // перша доріжка папки, що містить трек перед першим треком поточної
        int pf = pl_.folderOf((uint16_t)(first ? first - 1 : total - 1));
        if (pf < 0) return Res::BadState;
        to = pl_.folderFirst(pf);
        why = 'B';
      }
      break;
    }
  }
  errRun_ = 0;
  return playTrack(to, why);
}


// Відкриває файл і запускає сесію. Викликається з app. Нова позиція → події треку (до відкриття файлу).
// ReadFail = файл не відкрито (рішення про пропуск приймає playTrack).
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
  endSession();  // Play→Play: мʼют, зупинка, закриття; з Stop/Pause/Eof/помилки — просто закриття
  g_audio.mute();  // ідемпотентно: сесія завжди стартує з замʼючено DAC (задача mp3 чекає розмʼюту), як у перевірених сценаріях
  st_ = PlayState::Stop;  // status() = 0/0, доки сесія не стартувала (відкриття може не вдатись)
  if (n != cur_) {
    cur_ = n;
    emitTrackInfo();  // TRACK, FOLDER, META, TIME; STATE додасть менеджер
  }
  if (!media_.reader().open(path)) {
    LOGW(TAG, "play: не відкрито #%u", (unsigned)n + 1);
    return media_.mounted() ? Res::ReadFail : Res::NoMedia;
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

// Запуск треку з автопропуском: файл не відкрито → ERR,read_fail і наступний трек; kPlayErrStop підряд → stop.
Res SdSource::playTrack(uint16_t n, char why) {
  for (;;) {
    note(why, n);
    Res r = startTrack(n);
    if (r != Res::ReadFail) return r;  // Ok / NoMedia / BadArg — нагору
    if (!countFail(r)) return Res::Ok;  // ліміт: state=stop, ERR уже надіслано
    n = nextIdx(n);
    why = 'x';
  }
}

// ERR + лічильник підряд-помилок. false = ліміт досягнуто → stop (STATE надішле менеджер).
bool SdSource::countFail(Res r) {
  Events::err(resCode(r), "play");
  errRun_++;
  if (errRun_ < kPlayErrStop) return true;
  LOGW(TAG, "%u помилок підряд -> stop на #%u", (unsigned)errRun_, (unsigned)cur_ + 1);
  st_ = PlayState::Stop;
  return false;
}

// Помилка під час гри (сесія вже завершена, state=stop): ERR і перехід до наступного за списком (REP не враховується).
void SdSource::onPlayError(Res r) {
  if (countFail(r)) playTrack(nextIdx(cur_), 'x');
}

// Природний кінець треку (state вже stop, файл закрито): REP 0/1/2.
void SdSource::onTrackEnd() {
  uint16_t total = pl_.tracks();
  if (!total) return;
  if (rep_ == 1) {  // повтор поточного
    playTrack(cur_, 'r');
  } else if (rep_ == 2 || cur_ + 1 < total) {  // цикл списку / REP=0, не останній
    playTrack(nextIdx(cur_), 'e');
  } else {  // REP=0, кінець списку: stop, поточний = перший (PLAY почне спочатку)
    note('z', 0);
    st_ = PlayState::Stop;
    setCur(0);
  }
}

Res SdSource::play() {
  Res r = ready();
  if (r != Res::Ok) return r;
  if (st_ == PlayState::Play) return Res::Ok;
  if (st_ == PlayState::Pause) {  // позиція збережена: файл відкрито, задача чекає
    dec_.resume();                // розмʼют DAC зробить менеджер після повернення (задача його дочекається)
    st_ = PlayState::Play;
    nextTimeAt_ = millis() + kTimeEventMs;
    return Res::Ok;
  }
  errRun_ = 0;
  return playTrack(cur_, 'p');    // Stop: поточний трек (на старті / після кінця списку — перший)
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
  errRun_ = 0;
  if (st_ == PlayState::Play || st_ == PlayState::Pause) {
    endSession();
    st_ = PlayState::Stop;
    pushMeta();  // теги, розібрані до зупинки, але ще не надіслані
  }
  return Res::Ok;
}

// Події задачі mp3 (кінець треку/помилка) і TIME раз на kTimeEventMs.
void SdSource::pollPlayer(uint32_t now) {
  takeTags(true);  // теги нового треку → повторний META (лише якщо змінився)
  if (st_ == PlayState::Play && errRun_ && dec_.posMs() >= kPlayOkMs) errRun_ = 0;  // новий трек відіграв ≥ 1 с
  Mp3Player::Ev ev = dec_.takeEvent();
  if (ev != Mp3Player::Ev::None) {
    if (media_.reader().isOpen()) media_.reader().close();
    st_ = PlayState::Stop;  // якщо далі не стартує інший трек, менеджер замʼютить DAC і надішле STATE
    switch (ev) {
      case Mp3Player::Ev::Eof:
        LOGI(TAG, "кінець треку #%u", (unsigned)cur_ + 1);
        onTrackEnd();
        break;
      case Mp3Player::Ev::ReadFail:
        LOGW(TAG, "помилка читання під час відтворення #%u", (unsigned)cur_ + 1);
        onPlayError(Res::ReadFail);
        break;
      default:
        LOGW(TAG, "помилка декодування #%u", (unsigned)cur_ + 1);
        onPlayError(Res::DecodeFail);
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
    case Dbg::Nav: dbgNav(); job_ = Dbg::None; break;
    case Dbg::Tag: dbgTag(); job_ = Dbg::None; break;
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
  errRun_ = 0;
  Res r = playTrack((uint16_t)(arg_ - 1), 'd');
  if (r != Res::Ok) Events::err(resCode(r), "dbgplay");
}

// DBG,nav: стан навігації та історія переходів (лише лог).
void SdSource::dbgNav() {
  uint16_t total = pl_.tracks();
  LOGI(TAG, "nav: state=%s rep=%u shuf=%d помилок підряд=%u (ліміт %u)", stateName(st_), (unsigned)rep_, shuf_ ? 1 : 0,
       (unsigned)errRun_, (unsigned)kPlayErrStop);
  if (!total) {
    LOGI(TAG, "nav: плейлист порожній");
  } else {
    int f = pl_.folderOf(cur_);
    unsigned first = pl_.folderFirst(f), cnt = pl_.folderTracks(f);
    LOGI(TAG, "nav: поточний #%u/%u, папка %d діапазон %u..%u (%u тр.)", (unsigned)cur_ + 1, (unsigned)total, f, first + 1,
         first + cnt, cnt);
  }
  char buf[kNavHist * 14 + 8];
  size_t w = 0;
  buf[0] = 0;
  for (uint8_t k = 0; k < histCnt_; k++) {  // від найстарішого до найновішого
    const Hist& h = hist_[(histHead_ + kNavHist - histCnt_ + k) % kNavHist];
    int n = snprintf(buf + w, sizeof buf - w, " %c%u>%u", h.why, (unsigned)h.from + 1, (unsigned)h.to + 1);
    if (n < 0 || (size_t)n >= sizeof buf - w) break;
    w += (size_t)n;
  }
  LOGI(TAG, "nav: історія(%u):%s", (unsigned)histCnt_, histCnt_ ? buf : " порожньо");
  LOGI(TAG, "nav: причини: N next, P prev, R перезапуск, F fnext, B fprev, p play");
  LOGI(TAG, "nav: причини: e кінець->наступний, r repeat-1, z кінець списку, x пропуск помилки, d DBG,play");
}

// DBG,tag,<n>: розібрати теги треку n без відтворення (src=sd у stop; скретч — вхідний буфер області PCM).
void SdSource::dbgTag() {
  if (!active_ || st_ != PlayState::Stop || rdOpen_ || dec_.sessionActive()) {
    LOGW(TAG, "tag: потрібен src=sd у стані stop (state=%s)", stateName(st_));
    return;
  }
  if (arg_ < 1 || arg_ > (int)pl_.tracks()) {
    LOGW(TAG, "tag: номер 1..%u", (unsigned)pl_.tracks());
    return;
  }
  char path[kScanPathMax + kMediaNameMax + 8];
  IFileReader& rd = media_.reader();
  if (!pl_.trackPath((uint16_t)(arg_ - 1), path, sizeof path) || !rd.open(path)) {
    LOGW(TAG, "tag: не відкрито #%d", arg_);
    return;
  }
  size_t cap = 0;
  uint8_t* buf = dec_.scratch(cap);
  Id3::Tags t;
  Id3::Info inf;
  uint32_t t0 = micros();
  uint8_t mask = Id3::parse(rd, buf, cap, t, &inf);
  uint32_t us = micros() - t0;
  bool failed = rd.failed();
  rd.close();
  LOGI(TAG, "tag #%d: v2=%u v1=%d, тег %u Б, кадрів %u, прочитано %u Б%s%s, розбір %u мкс", arg_, (unsigned)inf.ver,
       (mask & Id3::kFoundV1) ? 1 : 0, (unsigned)inf.tagBytes, (unsigned)inf.frames, (unsigned)inf.readBytes,
       inf.cut ? " (УРІЗАНО лімітом)" : "", failed ? " (ПОМИЛКА читання)" : "", (unsigned)us);
  static const char* const kName[3] = {"artist", "title", "album"};
  const char* val[3] = {t.artist, t.title, t.album};
  for (int i = 0; i < 3; i++) {
    const Id3::FieldInfo& f = inf.f[i];
    char hex[kId3DbgRaw * 3 + 1];
    size_t w = 0;
    for (uint8_t k = 0; k < f.rawN; k++) w += (size_t)snprintf(hex + w, sizeof hex - w, "%02X ", (unsigned)f.raw[k]);
    hex[w] = 0;
    LOGI(TAG, "tag: %-6s src=v%u enc=%u raw=[%s] -> '%s'", kName[i], (unsigned)f.src, (unsigned)f.enc, hex, val[i]);
  }
  Diag::logHeap("tag");
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

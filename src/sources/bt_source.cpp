#include "sources/bt_source.h"

#include <Arduino.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <vector>

#include "BluetoothA2DPSink.h"
#include "esp_a2dp_api.h"
#include "esp_gap_bt_api.h"

#include "audio/audio_task.h"
#include "audio/pcm_arena.h"
#include "audio/pcm_ring.h"
#include "core/device_state.h"
#include "core/events.h"
#include "storage/nvs_store.h"
#include "util/diag.h"
#include "util/log.h"
#include "util/utf8.h"

namespace {

constexpr const char* TAG = "bt";
constexpr const char* kKeyPeer = "bt_peer";   // BD-адреса: 12 hex-символів
constexpr const char* kKeyName = "bt_name";
constexpr uint8_t kAvrcUnknown = 0xFE;
constexpr uint8_t kRsnNone = 0xFF;            // disc_rsn невідома
constexpr size_t kNameScanMax = 249;          // макс. довжина BT-імені + 1
constexpr int32_t kPosDeadZoneMs = 1500;      // розбіжність позиції, яку не коригуємо

// Кільце живе в спільній області PcmArena (перші kBtRingBytes); власник Bt лише поки BT активне.
PcmRing s_ring(g_pcmArena, kBtRingBytes);
volatile uint8_t s_cbBusy = 0;  // 1, поки колбек A2DP пише в кільце (для безпечної передачі області SD)
BluetoothA2DPSink* s_a2dp = nullptr;

// Діагностика затримки реакції телефона на команди AVRCP
uint32_t s_cmdAt = 0;
const char* s_cmdName = nullptr;
void noteCmd(const char* name) {
  s_cmdAt = millis();
  s_cmdName = name;
  LOGI(TAG, "AVRCP %s надіслано", name);
}

// ---- Inbox: колбеки бібліотеки (інші задачі) → задача app ----
portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
struct Inbox {
  bool conn = false, audio = false, avrc = false, pos = false, meta = false, rate = false;
  esp_a2d_connection_state_t connSt = ESP_A2D_CONNECTION_STATE_DISCONNECTED;
  esp_a2d_audio_state_t audioSt = ESP_A2D_AUDIO_STATE_STOPPED;
  uint8_t avrcSt = kAvrcUnknown;
  uint32_t posMs = 0, metaAtMs = 0, lastAttrMs = 0, durMs = 0, connAtMs = 0;
  uint16_t rateHz = 0;
  char title[kBtMetaMax + 1] = {0}, artist[kBtMetaMax + 1] = {0}, album[kBtMetaMax + 1] = {0};
} s_in;

struct Snap {
  bool conn, audio, avrc, pos, meta, rate;
  esp_a2d_connection_state_t connSt;
  esp_a2d_audio_state_t audioSt;
  uint8_t avrcSt;
  uint32_t posMs, metaAtMs, durMs, connAtMs;
  uint16_t rateHz;
  char title[kBtMetaMax + 1], artist[kBtMetaMax + 1], album[kBtMetaMax + 1];
};

void takeInbox(Snap& s) {
  portENTER_CRITICAL(&s_mux);
  s.conn = s_in.conn; s.connSt = s_in.connSt; s.connAtMs = s_in.connAtMs; s_in.conn = false;
  s.audio = s_in.audio; s.audioSt = s_in.audioSt; s_in.audio = false;
  s.avrc = s_in.avrc; s.avrcSt = s_in.avrcSt; s_in.avrc = false;
  s.pos = s_in.pos; s.posMs = s_in.posMs; s_in.pos = false;
  s.rate = s_in.rate; s.rateHz = s_in.rateHz; s_in.rate = false;
  s.meta = s_in.meta; s.metaAtMs = s_in.metaAtMs; s.durMs = s_in.durMs;
  if (s_in.meta) {
    memcpy(s.title, s_in.title, sizeof s.title);
    memcpy(s.artist, s_in.artist, sizeof s.artist);
    memcpy(s.album, s_in.album, sizeof s.album);
    s_in.meta = false;
  }
  portEXIT_CRITICAL(&s_mux);
}

// ---- підклас sink: причина розриву (disc_rsn) і політика scan mode ----
volatile bool s_blockScan = false;       // true: BT вимкнений — будь-яка спроба бібліотеки зробити connectable блокується
volatile bool s_wantDisc = false;        // бажана видимість, коли connectable
volatile uint8_t s_scanMode = 3;         // 0 non-conn, 1 connectable прихований, 2 connectable+discoverable, 3 невідомо
uint8_t s_rsn = kRsnNone;                // disc_rsn останнього A2DP DISCONNECTED (під s_mux)
uint32_t s_rsnAt = 0;                    // millis() цієї події

class A2dpSink : public BluetoothA2DPSink {
 protected:
  // Контекст BtAppT: лише фіксуємо причину, далі штатна логіка бібліотеки.
  void handle_connection_state(uint16_t event, void* p) override {
    esp_a2d_cb_param_t* a = (esp_a2d_cb_param_t*)p;
    if (a && a->conn_stat.state == ESP_A2D_CONNECTION_STATE_DISCONNECTED) {
      portENTER_CRITICAL(&s_mux);
      s_rsn = (uint8_t)a->conn_stat.disc_rsn;
      s_rsnAt = millis();
      portEXIT_CRITICAL(&s_mux);
    }
    BluetoothA2DPSink::handle_connection_state(event, p);
  }
  // Бібліотека викликає це після розриву/старту (з discoverable за замовчуванням): тут керуємо політикою.
  void set_scan_mode_connectable(bool c) override {
    if (s_blockScan || !c) {
      esp_bt_gap_set_scan_mode(ESP_BT_NON_CONNECTABLE, ESP_BT_NON_DISCOVERABLE);
      s_scanMode = 0;
    } else {
      bool d = s_wantDisc;
      esp_bt_gap_set_scan_mode(ESP_BT_CONNECTABLE, d ? ESP_BT_GENERAL_DISCOVERABLE : ESP_BT_NON_DISCOVERABLE);
      s_scanMode = d ? 2 : 1;
    }
  }
};

void cbNop(const uint8_t*, uint32_t) {}  // stream_reader не потрібен: PCM беремо з raw_stream_reader

// ---- колбеки бібліотеки (контекст BT-задач: коротко, без логів і блокувань) ----
void cbPcm(const uint8_t* d, uint32_t n) {
  s_ring.inBytes += n;
  __atomic_store_n(&s_cbBusy, (uint8_t)1, __ATOMIC_SEQ_CST);  // спершу «зайнято», потім перевірка accepting
  if (!s_ring.accepting()) {
    s_ring.dropInactive += n;
    __atomic_store_n(&s_cbBusy, (uint8_t)0, __ATOMIC_RELEASE);
    return;
  }
  s_ring.write(d, n);
  AudioTask::wake();
  __atomic_store_n(&s_cbBusy, (uint8_t)0, __ATOMIC_RELEASE);
}

// Після setAccepting(false): чекаємо завершення колбека, що вже встиг увійти (≤ кілька мс),
// щоб він не дописав у область, яку забирає SD.
void quiesceRing() {
  __atomic_thread_fence(__ATOMIC_SEQ_CST);
  for (int i = 0; i < 20 && __atomic_load_n(&s_cbBusy, __ATOMIC_ACQUIRE); i++) vTaskDelay(1);
  if (__atomic_load_n(&s_cbBusy, __ATOMIC_ACQUIRE)) LOGW("bt", "колбек PCM не завершився за 20 мс");
}

void cbRate(uint16_t hz) {
  s_ring.setPendingRate(hz);
  portENTER_CRITICAL(&s_mux);
  s_in.rate = true;
  s_in.rateHz = hz;
  portEXIT_CRITICAL(&s_mux);
}

void cbConn(esp_a2d_connection_state_t st, void*) {
  portENTER_CRITICAL(&s_mux);
  s_in.conn = true;
  s_in.connSt = st;
  s_in.connAtMs = millis();
  portEXIT_CRITICAL(&s_mux);
}

void cbAudio(esp_a2d_audio_state_t st, void*) {
  portENTER_CRITICAL(&s_mux);
  s_in.audio = true;
  s_in.audioSt = st;
  portEXIT_CRITICAL(&s_mux);
}

void cbAvrc(esp_avrc_playback_stat_t st) {
  portENTER_CRITICAL(&s_mux);
  s_in.avrc = true;
  s_in.avrcSt = (uint8_t)st;
  portEXIT_CRITICAL(&s_mux);
}

void cbPos(uint32_t ms) {
  portENTER_CRITICAL(&s_mux);
  s_in.pos = true;
  s_in.posMs = ms;
  portEXIT_CRITICAL(&s_mux);
}

void cbMeta(uint8_t id, const uint8_t* text) {
  if (!text) return;
  uint32_t now = millis();
  const char* t = (const char*)text;
  portENTER_CRITICAL(&s_mux);
  if (now - s_in.lastAttrMs > 300) {  // перший атрибут нової відповіді: скинути залишки попереднього треку
    s_in.title[0] = s_in.artist[0] = s_in.album[0] = 0;
    s_in.durMs = 0;
  }
  s_in.lastAttrMs = now;
  switch (id) {
    case ESP_AVRC_MD_ATTR_TITLE: Utf8::copySafe(s_in.title, sizeof s_in.title, t); break;
    case ESP_AVRC_MD_ATTR_ARTIST: Utf8::copySafe(s_in.artist, sizeof s_in.artist, t); break;
    case ESP_AVRC_MD_ATTR_ALBUM: Utf8::copySafe(s_in.album, sizeof s_in.album, t); break;
    case ESP_AVRC_MD_ATTR_PLAYING_TIME: s_in.durMs = (uint32_t)strtoul(t, nullptr, 10); break;
    default: break;
  }
  s_in.meta = true;
  s_in.metaAtMs = now;
  portEXIT_CRITICAL(&s_mux);
}

// ---- утиліти ----
bool parseAddr(const char* s, uint8_t* out) {
  if (strlen(s) != 12) return false;
  for (int i = 0; i < 6; i++) {
    char b[3] = {s[2 * i], s[2 * i + 1], 0};
    char* e = nullptr;
    long v = strtol(b, &e, 16);
    if (*e) return false;
    out[i] = (uint8_t)v;
  }
  return true;
}

// Сувора перевірка імені (від бібліотеки або з NVS): завершений рядок до kNameScanMax байтів,
// лише валідний UTF-8 без керуючих символів. Придатне імʼя копіюється в out (з обрізанням по межі символу).
bool validName(const char* s, char* out, size_t cap) {
  if (!s || !cap) return false;
  size_t n = 0;
  while (n < kNameScanMax && s[n]) n++;
  if (n == 0 || n >= kNameScanMax) return false;  // порожнє або не завершене
  size_t i = 0;
  while (i < n) {
    uint8_t c = (uint8_t)s[i];
    size_t k;
    if (c < 0x20 || c == 0x7F) return false;
    if (c < 0x80) k = 1;
    else if (c >= 0xC2 && c < 0xE0) k = 2;
    else if (c >= 0xE0 && c < 0xF0) k = 3;
    else if (c >= 0xF0 && c < 0xF5) k = 4;
    else return false;
    if (i + k > n) return false;
    for (size_t j = 1; j < k; j++) {
      if (((uint8_t)s[i + j] & 0xC0) != 0x80) return false;
    }
    i += k;
  }
  Utf8::copySafe(out, cap, s);
  return out[0] != 0;
}

const char* avrcName(uint8_t s) {
  switch (s) {
    case ESP_AVRC_PLAYBACK_STOPPED: return "stopped";
    case ESP_AVRC_PLAYBACK_PLAYING: return "playing";
    case ESP_AVRC_PLAYBACK_PAUSED: return "paused";
    case ESP_AVRC_PLAYBACK_FWD_SEEK: return "fwd_seek";
    case ESP_AVRC_PLAYBACK_REV_SEEK: return "rev_seek";
    case ESP_AVRC_PLAYBACK_ERROR: return "error";
    default: return "unknown";
  }
}

inline bool due(uint32_t now, uint32_t t) { return (int32_t)(now - t) >= 0; }

}  // namespace

// ============================================================ життєвий цикл

bool BtSource::begin() {
  AudioTask::attachStream(&s_ring);
  char hex[16];
  if (NvsStore::getStr(kKeyPeer, hex, sizeof hex) && parseAddr(hex, peer_)) {
    havePeer_ = true;
    char raw[64];
    savedName_[0] = 0;
    if (NvsStore::getStr(kKeyName, raw, sizeof raw) && !validName(raw, savedName_, sizeof savedName_)) {
      savedName_[0] = 0;  // у NVS лишилось сміття від попередніх збірок
      NvsStore::putStr(kKeyName, "");
    }
    LOGI(TAG, "збережений пристрій: %s '%s'", hex, savedName_);
  } else {
    LOGI(TAG, "збереженого пристрою немає");
  }
  return true;
}

void BtSource::startStack() {
  if (started_) return;
  s_blockScan = true;                             // політика «BT вимкнений» діє вже з підйому стеку
  s_scanMode = 0;
  s_a2dp = new A2dpSink();
  BluetoothA2DPSink& a = *s_a2dp;
  // Усі виклики бібліотеки зібрані тут (при іншій версії API правити лише цей блок).
  a.set_raw_stream_reader(cbPcm);                 // PCM у наше кільце ДО програмної гучності бібліотеки
  a.set_stream_reader(cbNop, false);              // власного I2S немає
  a.set_sample_rate_callback(cbRate);
  a.set_avrc_metadata_attribute_mask((esp_avrc_md_attr_mask_t)(ESP_AVRC_MD_ATTR_TITLE | ESP_AVRC_MD_ATTR_ARTIST |
                                                                ESP_AVRC_MD_ATTR_ALBUM | ESP_AVRC_MD_ATTR_PLAYING_TIME));
  a.set_avrc_metadata_callback(cbMeta);
  a.set_avrc_rn_playstatus_callback(cbAvrc);
  a.set_avrc_rn_play_pos_callback(cbPos, kBtPosNotifS);
  a.set_on_connection_state_changed(cbConn);
  a.set_on_audio_state_changed(cbAudio);
  // Absolute volume вимкнено: порожній список подій TG (без ESP_AVRC_RN_VOLUME_CHANGE) → телефон
  // не реєструє volume-change і ослаблює потік програмно. Має бути до start() (читається при підйомі AVRCP TG).
  a.set_avrc_rn_events(std::vector<esp_avrc_rn_event_ids_t>());
  a.start(kBtDeviceName, false);                  // автореконект бібліотеки вимкнено: ним керуємо самі

  started_ = true;
  uint32_t now = millis();
  statAt_ = now;
  scanReassertAt_ = now + 2000;                   // стек піднімається асинхронно: ще раз виставити non-connectable
  applyScanMode(now, true);

  static const char* const kNames[] = {"BTC_TASK", "BTU_TASK", "BtAppT"};
  for (const char* n : kNames) Diag::registerTask(n, xTaskGetHandle(n));
  LOGI(TAG, "A2DP sink запущено як '%s' (SBC); BT вимкнений до SRC,bt (non-connectable)", kBtDeviceName);
}

Res BtSource::activate() {
  active_ = true;
  uint32_t now = millis();
  if (!PcmArena::acquire(PcmArena::Owner::Bt)) {   // область має бути вільна: SD віддає її в deactivate()
    Events::err(proto::errc::kBtFail, "arena");
    st_ = PlayState::Idle;
    return Res::Ok;
  }
  if (!started_) {
    Events::err(proto::errc::kBtFail, "not_started");
    st_ = PlayState::Idle;
    return Res::Ok;
  }
  if (!conn_) suppress_ = false;                    // розрив не в польоті; інакше suppress_ зніме подія DISCONNECTED
  manualDisc_ = false;                              // «ручний розрив» діє лише в межах одного сеансу src=bt
  giveUp_ = false;
  tries_ = 0;
  discRetries_ = 0;
  nextTryAt_ = now;                                 // нова серія спроб реконекту (до останнього пристрою)
  s_ring.setPendingRate(rate_ ? rate_ : (uint16_t)kDefaultRate);  // гарантуємо правильний формат I2S
  s_ring.flush();                                   // після SD в області сміття: індекси — на порожнє кільце
  s_ring.setAccepting(true);
  applyScanMode(now, true);
  st_ = derive();
  metaForce_ = true;
  if (st_ == PlayState::Play || st_ == PlayState::Pause) timeOnce_ = true;
  LOGI(TAG, "activate: conn=%d state=%s", conn_, stateName(st_));
  return Res::Ok;
}

void BtSource::deactivate() {
  active_ = false;
  optPause_ = false;
  s_ring.setAccepting(false);                       // вхідний A2DP-звук відкидається
  quiesceRing();
  uint32_t now = millis();
  if (started_ && conn_) {
    Diag::logHeap("bt-before-disc");
    if (st_ == PlayState::Play || audioStarted_) {
      s_a2dp->pause();                              // телефону — AVRCP pause
      noteCmd("pause (деактивація)");
      lastInactivePauseAt_ = now;
      inactivePauses_ = 1;
      uint32_t t0 = now;                            // коротка пауза: ≤ kBtPauseBeforeDiscMs, виходимо, коли потік зупинився
      while (audioStarted_ && (uint32_t)(millis() - t0) < kBtPauseBeforeDiscMs) {
        vTaskDelay(pdMS_TO_TICKS(kBtPausePollMs));
        service(millis());
      }
    }
    applyScanMode(millis(), true);                  // non-connectable ДО розриву: бібліотека не зробить connectable
    discRetries_ = 0;
    sendDisconnect(millis());                       // подія BT,disconnected прийде з service() рівно один раз
  } else {
    applyScanMode(now, true);
  }
  PcmArena::release(PcmArena::Owner::Bt);           // область вільна для SD (audio-задача кільце більше не читає)
}

// ============================================================ команди (AVRCP passthrough)

Res BtSource::play() {
  if (!started_ || !conn_) return Res::BadState;
  optPause_ = false;                                // намір змінився: далі стан за телефоном
  if (st_ == PlayState::Play) return Res::Ok;
  s_a2dp->play();
  noteCmd("play");
  return Res::Ok;                                   // стан зміниться за подією AVRCP/A2DP
}

Res BtSource::pause() {
  if (!started_ || !conn_) return Res::BadState;
  if (st_ == PlayState::Pause) return Res::Ok;
  s_a2dp->pause();
  noteCmd("pause");
#if BT_OPTIMISTIC_PAUSE
  if (st_ == PlayState::Play) {                     // телефон звітує про паузу лише через ≈4,3 с
    optPause_ = true;
    optUntil_ = millis() + kBtOptimisticPauseMs;
  }
#endif
  return Res::Ok;
}

Res BtSource::stop() {
  if (!started_ || !conn_) return Res::BadState;
  s_a2dp->stop();
  noteCmd("stop");
  return Res::Ok;
}

Res BtSource::next() {
  if (!started_ || !conn_) return Res::BadState;
  s_a2dp->next();
  noteCmd("next");
  return Res::Ok;
}

Res BtSource::prev() {
  if (!started_ || !conn_) return Res::BadState;
  s_a2dp->previous();
  noteCmd("prev");
  return Res::Ok;
}

// ============================================================ стан

PlayState BtSource::derive() const {
  if (!conn_) return PlayState::Idle;
  if (optPause_) return PlayState::Pause;           // оптимістична пауза (до підтвердження телефона)
  switch (avrc_) {
    case ESP_AVRC_PLAYBACK_PLAYING:
    case ESP_AVRC_PLAYBACK_FWD_SEEK:
    case ESP_AVRC_PLAYBACK_REV_SEEK: return PlayState::Play;
    case ESP_AVRC_PLAYBACK_PAUSED: return PlayState::Pause;
    case ESP_AVRC_PLAYBACK_STOPPED: return PlayState::Stop;
    default: break;                                 // AVRCP-статусу немає — дивимось на A2DP
  }
  return audioStarted_ ? PlayState::Play : PlayState::Stop;
}

uint32_t BtSource::posMsAt(uint32_t now) const {
  return st_ == PlayState::Play ? posBase_ + (now - posAt_) : posBase_;
}

SourceStatus BtSource::status() const { return SourceStatus{st_, posMsAt(millis()) / 1000, durS_}; }

void BtSource::sendTime(uint32_t now) {
  uint32_t p = posMsAt(now) / 1000;
  if (durS_ && p > durS_) p = durS_;
  Events::position(p, durS_);
}

void BtSource::emitInfo() {
  if (!conn_) return;
  Events::meta(artist_, title_, album_);
  if (st_ == PlayState::Play || st_ == PlayState::Pause) sendTime(millis());
}

void BtSource::clearMeta() {
  title_[0] = artist_[0] = album_[0] = 0;
  pT_[0] = pA_[0] = pAl_[0] = 0;
  durS_ = 0;
  posBase_ = 0;
  posAt_ = millis();
  metaPending_ = false;
  metaChanged_ = true;
}

void BtSource::reportConn(bool connected) { Events::bt(connected, name_); }

void BtSource::savePeer() {
  char hex[13];
  snprintf(hex, sizeof hex, "%02X%02X%02X%02X%02X%02X", peer_[0], peer_[1], peer_[2], peer_[3], peer_[4], peer_[5]);
  NvsStore::putStr(kKeyPeer, hex);
  LOGI(TAG, "пристрій збережено: %s", hex);
}

void BtSource::saveName() {
  if (!name_[0] || !strcmp(name_, savedName_)) return;  // без зайвих записів у флеш
  strncpy(savedName_, name_, sizeof savedName_ - 1);
  NvsStore::putStr(kKeyName, name_);
}

void BtSource::doForget(uint32_t now) {
  havePeer_ = false;
  memset(peer_, 0, sizeof peer_);
  savedName_[0] = 0;
  NvsStore::putStr(kKeyPeer, "");
  NvsStore::putStr(kKeyName, "");
  giveUp_ = false;
  tries_ = 0;
  if (conn_) sendDisconnect(now);
  // Без цього телефон після «забути» і модуль зі старим link key дають «authentication failed».
  int nb = esp_bt_gap_get_bond_device_num();
  if (nb > 0) {
    esp_bd_addr_t bonds[8];                         // 48 Б у стеку app; модуль парується з одним телефоном
    int cnt = nb > 8 ? 8 : nb;
    if (esp_bt_gap_get_bond_device_list(&cnt, bonds) == ESP_OK) {
      for (int i = 0; i < cnt; i++) esp_bt_gap_remove_bond_device(bonds[i]);
    }
    LOGI(TAG, "видалено bond-записів: %d", cnt);
  }
  applyScanMode(now, true);
  LOGI(TAG, "збережений пристрій забуто");
}

// Політика видимості: поза src=bt — non-connectable/non-discoverable (s_blockScan блокує і бібліотеку);
// при src=bt — connectable; discoverable лише якщо джерело активне, телефон не підключений і немає
// збереженого пристрою (або спроби вичерпано), або відкрито вікно DBG,bt,pair.
void BtSource::applyScanMode(uint32_t, bool force) {
  if (!started_) return;
  bool off = !active_;
  s_blockScan = off;
  if (conn_ && !off) return;                        // при звʼязку режим сканування лишає бібліотека
  bool disc = !off && (pairing_ || !havePeer_ || giveUp_);
  uint8_t want = off ? 0 : (disc ? 2 : 1);
  if (!force && scanApplied_ && want == s_scanMode) return;
  s_wantDisc = disc;
  esp_err_t e = off ? esp_bt_gap_set_scan_mode(ESP_BT_NON_CONNECTABLE, ESP_BT_NON_DISCOVERABLE)
                    : esp_bt_gap_set_scan_mode(ESP_BT_CONNECTABLE, disc ? ESP_BT_GENERAL_DISCOVERABLE : ESP_BT_NON_DISCOVERABLE);
  s_scanMode = want;
  scanDisc_ = disc;
  scanApplied_ = (e == ESP_OK);
  LOGI(TAG, "scan mode: %s (err=%d)", off ? "non-connectable (BT вимкнений)" : (disc ? "connectable, DISCOVERABLE" : "connectable, прихований"), (int)e);
}

void BtSource::onConnected(uint32_t now) {
  conn_ = true;
  connecting_ = false;
  giveUp_ = false;
  tries_ = 0;
  avrc_ = kAvrcUnknown;
  audioStarted_ = false;
  optPause_ = false;
  inactivePauses_ = 0;
  pairing_ = false;

  esp_bd_addr_t* p = s_a2dp->get_current_peer_address();
  if (p) {
    uint8_t addr[6];
    memcpy(addr, *p, 6);
    if (!havePeer_ || memcmp(addr, peer_, 6) != 0) {  // новий телефон
      memcpy(peer_, addr, 6);
      havePeer_ = true;
      savedName_[0] = 0;
      savePeer();
    }
  }
  // Імʼя: лише з кешу NVS. get_peer_name() у момент підключення повертає неініціалізований буфер,
  // тому читається пізніше й тільки через validName() (див. service()).
  Utf8::copySafe(name_, sizeof name_, savedName_);
  DeviceState::setBt(true, name_);
  nameReported_ = name_[0] != 0;
  nameDeadline_ = now + kBtNameWaitMs;
  nameWatchEnd_ = now + 5000;
  nameWatch_ = true;
  if (nameReported_) reportConn(true);
  manualDisc_ = false;                              // телефон підключився (сам або за реконектом) — новий сеанс звʼязку
  discWait_ = false;
  heapAt_ = now + kBtHeapAfterMs;
  heapTag_ = "bt-connected";
  LOGI(TAG, "підключено: '%s'", name_);
}

void BtSource::onDisconnected(uint32_t now, uint32_t evAt) {
  connecting_ = false;
  if (!conn_) return;                               // невдала спроба — лише скинули прапорець
  conn_ = false;
  audioStarted_ = false;
  avrc_ = kAvrcUnknown;
  optPause_ = false;
  nameWatch_ = false;
  DeviceState::setBt(false, name_);
  Events::bt(false, name_);                         // BT,disconnected — один раз на розрив
  clearMeta();
  tries_ = 0;
  giveUp_ = false;
  discWait_ = true;                                 // реконект вирішить resolveDisc() за disc_rsn
  discEvtAt_ = evAt;
  discDeadline_ = now + kBtDiscWaitMs;
  heapAt_ = now + kBtHeapAfterMs;
  heapTag_ = "bt-after-disc";
  scanReassertAt_ = now + 1000;                     // бібліотека може змінити scan mode після нас
  applyScanMode(now, true);
  LOGI(TAG, "відʼєднано: '%s'", name_);
}

// Хто розірвав: наш запит (suppress_) → модуль; disc_rsn NORMAL без нашого запиту → телефон (ручний розрив,
// без реконекту); інакше або причину не отримано → втрата звʼязку (реконект за ліміти).
void BtSource::resolveDisc(uint32_t now) {
  uint8_t rsn;
  uint32_t rsnAt;
  portENTER_CRITICAL(&s_mux);
  rsn = s_rsn;
  rsnAt = s_rsnAt;
  portEXIT_CRITICAL(&s_mux);
  int32_t dt = (int32_t)(rsnAt - discEvtAt_);
  bool fresh = rsn != kRsnNone && dt > -(int32_t)kBtDiscPairMs && dt < (int32_t)kBtDiscPairMs;
  if (!fresh && !due(now, discDeadline_)) return;
  discWait_ = false;
  lastRsn_ = fresh ? rsn : kRsnNone;
  if (suppress_) {                                  // розрив ініційовано модулем (деактивація/forget)
    suppress_ = false;
    lastWho_ = 'M';
    if (active_ && havePeer_) { tries_ = 0; nextTryAt_ = now + kBtSwitchReconnectMs; }  // src=bt повернули, поки йшов розрив
  } else if (fresh && rsn == ESP_A2D_DISC_RSN_NORMAL) {
    lastWho_ = 'U';
    manualDisc_ = true;                             // користувач відʼєднав у телефоні: не підключаємось самі
  } else {
    lastWho_ = 'L';
    nextTryAt_ = now + kBtLossReconnectDelayMs;
  }
  LOGI(TAG, "розрив: причина=%s ініціатор=%c manual=%d", fresh ? (rsn == ESP_A2D_DISC_RSN_NORMAL ? "NORMAL" : "ABNORMAL") : "не отримано",
       lastWho_, manualDisc_);
  applyScanMode(now, true);
}

void BtSource::sendDisconnect(uint32_t now) {
  suppress_ = true;
  discRetryAt_ = now + kBtDiscRetryMs;
  esp_err_t e = esp_a2d_sink_disconnect(peer_);     // адреса поточного пристрою (disconnect() бібліотеки бере last_connection)
  LOGI(TAG, "розрив A2DP (за політикою) err=%d", (int)e);
}

// ============================================================ службовий цикл

void BtSource::service(uint32_t now) {
  if (!started_) return;

  if (resetReq_) { resetReq_ = false; s_ring.resetStats(); lastIn_ = lastOut_ = 0; }
  if (forgetReq_) { forgetReq_ = false; doForget(now); }
  if (pairReq_) {
    pairReq_ = false;
    if (!active_) {
      LOGW(TAG, "pair лише при src=bt (BT вимкнений)");
    } else if (conn_) {
      LOGW(TAG, "уже підключено до '%s'; спершу DBG,bt,forget", name_);
    } else {
      pairing_ = true;
      pairUntil_ = now + kPairWindowMs;
      applyScanMode(now, true);
    }
  }
  if (pairing_ && due(now, pairUntil_)) { pairing_ = false; applyScanMode(now, true); LOGI(TAG, "вікно pair закрито"); }
  if (scanReassertAt_ && due(now, scanReassertAt_)) { scanReassertAt_ = 0; applyScanMode(now, true); }

  // ---- події бібліотеки ----
  Snap sn;
  takeInbox(sn);
  if (sn.rate) rate_ = sn.rateHz;
  if (sn.conn) {
    if (sn.connSt == ESP_A2D_CONNECTION_STATE_CONNECTING) connecting_ = true;
    else if (sn.connSt == ESP_A2D_CONNECTION_STATE_CONNECTED) onConnected(now);
    else if (sn.connSt == ESP_A2D_CONNECTION_STATE_DISCONNECTED) onDisconnected(now, sn.connAtMs);
  }
  if (discWait_) resolveDisc(now);
  if (sn.avrc) {
    avrc_ = sn.avrcSt;
    LOGI(TAG, "подія AVRCP-статусу: %s", avrcName(sn.avrcSt));
  }
  if (sn.audio) {
    audioStarted_ = (sn.audioSt == ESP_A2D_AUDIO_STATE_STARTED);
    LOGI(TAG, "подія A2DP audio: %s", audioStarted_ ? "STARTED" : "STOPPED/SUSPEND");
  }
  if (sn.pos) {                                        // позиція від телефона: коригуємо лише помітну розбіжність
    int32_t d = (int32_t)(sn.posMs - posMsAt(now));
    if (st_ != PlayState::Play || d > kPosDeadZoneMs || d < -kPosDeadZoneMs) {
      posBase_ = sn.posMs;
      posAt_ = now;
    }
  }
  if (sn.meta) {
    memcpy(pT_, sn.title, sizeof pT_);
    if (!strcasecmp(pT_, "Not Provided")) pT_[0] = 0;  // телефон так позначає відсутню назву → порожнє поле
    memcpy(pA_, sn.artist, sizeof pA_);
    memcpy(pAl_, sn.album, sizeof pAl_);
    pendDurMs_ = sn.durMs;
    metaAt_ = sn.metaAtMs;
    metaPending_ = true;
  }

  // ---- ім'я пристрою (бібліотека читає його трохи після підключення) ----
  if (conn_ && nameWatch_) {
    char tmp[sizeof name_];
    if (validName(s_a2dp->get_peer_name(), tmp, sizeof tmp) && strcmp(tmp, name_) != 0) {
      memcpy(name_, tmp, sizeof name_);
      DeviceState::setBt(true, name_);
      reportConn(true);
      nameReported_ = true;
      saveName();
    } else if (!nameReported_ && due(now, nameDeadline_)) {
      reportConn(true);                                // імʼя так і не дізналися — порожнє поле допустиме
      nameReported_ = true;
    }
    if (due(now, nameWatchEnd_)) nameWatch_ = false;
  }

  // ---- метадані з дебаунсом ----
  if (metaPending_ && due(now, metaAt_ + kBtMetaDebounceMs)) {
    metaPending_ = false;
    bool diff = strcmp(title_, pT_) || strcmp(artist_, pA_) || strcmp(album_, pAl_) || (pendDurMs_ / 1000) != durS_;
    if (diff) {
      bool newTrack = strcmp(title_, pT_) || strcmp(artist_, pA_);
      memcpy(title_, pT_, sizeof title_);
      memcpy(artist_, pA_, sizeof artist_);
      memcpy(album_, pAl_, sizeof album_);
      durS_ = pendDurMs_ / 1000;
      if (newTrack) { posBase_ = 0; posAt_ = now; }
      metaChanged_ = true;
    }
  }

  // ---- оптимістична пауза: коли знімати ----
  if (optPause_) {
    bool realPaused = (avrc_ == ESP_AVRC_PLAYBACK_PAUSED) || (avrc_ == ESP_AVRC_PLAYBACK_STOPPED) ||
                      (avrc_ == kAvrcUnknown && !audioStarted_);
    uint32_t sinceMs = now - (optUntil_ - kBtOptimisticPauseMs);
    if (realPaused) {
      optPause_ = false;
      LOGI(TAG, "оптимістичну паузу підтверджено телефоном через %u мс", (unsigned)sinceMs);
    } else if (due(now, optUntil_)) {
      optPause_ = false;
      LOGW(TAG, "телефон не підтвердив паузу за %u мс: повернення до реального стану", (unsigned)sinceMs);
    }
  }

  // ---- стан за AVRCP/A2DP (з урахуванням оптимістичної паузи) ----
  PlayState ns = derive();
  if (ns != st_) {
    if (st_ == PlayState::Play) posBase_ = posMsAt(now);   // заморозити позицію
    if (ns == PlayState::Play) { posAt_ = now; lastTimeAt_ = now; }
    if (ns == PlayState::Stop || ns == PlayState::Idle) posBase_ = 0;
    if (ns == PlayState::Pause) timeOnce_ = true;          // один TIME при паузі
    if (s_cmdAt && (now - s_cmdAt) < 15000) {
      LOGI(TAG, "стан %s -> %s (avrcp=%s a2dp_audio=%d opt=%d), +%u мс після команди %s", stateName(st_),
           stateName(ns), avrcName(avrc_), audioStarted_, optPause_, (unsigned)(now - s_cmdAt),
           s_cmdName ? s_cmdName : "?");
      s_cmdAt = 0;
    } else {
      LOGI(TAG, "стан %s -> %s (avrcp=%s a2dp_audio=%d opt=%d)", stateName(st_), stateName(ns), avrcName(avrc_),
           audioStarted_, optPause_);
    }
    st_ = ns;
  }

  // ---- неактивне джерело: телефон не має грати ----
  if (!active_ && conn_) {
    bool playing = (avrc_ == ESP_AVRC_PLAYBACK_PLAYING) || audioStarted_;
    if (!playing) {
      inactivePauses_ = 0;
    } else if (inactivePauses_ < 10 && due(now, lastInactivePauseAt_ + kBtInactivePauseGapMs)) {
      s_a2dp->pause();
      lastInactivePauseAt_ = now;
      inactivePauses_++;
      LOGI(TAG, "джерело не bt, а телефон грає: повторний AVRCP pause (%u)", (unsigned)inactivePauses_);
    }
    if (!suppress_) {                                  // підключився попри політику (завершилась давня спроба)
      discRetries_ = 0;
      sendDisconnect(now);
    } else if (discRetries_ < kBtDiscRetryMax && due(now, discRetryAt_)) {
      discRetries_++;
      sendDisconnect(now);                             // подія розриву не прийшла: повторюємо запит
    }
  }

  // ---- автореконект до останнього пристрою ----
  if (connecting_ && due(now, connectAt_ + kBtConnectTimeoutMs)) connecting_ = false;
  bool mayTry = active_;                               // реконект лише при src=bt
  if (!conn_ && !connecting_ && havePeer_ && !giveUp_ && !suppress_ && !manualDisc_ && !discWait_ && mayTry &&
      due(now, nextTryAt_)) {
    if (tries_ >= kBtReconnectMaxTries) {
      giveUp_ = true;
      LOGW(TAG, "реконект: вичерпано %u спроб", (unsigned)kBtReconnectMaxTries);
      if (active_) Events::err(proto::errc::kBtFail, "reconnect");
      applyScanMode(now, true);                        // тепер можна показатись новому телефону
    } else {
      tries_++;
      connectAt_ = now;
      nextTryAt_ = now + kBtReconnectIntervalMs;
      bool ok = s_a2dp->connect_to(peer_);
      connecting_ = ok;
      LOGI(TAG, "реконект, спроба %u/%u: %s", (unsigned)tries_, (unsigned)kBtReconnectMaxTries, ok ? "запит надіслано" : "помилка");
      if (!ok && active_) Events::err(proto::errc::kBtFail, "connect");
    }
  }

  if (heapAt_ && due(now, heapAt_)) {                  // поточні free/largest після зміни стану звʼязку
    heapAt_ = 0;
    Diag::logHeap(heapTag_);
  }

  // ---- швидкість потоку, раз на секунду ----
  if (due(now, statAt_ + 1000)) {
    uint32_t dt = now - statAt_;
    statAt_ = now;
    uint32_t in = s_ring.inBytes, out = s_ring.outBytes;
    inRate_ = (uint32_t)((uint64_t)(in - lastIn_) * 1000 / dt);
    outRate_ = (uint32_t)((uint64_t)(out - lastOut_) * 1000 / dt);
    lastIn_ = in;
    lastOut_ = out;
  }
}

void BtSource::loop(uint32_t now) {
  service(now);
  if (!started_) return;
  if (metaChanged_ || (metaForce_ && conn_)) {
    metaChanged_ = false;
    metaForce_ = false;
    Events::meta(artist_, title_, album_);
  }
  if (st_ == PlayState::Play && due(now, lastTimeAt_ + 1000)) {
    lastTimeAt_ = now;
    sendTime(now);
  }
  if (timeOnce_) {
    timeOnce_ = false;
    sendTime(now);
  }
}

// ============================================================ DBG

void BtSource::dump() const {
  char hex[13];
  snprintf(hex, sizeof hex, "%02X%02X%02X%02X%02X%02X", peer_[0], peer_[1], peer_[2], peer_[3], peer_[4], peer_[5]);
  LOGI(TAG, "started=%d active=%d conn=%d connecting=%d state=%s opt_pause=%d dev='%s' saved=%s", started_, active_,
       conn_, connecting_, stateName(st_), optPause_, name_, havePeer_ ? hex : "—");
  LOGI(TAG, "кодек=SBC частота=%u Гц avrcp=%s a2dp_audio=%d scan=%s pair=%d", (unsigned)rate_, avrcName(avrc_),
       audioStarted_, scanDisc_ ? "discoverable" : "прихований", pairing_);
  LOGI(TAG, "PCM вх=%u Б/с вих=%u Б/с (очік. %u) всього вх=%u вих=%u відкинуто(неактивне)=%u скинуто=%u",
       (unsigned)inRate_, (unsigned)outRate_, (unsigned)rate_ * 4u, (unsigned)s_ring.inBytes,
       (unsigned)s_ring.outBytes, (unsigned)s_ring.dropInactive, (unsigned)s_ring.flushed);
  LOGI(TAG, "overrun=%u (%u Б) underrun=%u wr_timeout=%u max_write=%u мкс кільце=%u/%u", (unsigned)s_ring.overruns,
       (unsigned)s_ring.overrunBytes, (unsigned)s_ring.underruns, (unsigned)s_ring.writeTimeouts,
       (unsigned)s_ring.maxWriteUs, (unsigned)s_ring.available(), (unsigned)s_ring.capacity());
  LOGI(TAG, "реконект спроб=%u/%u giveup=%d suppress=%d", (unsigned)tries_, (unsigned)kBtReconnectMaxTries, giveUp_,
       suppress_);
  static const char* const kScanNames[] = {"non-connectable/non-discoverable", "connectable, прихований",
                                           "connectable, discoverable", "невідомо"};
  uint8_t sm = s_scanMode > 3 ? 3 : s_scanMode;
  const char* rs = lastRsn_ == kRsnNone ? "невідома" : (lastRsn_ == ESP_A2D_DISC_RSN_NORMAL ? "NORMAL" : "ABNORMAL");
  const char* who = lastWho_ == 'U' ? "телефон" : lastWho_ == 'L' ? "втрата звʼязку" : lastWho_ == 'M' ? "модуль" : "—";
  LOGI(TAG, "ручний_розрив=%d остання_причина=%s (%s) scan_mode=%s політика_BT_вимкнений=%d очікування_причини=%d", manualDisc_,
       rs, who, kScanNames[sm], !active_, discWait_);
  Diag::logHeap("dbg-bt");
  LOGI(TAG, "meta: артист='%s' назва='%s' альбом='%s' трив=%u с", artist_, title_, album_, (unsigned)durS_);
}
#include "sources/stub_source.h"

#include <Arduino.h>

#include "config.h"
#include "core/device_state.h"
#include "core/events.h"
#include "util/log.h"

namespace {
constexpr const char* TAG = "stub";
constexpr uint8_t kTracks = 3, kFolders = 2;
constexpr uint8_t kFolderOf[kTracks] = {0, 0, 1};
constexpr uint8_t kFolderFirst[kFolders] = {0, 2};
constexpr uint8_t kFolderCount[kFolders] = {2, 1};
constexpr const char* kFolderName[kFolders] = {"Stub A", "Stub B"};
constexpr uint32_t kScanMs = 400, kBtConnectMs = 1500, kTagsMs = 300;
inline bool due(uint32_t now, uint32_t t) { return (int32_t)(now - t) >= 0; }
}  // namespace

bool StubSource::begin() {
  LOGI(TAG, "[%s] begin", srcName(id_));
  return true;
}

bool StubSource::mediaPresent() const {
  return id_ == SrcId::SD ? DeviceState::sd() : DeviceState::usb();
}

Res StubSource::activate() {
  active_ = true;
  pos_ = 0;
  LOGI(TAG, "[%s] activate", srcName(id_));
  if (!isMedia()) {
    st_ = DeviceState::btConnected() ? PlayState::Stop : PlayState::Idle;
    btAt_ = millis() + kBtConnectMs;  // імітація підключення телефону
    return Res::Ok;
  }
  if (!mediaPresent()) {
    st_ = PlayState::Idle;
    return Res::NoMedia;
  }
  st_ = PlayState::Scan;
  track_ = 0;
  scanEnd_ = millis() + kScanMs;
  return Res::Ok;
}

void StubSource::deactivate() {
  LOGI(TAG, "[%s] deactivate", srcName(id_));
  active_ = false;
  tagsPending_ = false;
  st_ = PlayState::Idle;
  pos_ = 0;
}

Res StubSource::play() {
  if (st_ == PlayState::Scan) return Res::BadState;
  if (st_ == PlayState::Idle) return isMedia() && !mediaPresent() ? Res::NoMedia : Res::BadState;
  if (st_ == PlayState::Play) return Res::Ok;
  bool fresh = (st_ == PlayState::Stop);
  st_ = PlayState::Play;
  nextTick_ = millis() + 1000;
  LOGI(TAG, "[%s] play", srcName(id_));
  if (!isMedia()) {
    if (fresh) pos_ = 0;
    Events::meta("Stub Artist", "Stub BT song", "Stub Album");
    Events::position(pos_, 0);
  } else if (fresh) {
    pos_ = 0;
    sendInfo();
  }
  return Res::Ok;
}

Res StubSource::pause() {
  if (st_ == PlayState::Pause) return Res::Ok;
  if (st_ != PlayState::Play) return Res::BadState;
  st_ = PlayState::Pause;
  LOGI(TAG, "[%s] pause", srcName(id_));
  Events::position(pos_, isMedia() ? dur_ : 0);  // один раз при pause
  return Res::Ok;
}

Res StubSource::stop() {
  if (st_ == PlayState::Scan) return Res::BadState;
  if (st_ == PlayState::Play || st_ == PlayState::Pause) {
    st_ = PlayState::Stop;
    pos_ = 0;
    LOGI(TAG, "[%s] stop", srcName(id_));
  }
  return Res::Ok;
}

Res StubSource::move(int dir, bool folder) {
  if (!isMedia()) {  // BT: NEXT/PREV як AVRCP passthrough, FNEXT/FPREV недоступні
    if (folder || st_ == PlayState::Idle) return Res::BadState;
    LOGI(TAG, "[bt] AVRCP passthrough %s", dir > 0 ? "NEXT" : "PREV");
    return Res::Ok;
  }
  if (st_ == PlayState::Scan) return Res::BadState;
  if (st_ == PlayState::Idle) return mediaPresent() ? Res::BadState : Res::NoMedia;
  if (!folder && dir < 0 && pos_ > 3) {  // PREV при позиції > 3 с: перезапуск
    pos_ = 0;
    Events::position(0, dur_);
    return Res::Ok;
  }
  if (folder) {
    int f = (kFolderOf[track_] + dir + kFolders) % kFolders;
    track_ = kFolderFirst[f];
  } else {
    track_ = (uint8_t)((track_ + dir + kTracks) % kTracks);
  }
  st_ = PlayState::Play;
  restartTrack();
  return Res::Ok;
}

void StubSource::restartTrack() {
  pos_ = 0;
  nextTick_ = millis() + 1000;
  sendInfo();
}

void StubSource::sendInfo() {  // порядок: TRACK, FOLDER, META, TIME (далі STATE від менеджера)
  char fn[16];
  snprintf(fn, sizeof fn, "track%02u", (unsigned)track_ + 1);
  Events::track(track_ + 1, kTracks);
  Events::folder(kFolderName[kFolderOf[track_]], kFolderCount[kFolderOf[track_]]);
  Events::meta("", fn, "");  // одразу: назва = імʼя файлу
  Events::position(pos_, dur_);
  tagsPending_ = true;
  tagsAt_ = millis() + kTagsMs;  // повторно після «розбору тегів»
}

SourceStatus StubSource::status() const {
  return SourceStatus{st_, pos_, isMedia() ? dur_ : 0};
}

void StubSource::loop(uint32_t now) {
  if (!active_) return;
  if (!isMedia()) {
    if (st_ == PlayState::Idle && due(now, btAt_)) {
      DeviceState::setBt(true, "Stub Phone");
      Events::bt(true, "Stub Phone");
      st_ = PlayState::Stop;
      LOGI(TAG, "[bt] імітація: телефон підключено");
    }
  } else if (st_ == PlayState::Scan && due(now, scanEnd_)) {
    st_ = PlayState::Play;
    LOGI(TAG, "[%s] scan завершено, play", srcName(id_));
    restartTrack();
  }
  if (tagsPending_ && due(now, tagsAt_)) {
    tagsPending_ = false;
    char fn[16];
    snprintf(fn, sizeof fn, "track%02u", (unsigned)track_ + 1);
    Events::meta("Stub Artist", fn, "Stub Album");
  }
  if (st_ == PlayState::Play && due(now, nextTick_)) {
    nextTick_ += 1000;
    pos_++;
    if (isMedia() && pos_ >= dur_) {  // кінець треку природним шляхом
      if (rep_ == 1) restartTrack();
      else if (track_ + 1 < kTracks) { track_++; restartTrack(); }
      else if (rep_ == 2) { track_ = 0; restartTrack(); }
      else { st_ = PlayState::Stop; pos_ = 0; }
    } else {
      Events::position(pos_, isMedia() ? dur_ : 0);
    }
  }
}

void StubSource::emitInfo() {
  if (!isMedia()) {
    if (st_ == PlayState::Play || st_ == PlayState::Pause) {
      Events::meta("Stub Artist", "Stub BT song", "Stub Album");
      Events::position(pos_, 0);
    }
    return;
  }
  if (st_ == PlayState::Play || st_ == PlayState::Pause || st_ == PlayState::Stop) {
    char fn[16];
    snprintf(fn, sizeof fn, "track%02u", (unsigned)track_ + 1);
    Events::track(track_ + 1, kTracks);
    Events::folder(kFolderName[kFolderOf[track_]], kFolderCount[kFolderOf[track_]]);
    Events::meta("Stub Artist", fn, "Stub Album");
    Events::position(pos_, dur_);
  }
}

Res StubSource::refreshMedia() {
  if (isMedia() && active_ && st_ != PlayState::Idle && !mediaPresent()) {
    st_ = PlayState::Idle;
    pos_ = 0;
    tagsPending_ = false;
    return Res::NoMedia;
  }
  return Res::Ok;
}
#include "sources/source_manager.h"

#include "core/device_state.h"
#include "core/events.h"
#include "storage/nvs_store.h"
#include "util/diag.h"
#include "util/log.h"

namespace {
constexpr const char* TAG = "src";
constexpr const char* kKeyLast = "last_src";
}  // namespace

void SourceManager::begin(AudioOut* audio, ISource* bt, ISource* sd, ISource* usb) {
  audio_ = audio;
  src_[(int)SrcId::BT] = bt;
  src_[(int)SrcId::SD] = sd;
  src_[(int)SrcId::USB] = usb;
  for (int i = 1; i <= 3; i++) src_[i]->begin();
  uint8_t v = 0;
  if (NvsStore::getU8(kKeyLast, v) && v >= 1 && v <= 3) lastSaved_ = v;
  LOGI(TAG, "старт: src=off, останнє джерело в NVS: %s", srcName((SrcId)lastSaved_));
}

PlayState SourceManager::state() const {
  ISource* s = src_[(int)cur_];
  return s ? s->status().state : PlayState::Idle;
}

void SourceManager::saveLast(SrcId id) {
  if ((uint8_t)id == lastSaved_) return;  // не гаяти флеш без потреби
  lastSaved_ = (uint8_t)id;
  NvsStore::putU8(kKeyLast, lastSaved_);
}

void SourceManager::resumeLast() {
  if (lastSaved_ >= 1 && lastSaved_ <= 3) select((SrcId)lastSaved_);
}

void SourceManager::emitErr(Res r, const char* detail) { Events::err(resCode(r), detail); }

Res SourceManager::select(SrcId id) {
  if (id == cur_) {  // ідемпотентно
    LOGI(TAG, "джерело %s вже активне", srcName(id));
    return Res::Ok;
  }
  LOGI(TAG, "перемикання %s -> %s", srcName(cur_), srcName(id));
  audio_->mute();  // тиша до зупинки попереднього джерела
  ISource* prev = src_[(int)cur_];
  if (prev) {
    LOGI(TAG, "deactivate %s", srcName(cur_));
    prev->deactivate();  // завершується до activate() нового
  }
  cur_ = id;
  Res r = Res::Ok;
  ISource* nx = src_[(int)id];
  if (nx) {
    nx->setShuffle(shuf_);
    nx->setRepeat(rep_);
    LOGI(TAG, "activate %s", srcName(id));
    r = nx->activate();
  } else {
    LOGI(TAG, "src=off: DAC замʼючено");  // BT: вхідний A2DP відкидається, AVRCP pause (етап 3)
  }
  if (id != SrcId::Off) saveLast(id);
  Diag::logHeap("src");
  Diag::logTasks();
  publish(false);
  if (r != Res::Ok) emitErr(r, nullptr);
  return r;
}

void SourceManager::publish(bool force) {
  ISource* s = src_[(int)cur_];
  PlayState st = s ? s->status().state : PlayState::Idle;
  bool srcStateChanged = cur_ != pubSrc_ || st != pubState_;
  bool changed = force || srcStateChanged || shuf_ != pubShuf_ || rep_ != pubRep_;
  if (!changed) return;
  if (srcStateChanged && !force) {  // DAC звучить лише при state==play
    if (cur_ != SrcId::Off && st == PlayState::Play) audio_->unmute();
    else audio_->mute();
  }
  pubSrc_ = cur_;
  pubState_ = st;
  pubShuf_ = shuf_;
  pubRep_ = rep_;
  Events::state(cur_, st, shuf_, rep_);
}

void SourceManager::emitFullState() {
  publish(true);
  Events::media(DeviceState::sd(), DeviceState::usb());
  char name[33];
  DeviceState::btName(name, sizeof name);
  Events::bt(DeviceState::btConnected(), name);
  ISource* s = src_[(int)cur_];
  if (s) s->emitInfo();
}

void SourceManager::onMediaChanged() {
  Events::media(DeviceState::sd(), DeviceState::usb());
  ISource* s = src_[(int)cur_];
  if (s && (cur_ == SrcId::SD || cur_ == SrcId::USB)) {
    Res r = s->refreshMedia();
    if (r != Res::Ok) emitErr(r, nullptr);
  }
  publish(false);
}

void SourceManager::handle(const Cmd& c) {
  using proto::CmdId;
  switch (c.id) {
    case CmdId::Hello:
      Events::ready();
      emitFullState();
      return;
    case CmdId::Get:
      emitFullState();
      return;
    case CmdId::Src:
      select((SrcId)c.arg);
      return;
    case CmdId::Shuf:
      shuf_ = c.arg != 0;
      for (int i = 1; i <= 3; i++) src_[i]->setShuffle(shuf_);
      publish(false);
      return;
    case CmdId::Rep:
      rep_ = (uint8_t)c.arg;
      for (int i = 1; i <= 3; i++) src_[i]->setRepeat(rep_);
      publish(false);
      return;
    case CmdId::DbgMedia:
      DeviceState::setMedia(c.arg & 1, (c.arg & 2) != 0);
      onMediaChanged();
      return;
    default:
      break;
  }

  ISource* s = src_[(int)cur_];
  if (!s) {  // src=off: команди керування не мають адресата
    emitErr(Res::BadState, proto::cmdName(c.id));
    return;
  }
  Res r = Res::Ok;
  switch (c.id) {
    case CmdId::Play: r = s->play(); break;
    case CmdId::Pause: r = s->pause(); break;
    case CmdId::Toggle: r = (s->status().state == PlayState::Play) ? s->pause() : s->play(); break;
    case CmdId::Stop: r = s->stop(); break;
    case CmdId::Next: r = s->next(); break;
    case CmdId::Prev: r = s->prev(); break;
    case CmdId::FNext: r = s->fnext(); break;
    case CmdId::FPrev: r = s->fprev(); break;
    default: break;
  }
  if (r != Res::Ok) emitErr(r, proto::cmdName(c.id));
  publish(false);
}

void SourceManager::loop(uint32_t nowMs) {
  ISource* s = src_[(int)cur_];
  if (s) s->loop(nowMs);
  publish(false);  // STATE після TRACK/FOLDER/META/TIME, які породив loop()
}
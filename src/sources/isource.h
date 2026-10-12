#pragma once
#include <stdint.h>

#include "core/types.h"

struct SourceStatus {
  PlayState state;
  uint32_t posS;
  uint32_t durS;   // 0 якщо невідома
};

// Інтерфейс джерела. Усі методи викликаються лише з задачі app (loopTask).
class ISource {
 public:
  virtual ~ISource() {}
  virtual SrcId id() const = 0;
  virtual bool begin() = 0;               // одноразова ініціалізація
  virtual Res activate() = 0;             // NoMedia → джерело лишається активним у idle
  virtual void deactivate() = 0;          // повертається, коли джерело повністю зупинене
  virtual Res play() = 0;
  virtual Res pause() = 0;
  virtual Res stop() = 0;
  virtual Res next() = 0;
  virtual Res prev() = 0;
  virtual Res fnext() = 0;
  virtual Res fprev() = 0;
  virtual void setRepeat(uint8_t mode) = 0;
  virtual void setShuffle(bool on) = 0;
  virtual SourceStatus status() const = 0;
  virtual void loop(uint32_t nowMs) = 0;  // викликається щоітерації app
  virtual void emitInfo() = 0;            // TRACK, FOLDER, META, TIME для GET/HELLO
  virtual Res refreshMedia() { return Res::Ok; }  // після зміни носіїв
};
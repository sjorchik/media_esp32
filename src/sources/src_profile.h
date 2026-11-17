#pragma once
#include "audio/level.h"
#include "core/types.h"
#include "storage/resume_store.h"

// M4b4. Усе, чим відрізняються SD- і USB-джерела всередині спільного SdSource (скан, плейлист, навігація, shuffle, резюме).
// Чиста константа: без Arduino/FreeRTOS, перевіряється в native.
struct SrcProfile {
  SrcId id;               // ідентифікатор джерела (STATE, srcName)
  const char* tag;        // тег логу
  ResumeStore::Src rsm;   // ключ NVS резюме (rsm_sd / rsm_usb)
  level::Slot slot;       // слот цифрового рівня (шлях PCM відтворення)
  bool ownsMount;         // джерело саме відкриває/закриває носій (USB: activate/deactivate); SD монтує hot-plug (SdMedia)
  bool volId;             // резюме привʼязане до серійного номера тому (USB)
};

inline constexpr SrcProfile kSdProfile{SrcId::SD, "sdsrc", ResumeStore::Src::Sd, level::Slot::Sd, false, false};
inline constexpr SrcProfile kUsbProfile{SrcId::USB, "usbsrc", ResumeStore::Src::Usb, level::Slot::Usb, true, true};

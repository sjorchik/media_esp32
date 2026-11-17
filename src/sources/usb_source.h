#pragma once
#include "config.h"

#if ENABLE_USB_CH376

#include "media/usb_media.h"
#include "sources/sd_source.h"

// M4b4. USB-джерело: ті самі скан, плейлист, навігація, shuffle, резюме й помилки, що в SD (SdSource), але з профілем kUsbProfile
// (STATE,usb; резюме rsm_usb із серійним номером тому; слот рівня usb; том відкриває activate() і закриває deactivate()).
// DBG: DBG,usb,vscan | vlist[,N] | vtagn,<n> → dbg(Dbg::Scan|List|Tag); DBG,nav|shuf|resume|decode для активного USB.
class UsbSource final : public SdSource {
 public:
  UsbSource(UsbMedia& media, Mp3Player& dec) : SdSource(media, dec, kUsbProfile) {}
};

#endif  // ENABLE_USB_CH376

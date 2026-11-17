#pragma once
#include "audio/audio_out.h"
#include "sources/bt_source.h"
#include "sources/sd_source.h"
#include "sources/source_manager.h"
#if ENABLE_USB_CH376
#include "sources/usb_source.h"
#endif

// Глобальні обʼєкти застосунку (визначені в main.cpp)
extern AudioOut g_audio;
extern SourceManager g_sources;
extern BtSource g_bt;
extern SdSource g_sd;
extern Mp3Player g_dec;  // M4b4: єдиний MP3-декодер (SD і USB)
#if ENABLE_USB_CH376
extern UsbSource g_usb;
#endif


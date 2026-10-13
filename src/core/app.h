#pragma once
#include "audio/audio_out.h"
#include "sources/bt_source.h"
#include "sources/source_manager.h"

// Глобальні обʼєкти застосунку (визначені в main.cpp)
extern AudioOut g_audio;
extern SourceManager g_sources;
extern BtSource g_bt;
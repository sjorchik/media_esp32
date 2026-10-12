#pragma once
#include <stddef.h>
#include <stdint.h>

// ============================================================
//  РОЗПОДІЛ ЗАДАЧ FreeRTOS (документація)
//  Ядро 0 (PRO): BT-стек (контролер + Bluedroid, закріплені SDK, етап 3),
//                link_tx (prio 3), link_rx (prio 3), console (prio 2)
//  Ядро 1 (APP): audio (prio 6; тон/декодер/I2S-запис),
//                loopTask «app» (prio 1: виконання всіх команд, SourceManager::loop,
//                читання SD/USB на етапах 2–4)
//  Команди з UART0 і UART1 не виконуються в своїх задачах: роутер лише валідує
//  і кладе Cmd у чергу, виконує loopTask → немає гонок за станом.
// ============================================================

#define FW_VERSION "0.1.0"
inline constexpr int kProtoVer = 1;

// ---- Прапорці збірки ----
#ifndef AUTO_RESUME_SOURCE
#define AUTO_RESUME_SOURCE 0   // 1: після старту вмикати останнє джерело з NVS
#endif
#ifndef LOG_LEVEL
#define LOG_LEVEL 3            // 1=E 2=W 3=I 4=D
#endif
#define MIRROR_HB 0            // 1: дзеркалити HB в UART0
#define RELEASE_BLE_MEM 1      // esp_bt_controller_mem_release(ESP_BT_MODE_BLE) на старті
#define STUB_SD_PRESENT 1      // початковий стан «носій є» для заглушок
#define STUB_USB_PRESENT 1

// ---- Консоль / лог (UART0) ----
inline constexpr uint32_t kConsoleBaud = 115200;
inline constexpr size_t   kConsoleRxBuf = 512;
inline constexpr size_t   kConsoleLineMax = 260;

// ---- Лінк із S3 (UART1) ----
inline constexpr uint32_t kLinkBaud = 921600;
inline constexpr size_t   kLinkRxBuf = 2048;
inline constexpr uint32_t kHbPeriodMs = 1000;
inline constexpr uint32_t kPartnerTimeoutMs = 3000;
inline constexpr size_t   kTxHiDepth = 8;     // STATE, ERR, READY, MEDIA, BT
inline constexpr size_t   kTxLoDepth = 12;    // TRACK, FOLDER, META, TIME
inline constexpr size_t   kCmdQueueDepth = 16;

// ---- Аудіо ----
inline constexpr uint32_t kDefaultRate = 44100;
inline constexpr uint32_t kFadeMs = 8;          // fade-in / хвіст fade-out
inline constexpr uint32_t kMuteSettleMs = 10;   // після XSMT=LOW
inline constexpr uint32_t kUnmuteSettleMs = 10; // після XSMT=HIGH
inline constexpr size_t   kChunkFrames = 128;
inline constexpr int      kI2sDmaCount = 8;
inline constexpr int      kI2sDmaLen = 128;     // кадрів на буфер
inline constexpr float    kToneAmpl = 0.25f;

// ---- Задачі ----
inline constexpr int kCoreProto = 0;
inline constexpr int kCoreAudio = 1;
inline constexpr uint32_t kStackLinkTx = 3072;
inline constexpr uint32_t kStackLinkRx = 4096;
inline constexpr uint32_t kStackConsole = 4096;
inline constexpr uint32_t kStackAudio = 4096;
inline constexpr int kPrioLink = 3;
inline constexpr int kPrioConsole = 2;
inline constexpr int kPrioAudio = 6;

// ---- Діагностика ----
inline constexpr uint32_t kDiagPeriodMs = 60000;
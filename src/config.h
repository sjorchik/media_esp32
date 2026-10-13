#pragma once
#include <stddef.h>
#include <stdint.h>

// ============================================================
//  РОЗПОДІЛ ЗАДАЧ FreeRTOS (документація)
//  Ядро 0 (PRO): BT-стек (контролер + Bluedroid; задачі BTC_TASK/BTU_TASK/BtAppT
//                створює SDK/бібліотека A2DP), link_tx (prio 3), link_rx (prio 3),
//                console (prio 2)
//  Ядро 1 (APP): audio (prio 6; тон, помпа PCM з кільця BT → AudioOut, на етапах 3–4 декодер),
//                loopTask «app» (prio 1: виконання всіх команд, SourceManager::loop,
//                BtSource::service, читання SD/USB на етапах 3–4)
//  Команди з UART0 і UART1 не виконуються в своїх задачах: роутер лише валідує
//  і кладе Cmd у чергу, виконує loopTask → немає гонок за станом.
//  BT-колбеки (інші задачі) лише кладуть події в Inbox під spinlock; решту робить app.
// ============================================================

#define FW_VERSION "0.3.0"
inline constexpr int kProtoVer = 1;

// ---- Прапорці збірки ----
#ifndef AUTO_RESUME_SOURCE
#define AUTO_RESUME_SOURCE 0   // 1: після старту вмикати останнє джерело з NVS
#endif
#ifndef LOG_LEVEL
#define LOG_LEVEL 3            // 1=E 2=W 3=I 4=D
#endif
#ifndef DAC_MUTE_ACTIVE_HIGH
#define DAC_MUTE_ACTIVE_HIGH 0 // 0 = PCM5102 (XSMT: HIGH=звук); 1 = UDA1334A (MUTE: HIGH=мʼют)
#endif
#ifndef HAVE_DAC
#define HAVE_DAC 1             // інформативний: 0 → у лозі «DAC відсутній»
#endif
#ifndef ENABLE_USB_CH376
#define ENABLE_USB_CH376 0     // етап 4
#endif
#define MIRROR_HB 0            // 1: дзеркалити HB в UART0
#define RELEASE_BLE_MEM 1      // esp_bt_controller_mem_release(ESP_BT_MODE_BLE) на старті
#define STUB_SD_PRESENT 0      // SD тепер справжній: старт без картки, далі hot-plug (SdMedia)
#define STUB_USB_PRESENT 1

// ---- Консоль / лог (UART0) ----
inline constexpr uint32_t kConsoleBaud = 460800;
inline constexpr size_t   kConsoleRxBuf = 512;
inline constexpr size_t   kConsoleLineMax = 260;

// ---- Лінк із S3 (UART1) ----
inline constexpr uint32_t kLinkBaud = 921600;
inline constexpr size_t   kLinkRxBuf = 2048;
inline constexpr uint32_t kHbPeriodMs = 1000;
inline constexpr uint32_t kPartnerTimeoutMs = 3000;
inline constexpr size_t   kTxHiDepth = 8;     // STATE, ERR, READY, MEDIA, BT
inline constexpr size_t   kTxLoDepth = 12;    // TRACK, FOLDER, META (TIME — окремий слот)
inline constexpr uint32_t kTxWaitMs = 30;     // скільки send() чекає місця в черзі до фіксації втрати
inline constexpr size_t   kCmdQueueDepth = 16;

// ---- Аудіо ----
inline constexpr uint32_t kDefaultRate = 44100;
inline constexpr uint32_t kFadeMs = 8;          // fade-in / хвіст fade-out
inline constexpr uint32_t kMuteSettleMs = 10;   // після мʼюту
inline constexpr uint32_t kUnmuteSettleMs = 10; // після анмʼюту
inline constexpr size_t   kChunkFrames = 128;
inline constexpr int      kI2sDmaCount = 8;
inline constexpr int      kI2sDmaLen = 128;     // кадрів на буфер
inline constexpr float    kToneAmpl = 0.25f;

// ---- Потік PCM (BT → AudioOut) ----
inline constexpr size_t   kBtRingBytes = 16384;     // степінь двійки; ≈93 мс при 44,1 кГц стерео
static_assert((kBtRingBytes & (kBtRingBytes - 1)) == 0, "kBtRingBytes має бути степенем двійки");
inline constexpr size_t   kBtPrebufferBytes = 4096; // ≈23 мс перед стартом/після underrun
inline constexpr size_t   kPumpBytes = 1024;        // порція читання кільця
inline constexpr uint32_t kPcmWriteTimeoutMs = 40;  // ліміт блокування AudioOut::write
inline constexpr uint32_t kPumpIdleMs = 10;         // таймаут очікування в задачі audio

// ---- Bluetooth ----
inline constexpr const char* kBtDeviceName = "Media Module";
#ifndef BT_DISCONNECT_ON_DEACTIVATE
#define BT_DISCONNECT_ON_DEACTIVATE 0  // 0: лишати ACL при деактивації (швидке повернення); 1: розривати
#endif
#ifndef BT_RECONNECT_WHILE_INACTIVE
#define BT_RECONNECT_WHILE_INACTIVE 1  // реконект і коли джерело не bt (старт модуля)
#endif
#ifndef BT_OPTIMISTIC_PAUSE
#define BT_OPTIMISTIC_PAUSE 1          // 1: після нашого PAUSE одразу показувати pause (телефон звітує ≈4,3 с)
#endif
inline constexpr uint32_t kBtOptimisticPauseMs = 6000;      // скільки чекаємо підтвердження паузи від телефона
inline constexpr uint8_t  kBtReconnectMaxTries = 5;
inline constexpr uint32_t kBtBootReconnectDelayMs = 2000;   // дати телефону підключитись самому
inline constexpr uint32_t kBtLossReconnectDelayMs = 3000;   // після втрати звʼязку
inline constexpr uint32_t kBtReconnectIntervalMs = 8000;
inline constexpr uint32_t kBtConnectTimeoutMs = 10000;
inline constexpr uint32_t kBtNameWaitMs = 1500;             // очікування імені перед BT,connected
inline constexpr uint32_t kBtMetaDebounceMs = 200;
inline constexpr size_t   kBtMetaMax = 96;                  // байтів UTF-8 на поле метаданих
inline constexpr uint32_t kBtPosNotifS = 2;                 // період AVRCP-повідомлень позиції, с
inline constexpr uint32_t kBtInactivePauseGapMs = 5000;     // мін. інтервал повторного AVRCP pause
inline constexpr uint32_t kPairWindowMs = 120000;           // вікно DBG,bt,pair

// ---- SD (M3a) ----
#ifndef SD_CD_ACTIVE_LOW
#define SD_CD_ACTIVE_LOW 1             // 1: при вставленій картці контакт CD замкнений на GND
#endif
inline constexpr uint8_t  kSdSpiMhz[] = {20, 10, 4};   // сходинки частоти VSPI (відкат при помилках)
inline constexpr uint8_t  kSdSpiSteps = sizeof(kSdSpiMhz);
inline constexpr uint8_t  kSdErrStepDown = 2;          // кожні N помилок підряд — сходинка вниз (2→10, 4→4 МГц)
inline constexpr uint8_t  kSdErrUnmount = 5;           // помилок підряд — відмонтування
inline constexpr uint32_t kSdDebounceMs = 100;         // антидребезг card-detect
inline constexpr uint32_t kSdPollMs = 500;             // опитування присутності, коли SD_CD_PIN < 0
inline constexpr uint8_t  kSdPollFails = 2;            // невдалих проб підряд = картку вийнято
inline constexpr uint32_t kSdRemountMs = 3000;         // пауза між невдалими mount, поки CD каже «картка є»
inline constexpr uint8_t  kSdScanRetries = 2;          // автоповтори скану після помилки читання

inline constexpr size_t   kPlaylistArenaBytes = 16384; // ОДНА статична арена плейлиста (кратна 4, ≤ 65535)
inline constexpr int      kScanMaxDepth = 3;           // корінь = рівень 0; глибші каталоги ігноруються
inline constexpr size_t   kMediaNameMax = 256;         // буфер імені запису (UTF-8 + NUL)
inline constexpr size_t   kScanPathMax = 256;          // буфер шляху під час скану
inline constexpr uint32_t kScanSliceMs = 15;           // порція скану за ітерацію loopTask
inline constexpr uint32_t kReadSliceMs = 15;           // порція DBG,sdread
inline constexpr size_t   kSdReadBuf = 2048;           // статичний буфер DBG,sdread
inline constexpr size_t   kEvFolderNameMax = 48;       // макс. байтів назви папки в події FOLDER
inline constexpr int      kDbgListDefault = 20;        // DBG,list без аргументу

// ---- Задачі ----
inline constexpr int kCoreProto = 0;
inline constexpr int kCoreAudio = 1;
inline constexpr uint32_t kStackLinkTx = 4096;
inline constexpr uint32_t kStackLinkRx = 4096;
inline constexpr uint32_t kStackConsole = 4096;
inline constexpr uint32_t kStackAudio = 4096;
inline constexpr int kPrioLink = 3;
inline constexpr int kPrioConsole = 2;
inline constexpr int kPrioAudio = 6;

// ---- Діагностика ----
inline constexpr uint32_t kDiagPeriodMs = 60000;

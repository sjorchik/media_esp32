#pragma once
#include <stddef.h>
#include <stdint.h>

// ============================================================
//  РОЗПОДІЛ ЗАДАЧ FreeRTOS (документація)
//  Ядро 0 (PRO): BT-стек (контролер + Bluedroid; задачі BTC_TASK/BTU_TASK/BtAppT
//                створює SDK/бібліотека A2DP), link_tx (prio 3), link_rx (prio 3),
//                console (prio 2)
//  Ядро 1 (APP): audio (prio 6; тон, помпа PCM з кільця BT → AudioOut),
//                mp3 (prio 5; декодер minimp3 для SD: читає файл, декодує кадр і пише в AudioOut::write();
//                блокується на notify (Idle/Pause) або на i2s_write (Run); стек статичний, у PcmArena),
//                loopTask «app» (prio 1: виконання всіх команд, SourceManager::loop,
//                BtSource::service, скан SD; під час відтворення SD файл читає лише mp3)
//  SdFat не потокобезпечний: усі виклики SdFat серіалізує мʼютекс у sd_media.cpp; присутність картки під час
//  відтворення визначається по SD_CD_PIN (без SPI).
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
#define STUB_USB_PRESENT 0     // M5c4b/Q1: без CH376S usb=0 (при ENABLE_USB_CH376=1 прапорець веде драйвер); 1 — лише імітація на стенді

// ---- Консоль / лог (UART0) ----
inline constexpr uint32_t kConsoleBaud = 460800;
// M5c5: RX-буфер UART0 (виставляється до begin). Було 512 Б ≈ 11 мс при 460800: відповідь DBG,* блокує консоль на TX
// (≈1,4 мс на рядок) довше, ніж надходить наступна команда пачки → переповнення. 1472 Б = +960 Б (бюджет M5c5 ≤ 1 КБ
// разом із лічильниками консолі ≈ 32 Б). Базовий вимір: збірка з -DCONSOLE_RX_BUF=512.
#ifndef CONSOLE_RX_BUF
#define CONSOLE_RX_BUF 1472
#endif
inline constexpr size_t   kConsoleRxBuf = CONSOLE_RX_BUF;
static_assert(kConsoleRxBuf >= 256 && kConsoleRxBuf <= 2048, "kConsoleRxBuf: 256..2048 Б");
inline constexpr size_t   kConsoleLineMax = 260;
inline constexpr uint8_t  kConsoleRxHighPct = 90;   // M5c5: заповнення RX ≥ цього % ємності = «майже переповнення» (DBG,console)

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

// ---- EQ і цифровий рівень виходу (M6a) ----
// 5-смуговий peaking-EQ (RBJ) і послаблення в PCM до I2S. Порядок: декодування → EQ → рівень → одне насичення → I2S.
namespace eq_cfg {
constexpr uint8_t kBandCount = 5;
constexpr uint16_t kBandFreqHz[kBandCount] = {60, 250, 1000, 4000, 12000};  // за зростанням, інтервал ≈ 2 октави
constexpr int kGainMinDb = -12;       // цілі дБ; 0 = смуга прозора (не обчислюється)
constexpr int kGainMaxDb = 12;
constexpr int kGainStepDb = 1;
constexpr double kQ = 1.0;            // ширина ≈ 1,4 окт.: сусідні смуги плавно перекриваються
constexpr uint32_t kDefaultSampleRateHz = 44100;
constexpr uint32_t kMinSampleRateHz = 8000;    // значення поза межами ігноруються
constexpr uint32_t kMaxSampleRateHz = 96000;
constexpr double kMaxFreqRatio = 0.45;         // смуга з f0 > ratio·fs вимикається (12 кГц мовчить при fs < 26,7 кГц)

static_assert(kBandCount >= 1, "потрібна хоча б одна смуга");
static_assert(kGainMinDb < 0 && kGainMaxDb > 0, "діапазон має містити 0 дБ");
static_assert(kGainMinDb >= -127 && kGainMaxDb <= 127, "підсилення має влізати в int8_t");
static_assert(kGainStepDb >= 1 && (kGainMaxDb - kGainMinDb) % kGainStepDb == 0, "діапазон кратний кроку");
static_assert(kBandFreqHz[0] > 0 && kBandFreqHz[0] < kBandFreqHz[1] && kBandFreqHz[1] < kBandFreqHz[2] &&
                  kBandFreqHz[2] < kBandFreqHz[3] && kBandFreqHz[3] < kBandFreqHz[4],
              "частоти смуг мають зростати");
static_assert(kQ > 0.1 && kQ < 10.0, "Q поза розумним діапазоном");
static_assert(kMinSampleRateHz > 0 && kMinSampleRateHz < kMaxSampleRateHz, "діапазон частот дискретизації");
static_assert(kDefaultSampleRateHz >= kMinSampleRateHz && kDefaultSampleRateHz <= kMaxSampleRateHz, "типова частота в діапазоні");
static_assert(kMaxFreqRatio > 0.0 && kMaxFreqRatio < 0.5, "ratio нижче Найквіста");
}  // namespace eq_cfg

// Цифровий рівень (аналог output_trim на S3): лише послаблення, цілі дБ, три слоти за джерелами.
inline constexpr int      kLevelMinDb = -40;
inline constexpr int      kLevelMaxDb = 0;
inline constexpr int      kLevelDefaultDb = 0;      // 0 дБ = блок не чіпається (поведінка не змінюється)
inline constexpr uint32_t kLevelRampMs = 10;        // рампа на весь діапазон коефіцієнта 0→1
inline constexpr int32_t  kLevelRampFrames = (int32_t)(kDefaultRate * kLevelRampMs / 1000);  // 441 кадр @44,1 кГц
static_assert(kLevelMinDb < kLevelMaxDb && kLevelMaxDb == 0, "рівень лише послаблює: максимум 0 дБ");
static_assert(kLevelDefaultDb >= kLevelMinDb && kLevelDefaultDb <= kLevelMaxDb, "типовий рівень у діапазоні");
static_assert(kLevelRampFrames >= 1, "рампа має бути ≥ 1 кадру");

// ---- Потік PCM (BT → AudioOut) ----
inline constexpr size_t   kBtRingBytes = 16384;     // степінь двійки; ≈93 мс при 44,1 кГц стерео
static_assert((kBtRingBytes & (kBtRingBytes - 1)) == 0, "kBtRingBytes має бути степенем двійки");
inline constexpr size_t   kBtPrebufferBytes = 4096; // ≈23 мс перед стартом/після underrun
inline constexpr size_t   kPumpBytes = 1024;        // порція читання кільця
inline constexpr uint32_t kPcmWriteTimeoutMs = 40;  // ліміт блокування AudioOut::write
inline constexpr uint32_t kPumpIdleMs = 10;         // таймаут очікування в задачі audio

// ---- Спільна область PCM/MP3 (M3b): розкладка і розрахунок — pcm_arena.h, mp3_player.cpp ----
inline constexpr size_t   kDecStackBytes = 20480;   // статичний стек задачі mp3 (scratch minimp3 ≈ 16,3 КБ + запас)
inline constexpr size_t   kDecInBytes = 6144;       // вхідний буфер читання файлу
inline constexpr size_t   kDecInRefill = 3072;      // докачувати, коли лишилось менше (≥ 2 макс. кадрів 2×1441 Б)
inline constexpr size_t   kPcmArenaBytes = 38400;   // ≥ sizeof(SdSet) = 37904 (static_assert у mp3_player.cpp)
static_assert(kPcmArenaBytes >= kBtRingBytes, "область має вміщати BT-кільце");
inline constexpr uint32_t kDmaFrames = (uint32_t)kI2sDmaCount * kI2sDmaLen;  // глибина DMA, кадрів (≈23 мс при 44,1 кГц)
inline constexpr uint32_t kUnderTolUs = 1000;       // допуск оцінки underrun, мкс
inline constexpr uint32_t kDecStopWaitMs = 1500;    // скільки app чекає відповіді задачі mp3 (Stop/Pause)
inline constexpr uint32_t kDecJunkMax = 131072;     // байтів без жодного кадру → decode_fail
inline constexpr uint32_t kDecBadRunMax = 256;      // недекодованих проходів підряд → decode_fail
inline constexpr uint32_t kTimeEventMs = 1000;      // період події TIME під час play

// ---- Bluetooth ----
inline constexpr const char* kBtDeviceName = "Media Module";
// M2b: політика фіксована — BT активний (connectable, реконект) лише при src=bt; в усіх інших станах
// розрив + non-connectable/non-discoverable. Макроси нижче лишено для сумісності й більше не діють.
#ifndef BT_DISCONNECT_ON_DEACTIVATE
#define BT_DISCONNECT_ON_DEACTIVATE 1  // (не використовується)
#endif
#ifndef BT_RECONNECT_WHILE_INACTIVE
#define BT_RECONNECT_WHILE_INACTIVE 0  // (не використовується)
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
inline constexpr uint32_t kBtPauseBeforeDiscMs = 300;       // макс. очікування зупинки потоку після AVRCP pause перед розривом
inline constexpr uint32_t kBtPausePollMs = 20;              // крок опитування під час цього очікування
inline constexpr uint32_t kBtDiscRetryMs = 2000;            // повтор запиту розриву, якщо подія не прийшла
inline constexpr uint8_t  kBtDiscRetryMax = 3;
inline constexpr uint32_t kBtDiscPairMs = 400;              // макс. різниця часу «подія розриву» ↔ «disc_rsn»
inline constexpr uint32_t kBtDiscWaitMs = 600;              // скільки чекаємо disc_rsn (далі — вважаємо втратою звʼязку)
inline constexpr uint32_t kBtSwitchReconnectMs = 500;       // реконект, якщо src=bt повернули, поки йшов наш розрив
inline constexpr uint32_t kBtHeapAfterMs = 1500;            // через скільки після розриву/підключення логувати heap

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
inline constexpr uint32_t kSdLockWaitMs = 1000;        // очікування мʼютекса SdFat у poll()/mount (читання ≤ ~300 мс)
inline constexpr uint32_t kCardLossGraceMs = 400;      // M5c3: збій читання/скану може бути наслідком виймання картки — вердикт відкладається на цей час, поки антидребезг видасть Removed
static_assert(kCardLossGraceMs >= kSdDebounceMs + 100, "kCardLossGraceMs має покривати антидребезг card-detect + очікування мʼютекса");

inline constexpr size_t   kPlaylistArenaBytes = 16384; // ОДНА статична арена плейлиста (кратна 4, ≤ 65535)
inline constexpr int      kScanMaxDepth = 3;           // корінь = рівень 0; глибші каталоги ігноруються
inline constexpr size_t   kMediaNameMax = 256;         // буфер імені запису (UTF-8 + NUL)
inline constexpr size_t   kScanPathMax = 256;          // буфер шляху під час скану
inline constexpr uint32_t kScanSliceMs = 15;           // порція скану за ітерацію loopTask
inline constexpr uint32_t kReadSliceMs = 15;           // порція DBG,sdread
inline constexpr size_t   kEvFolderNameMax = 48;       // макс. байтів назви папки в події FOLDER
inline constexpr size_t   kEvMetaFieldMax = 96;        // макс. байтів назви в META (SD: імʼя файлу без .mp3)
inline constexpr uint32_t kPrevRestartMs = 3000;       // PREV: позиція більша за це → перезапуск поточного треку
inline constexpr uint8_t  kPlayErrStop = 3;            // помилок відтворення підряд → stop (M3c)
inline constexpr uint32_t kPlayOkMs = 1000;            // трек відіграв стільки → лічильник помилок підряд скидається
inline constexpr uint8_t  kNavHist = 8;                // глибина історії переходів (лише DBG,nav)
inline constexpr uint16_t kHistLen = 32;               // кільце останніх відтворених треків (M5a), 2 Б на запис
inline constexpr size_t   kPermBytesPerTrack = 2;      // перестановка shuffle (M5a): uint16_t на трек у вільному хвості арени
inline constexpr uint16_t kShufTestMax = 256;          // DBG,shuftest: макс. N у самоперевірці (буфери на стеку)
inline constexpr uint16_t kShufTestCircles = 20;       // DBG,shuftest: кіл на кожен розмір
inline constexpr uint8_t  kDbgShufHead = 10;           // DBG,shuf: скільки елементів перестановки показувати
inline constexpr uint8_t  kDbgHistTail = 8;            // DBG,shuf: скільки останніх записів історії показувати
inline constexpr uint16_t kRngHealthSamples = 64;      // DBG,shuf: вибірка esp_random()
inline constexpr int      kDbgListDefault = 20;        // DBG,list без аргументу

// ---- ID3-теги (M3d) ----
inline constexpr size_t   kId3FrameBuf = kEvMetaFieldMax * 4;  // читається лише початок кадру (байт кодування + текст)
inline constexpr size_t   kId3WinBytes = 512;          // вікно читання файлу (скретч: буфер кадру + вікно ≤ kDecInBytes)
inline constexpr uint16_t kId3MaxFrames = 48;          // макс. переглянутих кадрів ID3v2
inline constexpr uint32_t kId3MaxReadBytes = 32768;    // макс. байтів, реально прочитаних з носія при розборі v2 (великі кадри пропускаються seek'ом)
inline constexpr uint8_t  kId3Cp1251Pct = 50;          // «схоже на cp1251»: частка байтів ≥0x80 серед не-пробільних, %
inline constexpr bool     kId3C1ForcesCp1251 = true;   // наявність байтів 0x80..0x9F у Latin1-рядку → cp1251
inline constexpr size_t   kId3DbgRaw = 12;             // скільки сирих байтів показує DBG,tag

// ---- Резюме треку й позиції (M5b1) ----
inline constexpr uint32_t kResumeSaveMs = 10000;       // період запису позиції під час play
inline constexpr uint32_t kResumeMinStepBytes = 128;   // періодичний запис лише якщо зсув змінився ≥ цього (≈ один кадр MP3)
inline constexpr uint32_t kResumeTailGuard = 4096;     // зсув ближче до кінця файлу ігнорується (трек з початку)

// ---- Налаштування й захист від петлі аварій (M5b2) ----
inline constexpr uint8_t  kCrashLoopMax = 3;           // аварійних перезапусків підряд → режим захисту на цей запуск
inline constexpr uint32_t kBootStableMs = 60000;       // безперервна робота, після якої лічильник аварій → 0
#ifndef BOOT_TEST_CMDS
#define BOOT_TEST_CMDS 0       // 1: DBG,boot,crash (abort) і DBG,boot,set,<n> для перевірки на залізі; типово 0 (реліз), тести — env esp32dev_test
#endif

// ---- Зависання AudioOut після автопропуску треку (M5c1) ----
#ifndef SKIP_TEST_CMDS
#define SKIP_TEST_CMDS 0       // 1: DBG,failnext,<n>[,read|decode], DBG,media,ins|rem — штучні збої (перевірка автопропуску, hot-plug); типово 0 (реліз), тести — env esp32dev_test
#endif
#ifndef ALWAYS_MUTE_ON_TRACK_START
#define ALWAYS_MUTE_ON_TRACK_START 1   // 1: обхід M3c (мʼют на старті кожного треку); 0: діагностична збірка (-DALWAYS_MUTE_ON_TRACK_START=0)
#endif
inline constexpr bool kAlwaysMuteOnTrackStart = ALWAYS_MUTE_ON_TRACK_START != 0;  // SdSource::startTrack: mute() завжди перед стартом сесії
inline constexpr uint32_t kUnmuteWaitWarnMs = 3000;   // M5c3: Mp3Player::waitUnmuted() чекає розмʼюту довше за це → одне LOGW і лічильник у DBG,aout (лише діагностика)

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
inline constexpr int kPrioDecoder = 5;   // нижче за audio (6), вище за app (1)

// ---- Діагностика ----
inline constexpr uint32_t kDiagPeriodMs = 60000;



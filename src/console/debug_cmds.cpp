#include "console/debug_cmds.h"

#include <stdio.h>
#include <stdlib.h>
#include <strings.h>

#include "audio/audio_out.h"
#include "audio/audio_task.h"
#include "audio/level.h"
#include "audio/mp3_player.h"
#include "core/app.h"
#include "core/device_state.h"
#include "console/console.h"
#include "core/router.h"
#include "media/sd_media.h"
#include "proto/link.h"
#include "storage/storage_dbg.h"
#include "usb/ch376_diag.h"
#include "usb/usb_presence_diag.h"
#include "util/diag.h"
#include "util/heap_phases.h"
#include "util/log.h"

namespace {

constexpr const char* TAG = "dbg";

// M4b4: DBG-команди файлового джерела (nav, shuf, resume, list, play, tag, decode…) адресуються активному SD або USB.
SdSource& fileSrc() {
#if ENABLE_USB_CH376
  if (g_sources.current() == SrcId::USB) return g_usb;
#endif
  return g_sd;
}

bool toInt(const char* s, long& v) {
  if (!s || !*s) return false;
  char* e = nullptr;
  v = strtol(s, &e, 10);
  return *e == 0;
}

void help() {
  LOGI(TAG, "Команди контракту: HELLO,%d GET SRC,<bt|sd|usb|off> PLAY PAUSE TOGGLE STOP NEXT PREV FNEXT FPREV SHUF,<0|1> REP,<0|1|2>", kProtoVer);
  LOGI(TAG, "Також: LVL,<bt|sd|usb>,<-40..0> (proto_ver=%d; показ: DBG,lvl)", kProtoVer);
  LOGI(TAG, "Формат: NAME,арг,… або $BODY*XX");
  LOGI(TAG, "DBG,help | DBG,heap | DBG,heap,phases | DBG,heap,reset | DBG,tasks | DBG,state");
  LOGI(TAG, "DBG,tone[,Гц[,мс]] — тест-тон (лише при src=off), типово 440 Гц 1500 мс");
  LOGI(TAG, "DBG,media — присутність SD, сирий рівень card-detect, лічильники Inserted/Removed (прийнято/дубль/дребезг), state джерела");
#if SKIP_TEST_CMDS
  LOGI(TAG, "DBG,media,ins | DBG,media,rem — імітація події картки через SdMedia::poll (rem = справжнє відмонтування, далі авто-mount)");
#endif
  LOGI(TAG, "DBG,bt — стан BT, кодек, частота, лічильники PCM, метадані");
  LOGI(TAG, "DBG,bt,pair — вікно видимості 120 с | DBG,bt,forget — забути телефон | DBG,bt,reset — скинути лічильники");
  LOGI(TAG, "DBG,sd — присутність, SPI, тип/FS/обсяг | DBG,scan — підсумок останнього скану");
  LOGI(TAG, "DBG,list[,N] — перші N рядків плейлиста (типово %d, 0 = усі) | DBG,sdread,<n> — прочитати трек №n (як у list), КБ/с", kDbgListDefault);
  LOGI(TAG, "DBG,play,<n> — відтворити трек №n (як у list; потрібен src=sd) | DBG,decode — статистика декодера, буфери, heap");
  LOGI(TAG, "DBG,tag,<n> — розібрати ID3 треку №n без відтворення (src=sd, stop): версія, кодування, hex, UTF-8, час");
  LOGI(TAG, "DBG,nav — індекс, папка/діапазон, repeat, shuffle, помилки підряд, історія переходів");
  LOGI(TAG, "DBG,shuf — shuffle: режим, коло k/N, початок перестановки, хвіст історії, якість esp_random() | DBG,shuftest — самоперевірка рушія (OK/FAIL)");
  LOGI(TAG, "DBG,resume — що збережено в NVS для активного SD (шлях, зсув, мс, CRC) і що знайдено в плейлисті | DBG,resume,clear — стерти запис");
  LOGI(TAG, "DBG,resume,tail,<мс> — резюме поточного треку = кінець мінус N мс (потрібен play/pause)");
  LOGI(TAG, "DBG,boot — причина перезапуску, аварійний?, лічильник, режим захисту, AUTO_RESUME_SOURCE | DBG,boot,clear — скинути лічильник");
#if BOOT_TEST_CMDS
  LOGI(TAG, "DBG,boot,crash — abort() (тест аварії) | DBG,boot,set,<0..255> — виставити лічильник (тест)");
#endif
  LOGI(TAG, "DBG,settings — збережені last_src/shuf/rep (CRC) | DBG,settings,clear — стерти set_v1");
  LOGI(TAG, "DBG,link[,reset] — втрати черг, злиті TIME, час відправки кадру (UART1+дзеркало)");
  LOGI(TAG, "DBG,console[,reset] — M5c5: заповнення RX UART0 (cap/peak/high), рядки ok/long, макс. довжина, байти");
  LOGI(TAG, "DBG,lvl — рівень виходу bt/sd/usb і час рівня на кадр | DBG,lvl,<bt|sd|usb>,<-40..0> — послаблення, дБ (лише RAM, після перезапуску 0)");
  LOGI(TAG, "DBG,lvl,reset — скинути лічильники блоків | DBG,heap,phases | DBG,heap,reset — мінімум heap по фазах (M4c1)");
  LOGI(TAG, "DBG,aout — стан AudioOut (мʼют, лічильники, власник мʼютекса) і задачі mp3 (де стоїть); читає без app, працює й при завислому app");
#if ENABLE_USB_CH376
  LOGI(TAG, "DBG,usb,park — ініціалізація (host, 2 Мбод) і паркування: UART2 вимкнено, чіп лишається в host | DBG,usb,wake — UART2 на збереженій швидкості, CHECK_EXIST, GET_STATUS, heap/мкс (M4c2)");
  LOGI(TAG, "DBG,usb,int[,reset] — рівень INT# і спадні фронти з ISR | DBG,usb,pres[,auto|init] — присутність флешки, лічильники; auto — відновити фонову присутність, init — ініціалізувати чіп знову");
  LOGI(TAG, "DBG,usb — стан CH376S | DBG,usb,init — повна послідовність з мкс");
  LOGI(TAG, "DBG,usb,reset — скид і повернення на 9600 | DBG,usb,ping,<N> — N× CHECK_EXIST");
  LOGI(TAG, "DBG,usb,baud — перебір дільників | DBG,usb,baud,<бод>,<K hex> — одна спроба");
  LOGI(TAG, "DBG,usb,init[,<бод>|auto] — з узгодженням швидкості (1000000/460800/115200) | DBG,usb,stop — закрити порт");
  LOGI(TAG, "DBG,usb,ls[,<каталог>] — перелік (імʼя 8.3, тип, розмір; до 64) | DBG,usb,wait[,int|poll] — очікування статусу");
  LOGI(TAG, "DBG,usb,read,<шлях>[,<КБ>] — швидкість читання, блоки, помилки | DBG,usb,seek,<шлях>,<зсув>[,<N>] — seek і читання");
  LOGI(TAG, "DBG,usb,seq,<шлях>[,<КБ>] — читання «як у плеєра» (≈1,1 КБ / 26 мс): чи тримається потік 40 КБ/с");
  LOGI(TAG, "DBG,usb,trace,on|off|dump — кадри UART до/від чіпа | DBG,usb,gap[,<мкс>] — пауза імʼя→open");
  LOGI(TAG, "DBG,usb,fopen,<імʼя> — сирий FILE_OPEN | DBG,usb,cmd,<hex>[,<hex арг>] — довільна команда");
  LOGI(TAG, "DBG,usb,sec,<LBA>[,<блок 0..7>] — сирий сектор (DISK_READ), для розбору розкладки тому");
  LOGI(TAG, "DBG,usb,part — розбір MBR, GPT, BPB, exFAT (M4b2)");
  LOGI(TAG, "DBG,usb,rawread,<LBA>[,<секторів>] — сирі сектори, FNV-1a");
  LOGI(TAG, "DBG,usb,rawspeed,<LBA>,<КБ>[,<N>] — КБ/с сирого читання");
  LOGI(TAG, "DBG,usb,rawseq,<LBA>[+зсув],<КБ>[,<байт>[,<мс>]] — як у плеєра");
  LOGI(TAG, "DBG,usb,rawpre,<LBA>,<КБ>,<пакет Б>,<кадрів> — випереджальне читання");
  LOGI(TAG, "DBG,usb,rawcfg[,settle|skip|wait|spc,<значення>] — налаштування");
  LOGI(TAG, "DBG,usb,mount | DBG,usb,umount — SdFat-том над сирими секторами: швидкість, розділ, ФС, мітка, heap (M4b3)");
  LOGI(TAG, "DBG,usb,vls[,<каталог>[,N]] — перелік з довгими іменами (UTF-8) | DBG,usb,vtag,<шлях> — ID3 (початок і кінець файлу)");
  LOGI(TAG, "DBG,usb,vscan — підсумок скану USB | DBG,usb,vlist[,N] — плейлист USB | DBG,usb,vtagn,<n> — ID3 треку №n плейлиста (src=usb, stop) (M4b4)");
  LOGI(TAG, "DBG,usb,vread,<шлях>[,<КБ>] — швидкість читання через SdFat | DBG,usb,vseq,<шлях>[,<КБ>, 0 = весь файл] — «як у плеєра»");
#endif
#if SKIP_TEST_CMDS
  LOGI(TAG, "DBG,failnext,<n>[,read|decode] — наступні n стартів треку штучно завершуються read_fail (типово) або decode_fail");
#endif
}

const char* mp3St(Mp3Player::St s) { return s == Mp3Player::St::Idle ? "idle" : s == Mp3Player::St::Run ? "run" : "paused"; }

const char* mp3Ev(Mp3Player::Ev e) {
  switch (e) {
    case Mp3Player::Ev::Eof: return "eof";
    case Mp3Player::Ev::ReadFail: return "read_fail";
    case Mp3Player::Ev::DecodeFail: return "decode_fail";
    default: return "-";
  }
}

// M6a: DBG,lvl. Лише атомарні читання/записи, без черги до app. Рядки короткі (буфер логера 192 Б).
const char* const kLvlNames[level::kSlots] = {"bt", "sd", "usb"};

void lvlShow() {
  const int last = level::lastSlot();
  LOGI(TAG, "lvl: bt=%d sd=%d usb=%d дБ | останній активний слот: %s", (int)level::getDb(level::Slot::Bt),
       (int)level::getDb(level::Slot::Sd), (int)level::getDb(level::Slot::Usb),
       last >= 0 && last < (int)level::kSlots ? kLvlNames[last] : "-");
  Mp3Stats st;
  g_sd.dec().stats(st);
  LOGI(TAG, "lvl: час рівня на кадр SD/USB: сер. %u макс. %u мкс (остання сесія; 0 = файлове джерело не грало)", (unsigned)st.lvlAvgUs,
       (unsigned)st.lvlMaxUs);
  LOGI(TAG, "lvl: кадрів з обходом (0 дБ) %u, з них вікно >50 мкс %u (витіснення, не обробка)", (unsigned)st.lvlUnityFrames,
       (unsigned)st.lvlUnitySlow);
  // M4c1: чи справді йде множення. На 0 дБ без рампи mul не росте; якщо lvlAvgUs великий, а mul стоїть — це не вартість обробки.
  const level::Counters cn = level::counters();
  LOGI(TAG, "lvl: блоків без множення %u, з множенням %u (кадрів %u) | DBG,lvl,reset — скинути", (unsigned)cn.unity, (unsigned)cn.mul,
       (unsigned)cn.mulFrames);
}

// DBG,aout (M5c1): лише читання volatile-станів, без черги до app — відповідає й тоді, коли app завис на мʼютексі AudioOut.
void aout() {
  AudioOut::DbgInfo a;
  g_audio.dbgInfo(a);
  Mp3Player::Snap d;
  g_sd.dec().snap(d);
  LOGI(TAG, "aout: ready=%d muted=%d abort=%d | %u Гц %u кан gain=%u%%", a.ready, a.muted, a.aborting, (unsigned)a.rate,
       (unsigned)a.channels, (unsigned)a.gainPct);
  const SdSource& fs = fileSrc();
  LOGI(TAG, "aout: src=%s state=%s | %s=%s errRun=%u/%u failnext=%u(%s)", srcName(g_sources.current()),
       stateName(g_sources.state()), srcName(fs.id()), stateName(fs.playState()), (unsigned)fs.errRun(), (unsigned)kPlayErrStop,
       (unsigned)fs.failLeft(), fs.failIsDecode() ? "decode" : "read");
  LOGI(TAG, "aout: мʼют %u/%u анмʼют %u/%u fmt=%u stop=%u (виклики/реальні переходи)", (unsigned)a.muteCalls,
       (unsigned)a.muteDone, (unsigned)a.unmuteCalls, (unsigned)a.unmuteDone, (unsigned)a.fmtCalls, (unsigned)a.stopCalls);
  if (a.lastTrans) LOGI(TAG, "aout: остання зміна '%c' %u мс тому", a.lastTrans, (unsigned)a.lastTransAgoMs);
  else LOGI(TAG, "aout: змін мʼюту ще не було");
  LOGI(TAG, "aout: write: відкинуто=%u без_мʼютекса=%u неповних=%u", (unsigned)a.wDropped, (unsigned)a.wLockFail,
       (unsigned)a.wPartial);
  LOGI(TAG, "aout: мʼютекс: власник=%s оп='%c' | чекає '%c' %u мс | макс. очікування %u мс", a.owner ? a.owner : "вільний",
       a.ownerOp ? a.ownerOp : '-', a.waitingOp ? a.waitingOp : '-', (unsigned)a.waitingMs, (unsigned)a.lockWaitMaxMs);
  LOGI(TAG, "aout: DMA %u кадрів (≈%u мс), запас за оцінкою mp3 %d мкс", (unsigned)a.dmaFrames,
       (unsigned)(a.dmaFrames * 1000u / (a.rate ? a.rate : kDefaultRate)), (int)d.cushUs);
  LOGI(TAG, "mp3: st=%s req=%u ev=%s | місце: %s, %u мс", mp3St(d.st), (unsigned)d.req, mp3Ev(d.ev),
       Mp3Player::whereName(d.where), (unsigned)d.whereAgoMs);
  LOGI(TAG, "mp3: waitUnmuted довше %u мс: %u разів (лише діагностика)", (unsigned)kUnmuteWaitWarnMs, (unsigned)d.waitLong);
  LOGI(TAG, "mp3: сесій старт=%u eof=%u read=%u dec=%u stop=%u | остання завершена %d мс тому | вхід=%u Б поз=%u мс",
       (unsigned)d.starts, (unsigned)d.endEof, (unsigned)d.endRead, (unsigned)d.endDec, (unsigned)d.endStop,
       d.lastEndAgoMs == 0xFFFFFFFFu ? -1 : (int)d.lastEndAgoMs, (unsigned)d.inFill, (unsigned)d.posMs);
  LOGI(TAG, "aout: збірка: ALWAYS_MUTE_ON_TRACK_START=%d SKIP_TEST_CMDS=%d", (int)kAlwaysMuteOnTrackStart, SKIP_TEST_CMDS);
  Diag::logHeap("aout");
}

// DBG,media (M5c3): лише читання без блокувань і без черги до app.
void media() {
  SdMedia::DbgSnap s;
  SdMedia::dbgSnap(s);
  SdSource::MediaCnt c = g_sd.mediaCnt();
  if (s.cdPin >= 0)
    LOGI(TAG, "media: змонтовано=%d присутність=%d | CD GPIO%d рівень=%d (активний %s) стабільний=%d | заблоковано=%d lost=%d", s.mounted,
         s.present, s.cdPin, s.cdLevel, SD_CD_ACTIVE_LOW ? "LOW" : "HIGH", s.cdStable, s.blocked, s.lostPending);
  else LOGI(TAG, "media: змонтовано=%d присутність=%d | card-detect немає (опитування %u мс) заблоковано=%d lost=%d", s.mounted, s.present,
            (unsigned)kSdPollMs, s.blocked, s.lostPending);
  LOGI(TAG, "media: Inserted прийнято=%u дубль=%u | Removed прийнято=%u дубль=%u | відкинуто дебаунсом=%u (kSdDebounceMs=%u мс)",
       (unsigned)c.insOk, (unsigned)c.insDup, (unsigned)c.remOk, (unsigned)c.remDup, (unsigned)s.bounce, (unsigned)kSdDebounceMs);
  LOGI(TAG, "media: src=%s sd-активне=%d state=%s | імітацій=%u очікує=%u | сторожа(play без сесії)=%u | DeviceState sd=%d usb=%d",
       srcName(g_sources.current()), g_sd.active(), stateName(g_sd.playState()), (unsigned)s.sim, (unsigned)s.simPend, (unsigned)c.guard,
       DeviceState::sd(), DeviceState::usb());
  Diag::logHeap("media");
}

}  // namespace

void DebugCmds::handle(const proto::Msg& m) {
  if (m.argc < 1) { help(); return; }
  const char* c = m.args[0];

  if (!strcasecmp(c, "help")) {
    help();
  } else if (!strcasecmp(c, "heap")) {
    if (m.argc >= 2 && !strcasecmp(m.args[1], "phases")) {  // M4c1: мінімум free/largest по фазах
      HeapPhase::print();
    } else if (m.argc >= 2 && !strcasecmp(m.args[1], "reset")) {
      HeapPhase::reset();
      LOGI(TAG, "heap: лічильники фаз скинуто");
    } else {
      Diag::logHeap("dbg");
    }
  } else if (!strcasecmp(c, "tasks")) {
    Diag::logTasks();
  } else if (!strcasecmp(c, "state")) {
    char nm[33];
    DeviceState::btName(nm, sizeof nm);
    Link::Stats s = Link::stats();
    LOGI(TAG, "src=%s state=%s shuf=%d rep=%u", srcName(g_sources.current()),
         stateName(g_sources.state()), g_sources.shuffle() ? 1 : 0, (unsigned)g_sources.repeat());
    LOGI(TAG, "media sd=%d usb=%d | bt=%d '%s' | dac_muted=%d", DeviceState::sd(), DeviceState::usb(),
         DeviceState::btConnected(), nm, g_audio.isMuted());
    LOGI(TAG, "link good=%u bad=%u dropped=%u partner=%s", (unsigned)s.good, (unsigned)s.bad,
         (unsigned)s.dropped, s.alive ? "так" : "ні");
  } else if (!strcasecmp(c, "tone")) {
    long hz = 440, ms = 1500;
    if ((m.argc >= 2 && (!toInt(m.args[1], hz) || hz < 50 || hz > 8000)) ||
        (m.argc >= 3 && (!toInt(m.args[2], ms) || ms < 100 || ms > 10000))) {
      LOGW(TAG, "DBG,tone[,50..8000[,100..10000]]");
      return;
    }
    if (g_sources.current() != SrcId::Off) { LOGW(TAG, "тон лише при src=off"); return; }
    if (!AudioTask::requestTone((uint16_t)hz, (uint16_t)ms)) LOGW(TAG, "аудіо зайняте");
  } else if (!strcasecmp(c, "media")) {
    if (m.argc == 1) {
      media();
#if SKIP_TEST_CMDS
    } else if (m.argc == 2 && (!strcasecmp(m.args[1], "ins") || !strcasecmp(m.args[1], "rem"))) {
      const bool ins = !strcasecmp(m.args[1], "ins");
      if (SdMedia::dbgInject(ins ? MediaEvent::Inserted : MediaEvent::Removed))
        LOGI(TAG, "media: імітація %s поставлена (виконає app через SdMedia::poll)", ins ? "ins" : "rem");
      else LOGW(TAG, "media: попередня імітація ще не виконана");
#endif
#if !SKIP_TEST_CMDS
    } else if (m.argc == 2 && (!strcasecmp(m.args[1], "ins") || !strcasecmp(m.args[1], "rem"))) {
      LOGW(TAG, "DBG,media,ins|rem вимкнено (SKIP_TEST_CMDS=0)");
#endif
    } else {
#if SKIP_TEST_CMDS
      LOGW(TAG, "DBG,media | DBG,media,ins|rem");
#else
      LOGW(TAG, "DBG,media");
#endif
    }
  } else if (!strcasecmp(c, "bt")) {
    if (m.argc == 1) g_bt.dump();
    else if (!strcasecmp(m.args[1], "pair")) g_bt.requestPair();
    else if (!strcasecmp(m.args[1], "forget")) g_bt.requestForget();
    else if (!strcasecmp(m.args[1], "reset")) g_bt.resetStats();
    else LOGW(TAG, "DBG,bt[,pair|forget|reset]");
  } else if (!strcasecmp(c, "sd") || !strcasecmp(c, "scan") || !strcasecmp(c, "list") || !strcasecmp(c, "sdread") ||
             !strcasecmp(c, "play") || !strcasecmp(c, "decode") ||
             !strcasecmp(c, "nav") || !strcasecmp(c, "tag") || !strcasecmp(c, "shuf") || !strcasecmp(c, "shuftest")) {
    SdSource::Dbg k = !strcasecmp(c, "sd") ? SdSource::Dbg::Info : !strcasecmp(c, "scan") ? SdSource::Dbg::Scan
                      : !strcasecmp(c, "list") ? SdSource::Dbg::List : !strcasecmp(c, "play") ? SdSource::Dbg::Play
                      : !strcasecmp(c, "decode") ? SdSource::Dbg::Decode
                      : !strcasecmp(c, "nav") ? SdSource::Dbg::Nav
                      : !strcasecmp(c, "tag") ? SdSource::Dbg::Tag
                      : !strcasecmp(c, "shuf") ? SdSource::Dbg::Shuf
                      : !strcasecmp(c, "shuftest") ? SdSource::Dbg::ShufTest : SdSource::Dbg::Read;
    long n = (k == SdSource::Dbg::List) ? kDbgListDefault : 0;
    if (k == SdSource::Dbg::List && m.argc >= 2 && (!toInt(m.args[1], n) || n < 0 || n > 65535)) {
      LOGW(TAG, "DBG,list[,0..65535]");
      return;
    }
    if ((k == SdSource::Dbg::Read || k == SdSource::Dbg::Play || k == SdSource::Dbg::Tag) && (m.argc != 2 || !toInt(m.args[1], n) || n < 1 || n > 65535)) {
      LOGW(TAG, "DBG,%s,<n> (n ≥ 1, номер із DBG,list)", k == SdSource::Dbg::Play ? "play" : k == SdSource::Dbg::Tag ? "tag" : "sdread");
      return;
    }
    if (!(k == SdSource::Dbg::Info ? g_sd : fileSrc()).dbg(k, (int)n)) LOGW(TAG, "%s: попередня DBG-команда ще виконується", srcName(fileSrc().id()));
  } else if (!strcasecmp(c, "lvl")) {
    if (m.argc == 1) {
      lvlShow();
      return;
    }
    if (m.argc == 2 && !strcasecmp(m.args[1], "reset")) {  // M4c1: скинути лічильники блоків
      level::resetCounters();
      lvlShow();
      return;
    }
    long db = 0;
    int slot = -1;
    if (m.argc == 3)
      for (int i = 0; i < (int)level::kSlots; i++)
        if (!strcasecmp(m.args[1], kLvlNames[i])) slot = i;
    if (slot < 0 || !toInt(m.args[2], db) || db < kLevelMinDb || db > kLevelMaxDb) {
      LOGW(TAG, "DBG,lvl[,<bt|sd|usb>,<%d..%d>]", kLevelMinDb, kLevelMaxDb);
      return;
    }
    level::setDb((level::Slot)slot, (int8_t)db);
    lvlShow();
  } else if (!strcasecmp(c, "aout")) {
    aout();
  } else if (!strcasecmp(c, "console")) {
    // Виконується в задачі console, тож лічильники не конкурують із записом.
    if (m.argc == 2 && !strcasecmp(m.args[1], "reset")) {
      Console::resetStats();
      LOGI(TAG, "console: лічильники скинуто");
    } else if (m.argc == 1) {
      Console::Stats s = Console::stats();
      // Два рядки: буфер логера 192 Б (кирилиця = 2 Б/символ), один рядок обрізався б.
      LOGI(TAG, "console: RX-буфер cap=%u peak=%u high=%u (≥%u%% ємності)", (unsigned)s.rxCap, (unsigned)s.rxPeak,
           (unsigned)s.rxHigh, (unsigned)kConsoleRxHighPct);
      LOGI(TAG, "console: рядки ok=%u long=%u longB=%u maxlen=%u/%u байтів=%u", (unsigned)s.lines, (unsigned)s.longLines,
           (unsigned)s.longBytes, (unsigned)s.maxLen, (unsigned)(kConsoleLineMax - 1), (unsigned)s.bytes);
    } else {
      LOGW(TAG, "DBG,console[,reset]");
    }
  } else if (!strcasecmp(c, "failnext")) {
#if SKIP_TEST_CMDS
    long n = 0;
    bool dec = false;
    bool ok = m.argc >= 2 && m.argc <= 3 && toInt(m.args[1], n) && n >= 0 && n <= 50;
    if (ok && m.argc == 3) {
      if (!strcasecmp(m.args[2], "decode")) dec = true;
      else if (strcasecmp(m.args[2], "read") != 0) ok = false;
    }
    if (!ok) {
      LOGW(TAG, "DBG,failnext,<0..50>[,read|decode]");
      return;
    }
    if (!fileSrc().dbg(SdSource::Dbg::FailNext, (int)n | (dec ? 0x100 : 0))) LOGW(TAG, "%s: попередня DBG-команда ще виконується", srcName(fileSrc().id()));
#else
    LOGW(TAG, "DBG,failnext вимкнено (SKIP_TEST_CMDS=0)");
#endif
  } else if (!strcasecmp(c, "resume")) {
    if (m.argc == 3 && !strcasecmp(m.args[1], "tail")) {  // M5c6: резюме = кінець треку мінус N мс (крайні випадки без 5 хвилин очікування)
      char* e = nullptr;
      long n = strtol(m.args[2], &e, 10);
      if (!e || *e || n < 0 || n > 3600000L) {
        LOGW(TAG, "DBG,resume,tail,<0..3600000 мс>");
        return;
      }
      if (!fileSrc().dbg(SdSource::Dbg::ResumeTail, (int)n)) LOGW(TAG, "%s: попередня DBG-команда ще виконується", srcName(fileSrc().id()));
      return;
    }
    bool clear = m.argc == 2 && !strcasecmp(m.args[1], "clear");
    if (m.argc > 2 || (m.argc == 2 && !clear)) {
      LOGW(TAG, "DBG,resume[,clear|,tail,<мс>]");
      return;
    }
    if (!fileSrc().dbg(clear ? SdSource::Dbg::ResumeClear : SdSource::Dbg::Resume, 0)) LOGW(TAG, "%s: попередня DBG-команда ще виконується", srcName(fileSrc().id()));
  } else if (!strcasecmp(c, "boot") || !strcasecmp(c, "settings")) {
    // NVS пише лише loopTask: консоль ставить запит у скриньку (storage_dbg)
    const bool isBoot = !strcasecmp(c, "boot");
    StorageDbg::Req req = isBoot ? StorageDbg::Req::Boot : StorageDbg::Req::Settings;
    uint8_t arg = 0;
    bool ok = m.argc == 1;
    if (m.argc == 2 && !strcasecmp(m.args[1], "clear")) {
      req = isBoot ? StorageDbg::Req::BootClear : StorageDbg::Req::SettingsClear;
      ok = true;
#if BOOT_TEST_CMDS
    } else if (isBoot && m.argc == 2 && !strcasecmp(m.args[1], "crash")) {
      req = StorageDbg::Req::BootCrash;
      ok = true;
    } else if (isBoot && m.argc == 3 && !strcasecmp(m.args[1], "set")) {
      long n = 0;
      ok = toInt(m.args[2], n) && n >= 0 && n <= 255;
      req = StorageDbg::Req::BootSet;
      arg = (uint8_t)n;
#endif
    }
#if !BOOT_TEST_CMDS
    if (isBoot && m.argc >= 2 && (!strcasecmp(m.args[1], "crash") || !strcasecmp(m.args[1], "set"))) {
      LOGW(TAG, "DBG,boot,crash|set вимкнено (BOOT_TEST_CMDS=0)");
      return;
    }
#endif
    if (!ok) {
      if (isBoot) LOGW(TAG, "DBG,boot[,clear|crash|set,<0..255>]");
      else LOGW(TAG, "DBG,settings[,clear]");
      return;
    }
    if (!StorageDbg::post(req, arg)) LOGW(TAG, "попередня DBG-команда сховища ще виконується");
  } else if (!strcasecmp(c, "link")) {
    if (m.argc >= 2 && !strcasecmp(m.args[1], "reset")) {
      Link::resetTxStats();
      LOGI(TAG, "лічильники лінку скинуто");
      return;
    }
    Link::Stats s = Link::stats();
    LOGI(TAG, "link: tx=%u втрати hi=%u lo=%u | TIME злито=%u | макс. відправка=%u мкс, з них дзеркало UART0=%u мкс",
         (unsigned)s.txItems, (unsigned)s.lostHi, (unsigned)s.lostLo, (unsigned)s.timeCoalesced,
         (unsigned)s.txMaxUs, (unsigned)s.mirrorMaxUs);
  } else if (!strcasecmp(c, "usb")) {
#if ENABLE_USB_CH376
    // M4b4: vscan / vlist[,N] / vtagn,<n> — підсумок скану, плейлист і ID3 треку №n активного USB-джерела (виконує app)
    const bool vs = m.argc >= 2 && !strcasecmp(m.args[1], "vscan"), vl = m.argc >= 2 && !strcasecmp(m.args[1], "vlist"),
               vt = m.argc >= 2 && !strcasecmp(m.args[1], "vtagn");
    if (vs || vl || vt) {
      long n = vl ? kDbgListDefault : 0;
      if ((vl && m.argc >= 3 && (!toInt(m.args[2], n) || n < 0 || n > 65535)) || (vt && (m.argc != 3 || !toInt(m.args[2], n) || n < 1 || n > 65535)) ||
          (vs && m.argc != 2) || (vl && m.argc > 3)) {
        LOGW(TAG, "DBG,usb,vscan | DBG,usb,vlist[,0..65535] | DBG,usb,vtagn,<n ≥ 1>");
        return;
      }
      if (!g_usb.dbg(vs ? SdSource::Dbg::Scan : vl ? SdSource::Dbg::List : SdSource::Dbg::Tag, (int)n)) LOGW(TAG, "usb: попередня DBG-команда ще виконується");
      return;
    }
    if (UsbPresDiag::handle(m)) return;  // M4c2: park | wake | int | pres
    Ch376Diag::handle(m);
#else
    LOGW(TAG, "DBG,usb вимкнено (ENABLE_USB_CH376=0)");
#endif
  } else {
    LOGW(TAG, "невідома DBG-команда '%s' (DBG,help)", c);
  }
}



#pragma once
#include <stdint.h>

#include "config.h"

#if ENABLE_USB_CH376

#include "usb/usb_chip.h"

// M4c2. Присутність USB-флешки незалежно від активного джерела. Чіп «запаркований» (host, UART2 вимкнено); подію вставлення/
// виймання дає INT# (режим 1) або опитування (режим 2). Автомат робить малі кроки (один tick = одна коротка дія) і сам нічого не
// знає про Arduino: чіп — IUsbChip, час і умови (BT, heap) — UsbPresEnv. Чистий C++, тестується в native.
enum class UsbPres : uint8_t { Unknown, Absent, Present, Unsupported };
const char* usbPresName(UsbPres p);

struct UsbPresEnv {
  uint32_t now = 0;
  bool defer = false;     // BT активне і heap малий: обмін з чіпом відкладено (подія не губиться: INT# лишається LOW)
  bool portBusy = false;  // порт чіпа відкрито не нами (сесія USB або консоль): не втручаємось
};

class UsbPresenceCore {
 public:
  UsbPresenceCore(IUsbChip& chip, uint8_t mode) : c_(chip), mode_(mode) {}

  void begin(uint32_t now);              // запланувати фонову ініціалізацію (через kUsbPresInitDelayMs)
  void tick(const UsbPresEnv& env);      // не більше одного короткого кроку
  UsbPres state() const { return st_; }
  UsbPres stateForOpen();                // Unknown, поки подія чекає/обробляється (стан міг застаріти)
  bool busy() const { return ph_ != Ph::Idle; }  // триває послідовність: порт відкрито нами
  bool needInit() const { return needInit_; }

  // Від USB-сесії (UsbVolume): чіпом володіє вона, поки том відкривається/відкритий.
  void yield() { toIdle(); }             // перед відкриттям тому: порт, відкритий нами, віддати (паркування)
  void sessionOpened() { needInit_ = false; setState(UsbPres::Present); }
  void openFailed(UsbOpenFail f);
  void volumeLost() { setState(UsbPres::Absent); }
  void sessionClosed(bool recheck);      // чіп запарковано; recheck — флешку могли вийняти непоміченою
  void invalidate(uint32_t now);         // чіп скинуто ззовні (DBG,usb,stop/init): потрібна нова ініціалізація
  void restartInit(uint32_t now);        // DBG,usb,pres,init: скинути лічильники спроб і ініціалізувати зараз

  // Прапорець MEDIA usb (= state()==Present) змінився від останнього виклику.
  bool takeChange(bool& flag);

  // Лічильники для DBG,usb,pres.
  struct Stats { uint16_t events, probes, retries, defers, inits, initFails; uint32_t lastMs; };
  const Stats& stats() const { return stat_; }
  const char* lastFail() const { return failWhat_; }   // на якому кроці востаннє зірвалась ініціалізація
  Ch376Err lastFailErr() const { return failErr_; }
  uint32_t lastFailReinitMs() const { return failRetryMs_; }

 private:
  enum class Ph : uint8_t {
    Idle, InitWake, InitReset, InitResetWait, InitBaud, InitHost, InitSettle,
    Settle, Wake, Status, DiskConn, DiskConnWait, Mount, MountWait, Probe
  };
  void setState(UsbPres s);
  bool pending();
  void toIdle();                       // порт закрити (паркування), перейти в Idle
  void retry(uint32_t now);            // збій обміну: повтор або «чіп втрачено»
  void initFail(uint32_t now, const char* what, Ch376Err e);
  void verdict(UsbPres s);
  bool poll(uint32_t now, uint32_t tmoMs, uint8_t& ev, bool& timedOut);

  IUsbChip& c_;
  uint8_t mode_;
  UsbPres st_ = UsbPres::Unknown;
  bool published_ = false;
  Ph ph_ = Ph::Idle;
  bool needInit_ = false, recheck_ = false, force_ = false, ownPort_ = false, gaveUp_ = false;
  uint8_t retry_ = 0, initTries_ = 0, connTries_ = 0;
  uint32_t t_ = 0, t0_ = 0, next_ = 0, initAt_ = 0, pollAt_ = 0, reinitMs_ = kUsbPresReinitMs, startedAt_ = 0;
  const char* failWhat_ = "";
  Ch376Err failErr_ = Ch376Err::Ok;
  uint32_t failRetryMs_ = 0;
  Stats stat_{};
};

#endif  // ENABLE_USB_CH376

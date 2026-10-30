#pragma once
#include <stddef.h>
#include <stdint.h>

#include "config.h"
#include "usb/ch376_link.h"

// Файловий шар CH376S (M4b1). Чистий C++ без Arduino.h: працює через Ch376Link, тестується в native.
// Лише один відкритий файл. Імена тільки 8.3 (довгих чіп не підтримує). Драйвер (скид, host, mount) цей шар сам не запускає.
// Усі виклики — з однієї задачі (console під час діагностики; потім задача плеєра).

// Результат операції. Ok — успіх; решта — конкретна причина. Сирий код статусу чіпа — lastStatus().
enum class FsErr : uint8_t {
  Ok,
  NotFound,      // ERR_MISS_FILE
  IsDir,         // відкрито каталог, а потрібен файл (повертає читач)
  NotDir,        // елемент шляху — файл, а потрібен каталог
  Disconnected,  // флешку витягнуто (USB_INT_DISCONNECT / ERR_DISK_DISCON)
  ReadError,     // помилка диска (USB_INT_DISK_ERR, переповнення буфера)
  Timeout,       // статус або дані не прийшли вчасно
  NoReply,       // чіп не відповів на GET_STATUS / GET_FILE_SIZE
  BadReply,      // відповідь неможлива (довжина блоку > 64, 0 байтів даних тощо)
  BadArg,        // порожній вказівник, надто великий аргумент
  BadName,       // елемент шляху не є коректним 8.3 або шлях задовгий
  NotOpen,       // операція над файлом без відкритого файлу
  Aborted,       // link попросив перервати
  ChipStatus,    // чіп повернув невідомий статус (код — lastStatus())
};

const char* ch376FsErrName(FsErr e);

enum class FsKind : uint8_t { None, File, Dir };

// Запис каталогу для переліку.
struct Ch376DirEntry {
  char name[kCh376FsNameMax + 1];  // «NAME.EXT», ASCII
  uint8_t attr;                    // атрибути FAT
  uint32_t size;
  bool isDir() const { return (attr & kCh376AttrDir) != 0; }
};

// Колбек переліку: false — перервати. Викликається під час очікування чіпа (ENUM_GO ще не надіслано).
typedef bool (*Ch376DirCb)(const Ch376DirEntry& e, void* ctx);

// Лічильники для DBG,usb,read|seq. Час блоку — від відправки BYTE_READ/BYTE_RD_GO до прийому даних блоку (до 255 Б).
struct Ch376FsStats {
  uint32_t blocks = 0, bytes = 0;
  uint32_t blkMinUs = 0xFFFFFFFFu, blkMaxUs = 0, blkSumUs = 0;
  uint32_t waitUs = 0;    // сума очікування події (INT або опитування) до останнього GET_STATUS
  uint32_t statUs = 0;    // сума часу останніх GET_STATUS (обмін по UART)
  uint32_t xferUs = 0;    // сума часу RD_USB_DATA0 (команда + 64 Б)
  uint32_t polls = 0;     // GET_STATUS, що повернули «події ще немає» (повтори)
  uint32_t intWaits = 0;  // очікувань, завершених за INT
  uint32_t intMisses = 0; // INT не з'явився за kCh376FsIntSliceMs: запасний GET_STATUS
  uint32_t errors = 0;    // операції читання/seek, що завершились помилкою
  uint32_t retries = 0;   // повтори читачем після збою (reopen + seek)
};

class Ch376Fs {
 public:
  explicit Ch376Fs(Ch376Link& link) : link_(link) {}

  // Скинути стан (після init/stop): файл закрито, режим очікування за замовчуванням, лічильники нуль.
  void reset();
  void setUseInt(bool on) { useInt_ = on; }
  bool useInt() const { return useInt_; }
  bool intUsable() const { return link_.hasIntPin(); }
  // Пауза між SET_FILE_NAME і FILE_OPEN, мкс (діагностика: чіп може не встигати прийняти імʼя на 1 Мбод). Типово 0.
  void setGapUs(uint32_t us) { gapUs_ = us; }
  uint32_t gapUs() const { return gapUs_; }
  uint8_t lastLenByte() const { return lastLen_; }  // байт довжини останньої відповіді RD_USB_DATA0 у rawCmd (діагностика)
  bool fileOpen() const { return open_; }
  uint8_t lastStatus() const { return lastSt_; }
  const Ch376FsStats& stats() const { return st_; }
  void resetStats() { st_ = Ch376FsStats{}; }
  void noteRetry() { st_.retries++; }

  // Відкрити шлях від кореня, поетапно (SET_FILE_NAME + FILE_OPEN на кожен елемент). Спершу закриває відкритий файл.
  // Шлях спочатку перевіряється ЦІЛКОМ (чіпа не торкаємось, якщо імʼя некоректне). Великі літери виставляє шар.
  // Файл → kind=File (відкрито). Каталог (також "/") → kind=Dir, поточний каталог чіпа змінено.
  FsErr openPath(const char* path, FsKind& kind);
  FsErr fileSize(uint32_t& size);                 // GET_FILE_SIZE відкритого файлу
  FsErr locate(uint32_t off);                     // BYTE_LOCATE
  // Читає до n байтів блоками (до 255 Б); dst == nullptr — дані приймаються й відкидаються (вимірювання).
  // got < n при Ok означає кінець файлу. Після помилки файл НЕ закрито автоматично, окрім Disconnected.
  FsErr read(void* dst, size_t n, size_t& got);
  FsErr close();                                  // FILE_CLOSE; стан «закрито» ставиться в будь-якому разі
  void abort();                                   // best-effort: скинути RX, FILE_CLOSE без перевірок, стан «закрито»
  // Перелік каталогу (імʼя 8.3, атрибути, розмір): без «.», «..», мітки тому, довгих записів. Не більше maxN записів,
  // more=true, якщо записів більше. Файл під час переліку має бути закритий.
  FsErr list(const char* dirPath, Ch376DirCb cb, void* ctx, uint16_t maxN, uint16_t& count, bool& more);

  // Діагностика: SET_FILE_NAME + FILE_OPEN з імʼям «як є» (лише великі літери, ≤ 12 знаків, шлях не розбирається).
  // st — статус чіпа; якщо 0x1D, у entry[32] лежить запис каталогу (hasEntry). Відкритий файл (0x14) закривається викликом close().
  FsErr rawOpen(const char* name, uint8_t& st, uint8_t* entry, bool& hasEntry);

  // Діагностика: довільна команда з аргументами, статус чіпа і (якщо чіп віддає) дані RD_USB_DATA0 (до 64 Б у data).
  FsErr rawCmd(uint8_t cmd, const uint8_t* a, size_t na, uint8_t& st, uint8_t* data, size_t& dlen);

  // Діагностика: блок 0..7 (по 64 Б) сектора lba через DISK_READ; сектор вичитується до кінця (8 блоків), у out — потрібний блок.
  FsErr rawSector(uint32_t lba, uint8_t block, uint8_t* out, size_t& len);

  // Чисті допоміжні (для тестів): перевірка елемента 8.3 («NAME.EXT», без * ? пробілів) і мапінг статусу чіпа.
  static bool valid83(const char* s, size_t n);
  static FsErr mapStatus(uint8_t st);

 private:
  FsErr send(uint8_t cmd, const uint8_t* a, size_t na);
  FsErr sendName(const char* s, size_t n, bool root);
  FsErr getStatus(uint8_t& st);
  FsErr waitStatus(uint8_t& st, uint32_t tmoMs);
  FsErr readData(uint8_t* dst, size_t cap, size_t& len);  // RD_USB_DATA0 → dst (nullptr — відкинути); len ≤ cap
  FsErr openStep(const char* el, size_t n, bool root, bool last, FsKind& kind);
  FsErr failRead(FsErr e);
  void noteBlock(uint32_t t0, uint32_t t1, uint32_t t2);

  Ch376Link& link_;
  bool useInt_ = kCh376FsUseInt;
  uint32_t gapUs_ = 0;
  uint8_t lastLen_ = 0;
  bool open_ = false;
  uint8_t lastSt_ = 0;
  Ch376FsStats st_;
  uint32_t tWaitUs_ = 0, tStatUs_ = 0;           // розбивка останнього waitStatus()
  uint8_t blk_[kCh376StageMax];                  // буфер службових блоків (запис каталогу 32 Б, сектор по 64 Б)
};

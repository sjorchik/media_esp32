#pragma once
#include <stddef.h>
#include <stdint.h>

#include "config.h"
#include "media/ifile_reader.h"
#include "usb/ch376_fs.h"

// IFileReader для USB-флешки через CH376S (M4b1). Імена лише 8.3, шлях від кореня: /A/B/FILE.MP3.
// Один відкритий файл. Семантика як у SdFileReader: read() блокує до n байтів, менше — лише в кінці файлу чи при помилці
// (тоді failed()). Після зникнення флешки стан чистий: isOpen() == false, failed() == true, read() одразу повертає 0.
// Збій читання/seek (не відключення) → до kCh376FsReadRetries повторів: abort, повторне відкриття й seek на останню добру позицію.
// Розширення: read(nullptr, n) приймає й відкидає дані (вимірювання пропускної здатності, DBG,usb,read).
// Драйвер (скид, host, mount) читач сам не запускає.
class Ch376FileReader final : public IFileReader {
 public:
  explicit Ch376FileReader(Ch376Fs& fs) : fs_(fs) {}

  bool open(const char* path) override;
  size_t read(void* dst, size_t n) override;
  bool seek(uint32_t pos) override;
  uint32_t size() const override { return size_; }
  uint32_t pos() const override { return pos_; }
  bool isOpen() const override { return open_ && fs_.fileOpen(); }
  bool failed() const override { return failed_; }
  void close() override;

  FsErr lastErr() const { return err_; }   // остання помилка open/read/seek (Ok після успіху open)
  void reset();                            // скинути стан без звернення до чіпа (після init/stop)

 private:
  bool recoverTo(uint32_t pos);            // abort → відкрити path_ → seek(pos)
  void failClose(FsErr e);

  Ch376Fs& fs_;
  uint32_t pos_ = 0, size_ = 0;
  bool open_ = false, failed_ = false;
  FsErr err_ = FsErr::Ok;
  char path_[kCh376FsPathMax] = {0};
};

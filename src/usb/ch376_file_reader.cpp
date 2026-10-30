#include "config.h"

#if ENABLE_USB_CH376

#include "usb/ch376_file_reader.h"

#include <string.h>

void Ch376FileReader::reset() {
  pos_ = size_ = 0;
  open_ = failed_ = false;
  err_ = FsErr::Ok;
  path_[0] = 0;
}

void Ch376FileReader::close() {
  if (open_) fs_.close();
  open_ = false;
}

// Збій: носій мовчить або відключено — файл закрито, прапорець failed().
void Ch376FileReader::failClose(FsErr e) {
  err_ = e;
  failed_ = true;
  fs_.abort();
  open_ = false;
}

bool Ch376FileReader::open(const char* path) {
  close();
  failed_ = false;
  pos_ = size_ = 0;
  err_ = FsErr::Ok;
  if (!path || strlen(path) >= kCh376FsPathMax) {
    err_ = FsErr::BadName;
    failed_ = true;
    return false;
  }
  FsKind k = FsKind::None;
  FsErr e = fs_.openPath(path, k);
  if (e == FsErr::Ok && k != FsKind::File) e = FsErr::IsDir;
  uint32_t sz = 0;
  if (e == FsErr::Ok) e = fs_.fileSize(sz);
  if (e != FsErr::Ok) {
    err_ = e;
    failed_ = true;
    if (fs_.fileOpen()) fs_.abort();
    return false;
  }
  strcpy(path_, path);
  size_ = sz;
  open_ = true;
  return true;
}

bool Ch376FileReader::recoverTo(uint32_t pos) {
  fs_.abort();
  FsKind k = FsKind::None;
  FsErr e = fs_.openPath(path_, k);
  if (e != FsErr::Ok || k != FsKind::File) {
    err_ = e == FsErr::Ok ? FsErr::IsDir : e;
    return false;
  }
  uint32_t sz = 0;
  e = fs_.fileSize(sz);
  if (e != FsErr::Ok || sz != size_) {  // файл підмінено або чіп мовчить
    err_ = e == FsErr::Ok ? FsErr::BadReply : e;
    return false;
  }
  if (pos && (e = fs_.locate(pos)) != FsErr::Ok) {
    err_ = e;
    return false;
  }
  return true;
}

size_t Ch376FileReader::read(void* dst, size_t n) {
  if (!open_ || failed_ || !fs_.fileOpen()) {  // носій зник або файл не відкрито: без очікувань
    failed_ = true;
    return 0;
  }
  if (n > size_ - pos_) n = size_ - pos_;  // за кінець файлу не просимо
  uint8_t* d = static_cast<uint8_t*>(dst);
  size_t total = 0;
  uint8_t tries = 0;
  while (total < n) {
    size_t got = 0;
    const size_t asked = n - total;
    FsErr e = fs_.read(d ? d + total : nullptr, asked, got);
    total += got;
    pos_ += (uint32_t)got;
    if (e == FsErr::Ok && (got == asked || pos_ >= size_)) break;
    if (e == FsErr::Ok) e = FsErr::ReadError;  // чіп віддав менше, ніж лишалось у файлі
    if (e == FsErr::Disconnected || e == FsErr::Aborted || ++tries > kCh376FsReadRetries) {
      failClose(e);
      return total;
    }
    fs_.noteRetry();
    if (!recoverTo(pos_)) {
      failClose(err_);
      return total;
    }
  }
  return total;
}

bool Ch376FileReader::seek(uint32_t p) {
  if (!open_ || failed_ || !fs_.fileOpen()) {
    failed_ = true;
    return false;
  }
  if (p > size_) return false;  // як SdFat::seekSet: за межі файлу — false, без збою
  uint8_t tries = 0;
  for (;;) {
    FsErr e = fs_.locate(p);
    if (e == FsErr::Ok) {
      pos_ = p;
      return true;
    }
    if (e == FsErr::Disconnected || e == FsErr::Aborted || ++tries > kCh376FsReadRetries) {
      failClose(e);
      return false;
    }
    fs_.noteRetry();
    if (!recoverTo(pos_)) {
      failClose(err_);
      return false;
    }
  }
}

#endif  // ENABLE_USB_CH376

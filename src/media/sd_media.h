#pragma once
#include "media/imedia.h"

// Реалізація IMedia/IFileReader для SD на SdFat. Увесь SdFat — лише в sd_media.cpp
// (стан у файлових статиках: один слот SD, один читач). Заголовок легкий.
class SdFileReader final : public IFileReader {
 public:
  bool open(const char* path) override;
  size_t read(void* dst, size_t n) override;
  bool seek(uint32_t pos) override;
  uint32_t size() const override { return size_; }
  uint32_t pos() const override { return pos_; }
  bool isOpen() const override;
  bool failed() const override { return failed_; }
  void close() override;

 private:
  uint32_t pos_ = 0, size_ = 0;
  bool failed_ = false;
};

class SdMedia final : public IMedia {
 public:
  bool begin() override;
  MediaEvent poll(uint32_t nowMs) override;
  bool mount() override;
  void unmount() override;
  bool mounted() const override;
  bool present() const override;
  int openDir(const char* path) override;
  int nextEntry(int h, MediaEntry& e) override;
  void closeDir(int h) override;
  IFileReader& reader() override { return reader_; }
  void info(MediaInfo& out) override;

 private:
  SdFileReader reader_;
};

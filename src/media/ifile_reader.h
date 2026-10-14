#pragma once
#include <stddef.h>
#include <stdint.h>

// Читання файлу з носія. Не залежить від типу носія (SD зараз, USB пізніше).
// Один споживач за раз: app (скан/DBG) або, під час відтворення, задача mp3 (SdSource передає файл після open()).
// Реалізація SD серіалізує SdFat мʼютексом. Після зникнення носія read() одразу повертає 0 (без очікувань).
class IFileReader {
 public:
  virtual ~IFileReader() {}
  virtual bool open(const char* path) = 0;       // повний шлях від кореня, UTF-8; закриває попередній файл
  virtual size_t read(void* dst, size_t n) = 0;  // прочитано байтів; 0 = кінець файлу або помилка (див. failed())
  virtual bool seek(uint32_t pos) = 0;
  virtual uint32_t size() const = 0;
  virtual uint32_t pos() const = 0;
  virtual bool isOpen() const = 0;
  virtual bool failed() const = 0;               // була помилка вводу/виводу або носій зник
  virtual void close() = 0;
};


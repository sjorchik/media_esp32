#pragma once
#include <stddef.h>
#include <stdint.h>

#include "config.h"

// Плейлист у одній статичній арені (kPlaylistArenaBytes). Лише з задачі app.
// Розкладка: знизу вгору — пул рядків (імʼя папки, далі імена її треків без ".mp3", кожне з NUL);
// зверху вниз — записи папок по 8 байт. Папка = суцільний діапазон треків (скан додає
// файли каталогу до його підкаталогів). Памʼять на трек = довжина імені + 1 + kPermBytesPerTrack:
// 2 Б на трек резервуються під перестановку shuffle (M5a) — вона лежить у вільному хвості арени
// (біля записів папок, див. permBuf()), тому нової статичної RAM не потрібно.
class Playlist {
 public:
  static constexpr uint16_t kNone = 0xFFFF;

  void clear();
  int addFolder(uint16_t parent, const char* name, size_t len);  // індекс або -1 (немає місця)
  bool addTrack(const char* stem, size_t len);                   // у останню додану папку; false = немає місця

  uint16_t tracks() const { return nTracks_; }
  uint16_t folders() const { return nFolders_; }
  size_t used() const { return pool_ + (size_t)nFolders_ * 8 + (size_t)nTracks_ * kPermBytesPerTrack; }  // з резервом
  static constexpr size_t capacity() { return kPlaylistArenaBytes; }

  int folderOf(uint16_t track) const;                          // індекс папки або -1
  uint16_t folderFirst(int f) const;
  uint16_t folderTracks(int f) const;
  bool folderName(int f, char* out, size_t cap) const;         // остання компонента; корінь = "/"
  bool folderPath(int f, char* out, size_t cap) const;         // "/" або "/A/B"
  bool trackName(uint16_t i, char* out, size_t cap) const;     // без розширення
  bool trackPath(uint16_t i, char* out, size_t cap) const;     // "/A/B/імʼя.mp3"

  // Буфер перестановки на tracks() елементів (uint16_t) у зарезервованому хвості арени.
  // Чинний лише після завершення скану (розташування залежить від кількості папок і треків); nullptr, якщо треків немає.
  uint16_t* permBuf();

 private:
  size_t freeBytes() const;
  uint16_t nTracks_ = 0, nFolders_ = 0;
  size_t pool_ = 0;
};


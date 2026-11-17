#pragma once
#include <stdint.h>

// M4b4. Правила життя USB-носія всередині SdSource (профіль ownsMount): коли том відкрито, коли прапорець MEDIA usb = 1,
// коли драйвер і heap звільняються. Чистий автомат без заліза: SdSource виконує те, що повертає крок. Лише з задачі app.
//   • прапорець MEDIA usb = 1 тільки коли том змонтовано І у плейлисті є треки (скан завершено успішно);
//   • порожній носій, збій скану, виймання, вихід з джерела → прапорець 0 і том закрито.
struct MediaStep {
  bool flagChanged = false;  // прапорець MEDIA usb змінився (якщо подію шле не менеджер — надіслати MEDIA)
  bool flag = false;         // новий прапорець
  bool release = false;      // закрити том і драйвер (UsbVolume::close)
};

class UsbMediaPolicy {
 public:
  bool flag() const { return flag_; }
  bool open() const { return open_; }

  void mountResult(bool ok) { open_ = ok; }  // результат UsbMedia::mount()

  // Скан завершено: failed — остаточний збій; tracks — скільки треків у плейлисті (Overflow теж дає треки).
  MediaStep scanDone(uint16_t tracks, bool failed) {
    MediaStep s;
    if (!failed && tracks > 0) {
      s.flagChanged = !flag_;
      flag_ = true;
      s.flag = true;
      return s;
    }
    return drop();
  }

  // Носій зник (Removed від UsbMedia::poll). Повторний виклик — без змін (дубль не дає другої події).
  MediaStep removed() { return drop(); }

  // Вихід із джерела (SRC,<інше>/off): прапорець 0, том закрито.
  MediaStep deactivate() { return drop(); }

 private:
  MediaStep drop() {
    MediaStep s;
    s.flagChanged = flag_;
    flag_ = false;
    s.flag = false;
    s.release = open_;
    open_ = false;
    return s;
  }
  bool flag_ = false, open_ = false;
};

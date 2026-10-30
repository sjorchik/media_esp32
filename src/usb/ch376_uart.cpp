#include "usb/ch376_uart.h"

#if ENABLE_USB_CH376

#include <Arduino.h>

#include "driver/uart.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include <stdio.h>
#include <stdlib.h>

#include "pins.h"
#include "util/log.h"

namespace {

SemaphoreHandle_t s_sem = nullptr;  // сигнал від ISR на INT# (двійковий)
uint32_t s_sleepAt = 0;             // коли востаннє віддавали процесор на 1 тік у спіні

// Трасування: запис = байт | 0x100 («до чіпа», T) | 0x200 (відкинутий flushRx, F); без прапорців — прийнятий (R).
constexpr size_t kTraceCap = 384;
uint16_t* s_tr = nullptr;
size_t s_trN = 0, s_trLost = 0;
inline void trPush(uint16_t v) {
  if (!s_tr) return;
  if (s_trN < kTraceCap) s_tr[s_trN++] = v;
  else s_trLost++;
}

// INT# CH376S: активний LOW, тримається до GET_STATUS. Фронт falling лише будить задачу; істина — рівень піна.
void IRAM_ATTR intIsr() {
  BaseType_t w = pdFALSE;
  if (s_sem) xSemaphoreGiveFromISR(s_sem, &w);
  if (w == pdTRUE) portYIELD_FROM_ISR();
}

// Спін без delay(1) (M4b1): очікування байта чи події йде в циклі за micros(). Щоб idle-задача ядра 0 не голодувала
// (task WDT), раз на kCh376SpinSleepUs віддаємо процесор на один тік.
inline void spinPause() {
  if ((uint32_t)(micros() - s_sleepAt) >= kCh376SpinSleepUs) {
    vTaskDelay(1);
    s_sleepAt = micros();
  }
}

// Реалізація Ch376Link на Serial2. Тайм-аути — за micros().
class UartLink : public Ch376Link {
 public:
  // flush() чекає, поки байти вийдуть з TX: інакше наступна зміна швидкості обрізає кадр.
  void write(const uint8_t* d, size_t n) override {
    if (s_tr)
      for (size_t i = 0; i < n; i++) trPush((uint16_t)(d[i] | 0x100));
    Serial2.write(d, n);
    Serial2.flush();
  }

  bool readByte(uint8_t& b, uint32_t tmoMs) override {
    const uint32_t t0 = micros();
    const uint32_t lim = tmoMs * 1000u;
    for (;;) {
      const int c = Serial2.read();
      if (c >= 0) {
        b = (uint8_t)c;
        trPush((uint16_t)c);
        return true;
      }
      if ((uint32_t)(micros() - t0) >= lim) return false;
      spinPause();
    }
  }

  // Тайм-аут бездіяльності: відлік заново після кожного прийому. Блок читається одним викликом uart_read_bytes (драйвер IDF),
  // а не Serial2.read() по одному байту: кожен такий виклик бере мьютекс і коштує десятки мкс (на 255 Б це було б ≈ 5 мс).
  size_t readBytes(uint8_t* dst, size_t n, uint32_t tmoUs) override {
    size_t k = 0;
    uint32_t t0 = micros();
    while (k < n) {
      const int got = uart_read_bytes(UART_NUM_2, dst + k, (uint32_t)(n - k), 0);  // без очікування: віддає, що вже є
      if (got > 0) {
        if (s_tr)
          for (int i = 0; i < got; i++) trPush((uint16_t)dst[k + (size_t)i]);
        k += (size_t)got;
        t0 = micros();
        continue;
      }
      if ((uint32_t)(micros() - t0) >= tmoUs) break;
      spinPause();
    }
    return k;
  }

  void flushRx() override {
    if (s_tr) {  // під час трасування відкинуті байти записуються (F)
      while (Serial2.available() > 0) {
        const int c = Serial2.read();
        if (c >= 0) trPush((uint16_t)(c | 0x200));
      }
      return;
    }
    uart_flush_input(UART_NUM_2);
  }

  // Лише порт; чіп перемикає SET_BAUDRATE окремо.
  bool setBaud(uint32_t baud) override {
    Serial2.updateBaudRate(baud);
    baud_ = baud;
    return true;
  }
  void setBaudValue(uint32_t baud) { baud_ = baud; }  // без зміни порту (begin/stop)
  uint32_t byteTimeUs() const override { return (10000000u + baud_ - 1) / baud_; }

  bool hasIntPin() const override { return CH_INT_PIN >= 0; }
  bool intLow() const override { return digitalRead(CH_INT_PIN) == LOW; }

  // Спершу скидаємо застарілий сигнал семафора, потім дивимось рівень піна: подія між цими кроками дає LOW (повертаємось),
  // а після перевірки — новий сигнал від ISR. Тож пробудження не губиться.
  bool waitIntLow(uint32_t tmoUs) override {
    if (CH_INT_PIN < 0) return false;
    if (s_sem) xSemaphoreTake(s_sem, 0);
    if (digitalRead(CH_INT_PIN) == LOW) return true;
    const uint32_t t0 = micros();
    for (;;) {
      const uint32_t el = (uint32_t)(micros() - t0);
      if (el >= tmoUs) return digitalRead(CH_INT_PIN) == LOW;
      if (s_sem) {
        TickType_t ticks = pdMS_TO_TICKS((tmoUs - el + 999u) / 1000u);
        if (ticks == 0) ticks = 1;
        xSemaphoreTake(s_sem, ticks);
      } else {
        spinPause();  // семафора немає (не вистачило памʼяті): опитування піна
      }
      if (digitalRead(CH_INT_PIN) == LOW) return true;
    }
  }

  void yieldNow() override {
    spinPause();
    yield();
  }

  // Апаратний RST у платі не виведено (CH_RST_PIN = -1): відкат програмний, тож false.
  bool hwReset() override {
    if (CH_RST_PIN < 0) return false;
    digitalWrite(CH_RST_PIN, kCh376RstActiveHigh ? HIGH : LOW);
    delay(kCh376RstPulseMs);
    digitalWrite(CH_RST_PIN, kCh376RstActiveHigh ? LOW : HIGH);
    return true;
  }

  bool abortRequested() const override { return false; }
  void delayMs(uint32_t ms) override { delay(ms); }
  uint32_t nowUs() const override { return (uint32_t)micros(); }

 private:
  uint32_t baud_ = kCh376DefaultBaud;
};

UartLink s_link;
Ch376 s_drv(s_link);
Ch376Fs s_fs(s_link);
Ch376FileReader s_rd(s_fs);
bool s_started = false;

}  // namespace

namespace ch376_uart {

bool trace(bool on) {
  if (!on) {
    free(s_tr);
    s_tr = nullptr;
    s_trN = s_trLost = 0;
    return true;
  }
  if (s_tr) return true;
  s_tr = static_cast<uint16_t*>(malloc(kTraceCap * sizeof(uint16_t)));
  s_trN = s_trLost = 0;
  return s_tr != nullptr;
}

void traceDump() {
  if (!s_tr) {
    LOGW("usb", "трасування вимкнено (DBG,usb,trace,on)");
    return;
  }
  size_t i = 0;
  while (i < s_trN) {
    const uint16_t dir = s_tr[i] & 0x300;
    char line[24 * 3 + 1];
    char* p = line;
    size_t n = 0;
    while (i < s_trN && (s_tr[i] & 0x300) == dir && n < 24) {
      p += snprintf(p, sizeof line - (size_t)(p - line), "%02X ", (unsigned)(s_tr[i] & 0xFF));
      i++;
      n++;
    }
    LOGI("usb", "%c %s", dir == 0x100 ? 'T' : (dir == 0x200 ? 'F' : 'R'), line);
  }
  if (s_trLost) LOGW("usb", "трасування: втрачено %u байтів (буфер повний)", (unsigned)s_trLost);
  s_trN = s_trLost = 0;
}

bool begin() {
  if (s_started) return true;
  Serial2.setRxBufferSize(kCh376RxBuf);  // RX-буфер виділяється в heap тут
  Serial2.begin(kCh376DefaultBaud, SERIAL_8N1, PIN_CH_RX, PIN_CH_TX);
  if (CH_INT_PIN >= 0) {
    pinMode(CH_INT_PIN, INPUT_PULLUP);
    s_sem = xSemaphoreCreateBinary();  // ≈ 100 Б heap; не вдалось — waitIntLow опитує пін
    if (s_sem) attachInterrupt(CH_INT_PIN, intIsr, FALLING);
  }
  if (CH_RST_PIN >= 0) {
    pinMode(CH_RST_PIN, OUTPUT);
    digitalWrite(CH_RST_PIN, kCh376RstActiveHigh ? LOW : HIGH);
  }
  s_sleepAt = micros();
  s_drv.setBaudState(kCh376DefaultBaud);
  s_link.setBaudValue(kCh376DefaultBaud);
  s_fs.reset();
  s_rd.reset();
  s_started = true;
  return true;
}

bool started() { return s_started; }

void stop() {
  if (!s_started) return;
  if (CH_INT_PIN >= 0) detachInterrupt(CH_INT_PIN);
  if (s_sem) {
    vSemaphoreDelete(s_sem);
    s_sem = nullptr;
  }
  trace(false);
  Serial2.end();  // звільняє RX-буфер
  s_started = false;
  s_drv.setBaudState(kCh376DefaultBaud);
  s_link.setBaudValue(kCh376DefaultBaud);
  s_fs.reset();
  s_rd.reset();
}

int intLevel() {
  if (!s_started || CH_INT_PIN < 0) return -1;
  return digitalRead(CH_INT_PIN) == HIGH ? 1 : 0;
}

Ch376& driver() { return s_drv; }
Ch376Fs& fs() { return s_fs; }
Ch376FileReader& reader() { return s_rd; }

}  // namespace ch376_uart

#endif  // ENABLE_USB_CH376
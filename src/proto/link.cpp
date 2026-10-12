#include "proto/link.h"

#include <Arduino.h>

#include "config.h"
#include "core/router.h"
#include "pins.h"
#include "proto/frame.h"
#include "proto/proto_table.h"
#include "util/diag.h"
#include "util/log.h"

namespace {

constexpr const char* TAG = "link";

struct TxItem {
  uint16_t len;
  char data[proto::kMaxFrame + 1];
};

alignas(4) uint8_t s_hiBuf[kTxHiDepth * sizeof(TxItem)];
alignas(4) uint8_t s_loBuf[kTxLoDepth * sizeof(TxItem)];
StaticQueue_t s_hiCtl, s_loCtl;
QueueHandle_t s_hi = nullptr, s_lo = nullptr;
TaskHandle_t s_txTask = nullptr, s_rxTask = nullptr;

volatile uint32_t s_lastValid = 0;
volatile bool s_everRx = false;
volatile uint32_t s_dropped = 0;

void onFrame(char* body, size_t, void*) {
  s_lastValid = millis();
  s_everRx = true;
  Router::dispatch(body, Origin::Link);
}
proto::FrameParser s_parser(onFrame, nullptr);

bool aliveNow(uint32_t now) {
  return s_everRx && (int32_t)(now - s_lastValid) <= (int32_t)kPartnerTimeoutMs;
}

void sendItem(const TxItem& it) {
  Serial1.write((const uint8_t*)it.data, it.len);
  Log::line("TX ", it.data + 1, it.len - 5);  // дзеркало в UART0 без '$' і '*XX'
}

void sendHb(uint16_t seq) {
  char buf[proto::kMaxFrame + 1];
  proto::Arg a[] = {proto::Arg::N(seq)};
  size_t len = proto::encodeFrame(buf, sizeof buf, proto::ev::kHb, a, 1);
  if (!len) return;
  Serial1.write((const uint8_t*)buf, len);
#if MIRROR_HB
  Log::line("TX ", buf + 1, len - 5);
#endif
}

void txTaskFn(void*) {
  const uint32_t boot = millis();
  uint32_t nextHb = boot + kHbPeriodMs;
  uint16_t seq = 0;  // 0..65535 по колу (природне переповнення uint16_t)
  bool aliveState = false, warnedNone = false;
  for (;;) {
    uint32_t now = millis();
    if ((int32_t)(now - nextHb) >= 0) {
      sendHb(seq++);
      nextHb += kHbPeriodMs;
      if ((int32_t)(now - nextHb) >= 0) nextHb = now + kHbPeriodMs;
      bool alive = aliveNow(now);
      if (alive != aliveState) {
        aliveState = alive;
        if (alive) LOGI(TAG, "партнер доступний");
        else LOGW(TAG, "партнер недоступний (немає валідних кадрів > %u мс)", (unsigned)kPartnerTimeoutMs);
      }
      if (!s_everRx && !warnedNone && (now - boot) > kPartnerTimeoutMs) {
        warnedNone = true;
        LOGW(TAG, "S3 не виявлено; працює режим налагодження через UART0");
      }
    }
    TxItem it;
    if (xQueueReceive(s_hi, &it, 0) == pdTRUE || xQueueReceive(s_lo, &it, 0) == pdTRUE) {
      sendItem(it);
      continue;
    }
    int32_t d = (int32_t)(nextHb - millis());
    if (d <= 0) continue;
    TickType_t t = pdMS_TO_TICKS(d);
    ulTaskNotifyTake(pdTRUE, t ? t : 1);
  }
}

void rxTaskFn(void*) {
  uint8_t tmp[64];
  for (;;) {
    int avail = Serial1.available();
    if (avail <= 0) {
      vTaskDelay(1);
      continue;
    }
    size_t n = Serial1.readBytes(tmp, avail > (int)sizeof tmp ? sizeof tmp : (size_t)avail);
    s_parser.feed(tmp, n);
  }
}

}  // namespace

void Link::begin() {
  s_hi = xQueueCreateStatic(kTxHiDepth, sizeof(TxItem), s_hiBuf, &s_hiCtl);
  s_lo = xQueueCreateStatic(kTxLoDepth, sizeof(TxItem), s_loBuf, &s_loCtl);
  Serial1.setRxBufferSize(kLinkRxBuf);
  Serial1.begin(kLinkBaud, SERIAL_8N1, PIN_LINK_RX, PIN_LINK_TX);
  s_lastValid = millis();
  xTaskCreatePinnedToCore(txTaskFn, "link_tx", kStackLinkTx, nullptr, kPrioLink, &s_txTask, kCoreProto);
  xTaskCreatePinnedToCore(rxTaskFn, "link_rx", kStackLinkRx, nullptr, kPrioLink, &s_rxTask, kCoreProto);
  Diag::registerTask("link_tx", s_txTask);
  Diag::registerTask("link_rx", s_rxTask);
  LOGI(TAG, "UART1 %u 8N1 RX=%d TX=%d", (unsigned)kLinkBaud, PIN_LINK_RX, PIN_LINK_TX);
}

void Link::send(const char* frame, size_t len, bool high) {
  if (!s_hi || !s_txTask || len == 0 || len > proto::kMaxFrame) return;
  TxItem it;
  it.len = (uint16_t)len;
  memcpy(it.data, frame, len);
  QueueHandle_t q = high ? s_hi : s_lo;
  if (xQueueSend(q, &it, 0) != pdTRUE) {  // черга повна: викинути найстаріший
    TxItem junk;
    xQueueReceive(q, &junk, 0);
    s_dropped++;
    xQueueSend(q, &it, 0);
  }
  xTaskNotifyGive(s_txTask);
}

Link::Stats Link::stats() {
  return Stats{s_parser.good(), s_parser.bad(), s_dropped, aliveNow(millis())};
}
#pragma once
// Усі номери пінів тільки тут. Відхилення від мапи — з обґрунтуванням у звіті.

inline constexpr int PIN_I2S_BCK  = 26;
inline constexpr int PIN_I2S_WS   = 25;
inline constexpr int PIN_I2S_DOUT = 22;
inline constexpr int PIN_XSMT     = 27;   // LOW = мʼют; рекомендовано підтяжка 100k до GND

inline constexpr int PIN_SD_SCK  = 18;
inline constexpr int PIN_SD_MISO = 19;
inline constexpr int PIN_SD_MOSI = 23;
inline constexpr int PIN_SD_CS   = 5;     // strapping, для CS ок; підтяжка 10k до 3V3
inline constexpr int SD_CD_PIN   = 21;    // -1 якщо нема card-detect

inline constexpr int PIN_CH_RX  = 16;     // ← TXD CH376S (через дільник 5В→3,3В)
inline constexpr int PIN_CH_TX  = 17;     // → RXD CH376S
inline constexpr int CH_INT_PIN = 4;      // -1 якщо нема
inline constexpr int CH_RST_PIN = -1;     // -1 якщо нема

inline constexpr int PIN_LINK_RX = 32;    // ← TX S3 (GPIO42)
inline constexpr int PIN_LINK_TX = 33;    // → RX S3 (GPIO48)

// Заборонено: strapping 0,2,12,15; флеш 6–11; input-only 34–39
constexpr bool pinOk(int p) {
  return p < 0 || !(p == 0 || p == 2 || p == 12 || p == 15 ||
                    (p >= 6 && p <= 11) || (p >= 34 && p <= 39));
}
static_assert(pinOk(PIN_I2S_BCK) && pinOk(PIN_I2S_WS) && pinOk(PIN_I2S_DOUT) && pinOk(PIN_XSMT), "I2S/XSMT: заборонений пін");
static_assert(pinOk(PIN_SD_SCK) && pinOk(PIN_SD_MISO) && pinOk(PIN_SD_MOSI) && pinOk(PIN_SD_CS) && pinOk(SD_CD_PIN), "SD: заборонений пін");
static_assert(pinOk(PIN_CH_RX) && pinOk(PIN_CH_TX) && pinOk(CH_INT_PIN) && pinOk(CH_RST_PIN), "CH376S: заборонений пін");
static_assert(pinOk(PIN_LINK_RX) && pinOk(PIN_LINK_TX), "Link: заборонений пін");
// led_out.h - one column out to the strip, for every supported chip.
//
// Clocked chips (APA102 / SK9822 / HD107S): ESP-IDF SPI master with DMA and two
// frame buffers, so the next column is encoded while the current one is on the
// wire. Single-wire chips (WS281x / SK6812): FastLED.
//
// API (same for both paths):
//   begin(nMax, n, hz)      allocate for nMax LEDs, start with n active
//   setCount(n)             active LED count (<= nMax)
//   build(i, col, map)      stage column into buffer i (0/1); col == nullptr -> black
//   send(i)                 start output of buffer i
//   waitDone()              block until the wire is idle
//   minColUs()              shortest column period the chip + strip allow
//   blankAll()              clear every LED up to nMax (after shrinking n)
#pragma once
#include <Arduino.h>
#include <math.h>
#include "config.h"

#if LED_IS_CLOCKED(POV_CHIP)
  #include "driver/spi_master.h"
  #include "esp_heap_caps.h"
  #include "esp_idf_version.h"
#else
  #include <FastLED.h>
#endif

#if defined(CONFIG_IDF_TARGET_ESP32) && \
    ((POV_PIN_DATA >= 6 && POV_PIN_DATA <= 11) || (LED_IS_CLOCKED(POV_CHIP) && POV_PIN_CLOCK >= 6 && POV_PIN_CLOCK <= 11))
  #error "GPIO 6-11 are the flash pins on ESP32. Set POV_PIN_DATA / POV_PIN_CLOCK in config.h (DevKit: 23 / 18)."
#endif
#if defined(CONFIG_IDF_TARGET_ESP32S3) && \
    ((POV_PIN_DATA >= 26 && POV_PIN_DATA <= 32) || (LED_IS_CLOCKED(POV_CHIP) && POV_PIN_CLOCK >= 26 && POV_PIN_CLOCK <= 32))
  #error "GPIO 26-32 are the flash pins on ESP32-S3. Set POV_PIN_DATA / POV_PIN_CLOCK in config.h."
#endif

#if POV_CHIP == LED_SK9822 || POV_CHIP == LED_HD107S
  #define POV_CURRENT_GB 1   // 5-bit field sets drive current, not a second PWM
#else
  #define POV_CURRENT_GB 0
#endif

namespace ledout {

// ---- rendering state, rebuilt by setRender() ------------------------------------
static uint8_t s_lut8[256];   // sRGB -> output PWM value
static uint8_t s_lut5[256];   // sRGB -> 0..31 current level (RENDER_CURRENT)
static uint8_t s_gb = 31;     // 5-bit field for FULL / PWM_SAFE
static uint8_t s_on = 255;    // "on" value for PWM_SAFE
static uint8_t s_mode = RENDER_FULL;

static const char *chipName() {
  switch (POV_CHIP) {
    case LED_APA102: return "APA102";
    case LED_SK9822: return "SK9822";
    case LED_HD107S: return "HD107S";
    case LED_WS2812B: return "WS2812B";
    case LED_WS2812B_V5: return "WS2812B-V5";
    case LED_SK6812: return "SK6812";
    case LED_WS2811: return "WS2811";
    case LED_WS2813: return "WS2813";
    case LED_WS2815: return "WS2815";
  }
  return "?";
}

// Returns the mode actually used (RENDER_CURRENT falls back on chips without
// current control).
static uint8_t setRender(uint8_t mode, uint8_t bri, float gamma) {
  if (mode == RENDER_CURRENT && !POV_CURRENT_GB) mode = RENDER_PWM_SAFE;
  s_mode = mode;
  if (gamma < 0.5f) gamma = 0.5f;
  // Chips with current control dim through the 5-bit field so the PWM duty
  // (and its gaps) is not shortened; the rest dim in the 8-bit value.
  const float scale8 = POV_CURRENT_GB ? 255.0f : 255.0f * bri / 255.0f;
  for (int v = 0; v < 256; ++v) {
    const float g = powf(v / 255.0f, gamma);
    s_lut8[v] = (uint8_t)lroundf(g * scale8);
    s_lut5[v] = (uint8_t)lroundf(g * 31.0f * bri / 255.0f);
  }
  s_gb = POV_CURRENT_GB ? (uint8_t)lroundf(31.0f * bri / 255.0f) : 31;
  if (POV_CURRENT_GB && bri > 0 && s_gb == 0) s_gb = 1;
  // PWM-safe means no PWM gaps inside a column, so the on value is always full.
  // Chips with current control still dim through s_gb; the others ignore bri here.
  s_on = 255;
  return mode;
}

#if LED_IS_CLOCKED(POV_CHIP)
// =================================================================================
// Clocked: SPI + DMA
// Frame: start (32 x 0) | n x [0xE0|gb, c0, c1, c2] | reset (32 x 0) | end (>= n/2 bits of 0)
// The reset frame makes SK9822 latch now instead of on the next frame; APA102
// ignores it. Zero end bits keep LEDs past n untouched.
// =================================================================================
static spi_device_handle_t s_dev = nullptr;
static uint8_t *s_buf[2] = {nullptr, nullptr};
static spi_transaction_t s_tr[2];
static bool s_busy = false;
static uint16_t s_nMax = 0, s_n = 0;
static uint32_t s_bytes = 0;
static int s_hz = 0;
static uint8_t s_ord[3] = {2, 1, 0};  // wire byte k <- source channel s_ord[k]
static uint32_t s_errs = 0;           // failed SPI queue calls (shown by "info")

static inline uint32_t frameBytes(uint16_t n) {
  const uint32_t endBytes = (n + 15u) / 16u + 1u;
  const uint32_t b = 4u + 4u * n + 4u + endBytes;
  return (b + 3u) & ~3u;
}

static void parseOrder(const char *o) {
  for (int k = 0; k < 3; ++k) {
    const char c = o[k];
    s_ord[k] = (c == 'R' || c == 'r') ? 0 : (c == 'G' || c == 'g') ? 1 : 2;
  }
}

static bool addDevice(uint32_t hz) {
  spi_device_interface_config_t dc;
  memset(&dc, 0, sizeof dc);
  dc.clock_speed_hz = (int)hz;
  dc.mode = 0;
  dc.spics_io_num = -1;
  dc.queue_size = 2;
  dc.flags = SPI_DEVICE_HALFDUPLEX | SPI_DEVICE_NO_DUMMY;  // write-only
  if (spi_bus_add_device(SPI2_HOST, &dc, &s_dev) != ESP_OK) {
    s_dev = nullptr;
    return false;
  }
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 0, 0)
  int khz = 0;
  if (spi_device_get_actual_freq(s_dev, &khz) == ESP_OK) s_hz = khz * 1000;
  else s_hz = (int)hz;
#else
  s_hz = spi_get_actual_clock(80000000, (int)hz, 128);
#endif
  return true;
}

static void waitDone() {
  if (!s_busy || !s_dev) return;
  spi_transaction_t *done = nullptr;
  spi_device_get_trans_result(s_dev, &done, portMAX_DELAY);
  s_busy = false;
}

static bool begin(uint16_t nMax, uint16_t n, uint32_t hz) {
  s_nMax = nMax;
  const uint32_t maxBytes = frameBytes(nMax);
  for (int i = 0; i < 2; ++i) {
    s_buf[i] = (uint8_t *)heap_caps_malloc(maxBytes, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    if (!s_buf[i]) return false;
    memset(s_buf[i], 0, maxBytes);
    memset(&s_tr[i], 0, sizeof s_tr[i]);
    s_tr[i].tx_buffer = s_buf[i];
  }
  parseOrder(POV_CLOCKED_ORDER);

  spi_bus_config_t bus;
  memset(&bus, 0, sizeof bus);
  bus.mosi_io_num = POV_PIN_DATA;
  bus.miso_io_num = -1;
  bus.sclk_io_num = POV_PIN_CLOCK;
  bus.quadwp_io_num = -1;
  bus.quadhd_io_num = -1;
  bus.data4_io_num = -1;
  bus.data5_io_num = -1;
  bus.data6_io_num = -1;
  bus.data7_io_num = -1;
  bus.max_transfer_sz = (int)maxBytes;
  if (spi_bus_initialize(SPI2_HOST, &bus, SPI_DMA_CH_AUTO) != ESP_OK) return false;
  if (!addDevice(hz)) return false;
  s_n = n > nMax ? nMax : n;
  s_bytes = frameBytes(s_n);
  return true;
}

static bool setClock(uint32_t hz) {
  waitDone();
  if (s_dev) spi_bus_remove_device(s_dev);
  s_dev = nullptr;
  return addDevice(hz);
}

static int actualHz() { return s_hz; }

static void setCount(uint16_t n) {
  waitDone();
  s_n = n > s_nMax ? s_nMax : n;
  s_bytes = frameBytes(s_n);
}

// Wire time for one column, plus a fixed allowance for queueing the DMA.
static uint32_t minColUs() {
  const double us = (double)s_bytes * 8.0 * 1e6 / (double)(s_hz > 0 ? s_hz : 1);
  return (uint32_t)ceil(us) + 25;
}

static uint32_t wireBits() { return s_bytes * 8u; }

// col: this column's pixels (RGB888, image rows); map[i]: image row for LED i.
static void build(int i, const uint8_t *col, const uint16_t *map) {
  uint8_t *p = s_buf[i];
  p[0] = p[1] = p[2] = p[3] = 0;
  p += 4;
  const uint16_t n = s_n;
  const uint8_t o0 = s_ord[0], o1 = s_ord[1], o2 = s_ord[2];
  if (!col) {
    for (uint16_t k = 0; k < n; ++k, p += 4) { p[0] = 0xE0; p[1] = p[2] = p[3] = 0; }
  } else if (s_mode == RENDER_FULL) {
    const uint8_t hdr = 0xE0 | s_gb;
    for (uint16_t k = 0; k < n; ++k, p += 4) {
      const uint8_t *s = col + (uint32_t)map[k] * 3u;
      p[0] = hdr; p[1] = s_lut8[s[o0]]; p[2] = s_lut8[s[o1]]; p[3] = s_lut8[s[o2]];
    }
  } else if (s_mode == RENDER_PWM_SAFE) {
    const uint8_t hdr = 0xE0 | s_gb, on = s_on;
    for (uint16_t k = 0; k < n; ++k, p += 4) {
      const uint8_t *s = col + (uint32_t)map[k] * 3u;
      p[0] = hdr;
      p[1] = s[o0] >= 128 ? on : 0; p[2] = s[o1] >= 128 ? on : 0; p[3] = s[o2] >= 128 ? on : 0;
    }
  } else {  // RENDER_CURRENT
    for (uint16_t k = 0; k < n; ++k, p += 4) {
      const uint8_t *s = col + (uint32_t)map[k] * 3u;
      uint8_t m = s[0] > s[1] ? s[0] : s[1];
      if (s[2] > m) m = s[2];
      const uint8_t lvl = s_lut5[m];
      if (!lvl) { p[0] = 0xE0; p[1] = p[2] = p[3] = 0; continue; }
      // a channel is on when it is at least half of the brightest channel
      p[0] = 0xE0 | lvl;
      p[1] = (uint16_t)s[o0] * 2u >= m ? 255 : 0;
      p[2] = (uint16_t)s[o1] * 2u >= m ? 255 : 0;
      p[3] = (uint16_t)s[o2] * 2u >= m ? 255 : 0;
    }
  }
  // reset + end frames
  memset(p, 0, s_bytes - (uint32_t)(p - s_buf[i]));
}

static void send(int i) {
  if (!s_dev) return;
  waitDone();
  s_tr[i].length = s_bytes * 8u;
  s_tr[i].rxlength = 0;
  if (spi_device_queue_trans(s_dev, &s_tr[i], portMAX_DELAY) == ESP_OK) s_busy = true;
  else s_errs++;
}

static uint32_t errors() { return s_errs; }

static void blankAll() {
  const uint16_t keep = s_n;
  setCount(s_nMax);
  build(0, nullptr, nullptr);
  send(0);
  waitDone();
  setCount(keep);
}

#else
// =================================================================================
// Single-wire: FastLED. Bit periods and resets follow FastLED 3.10 timing tables.
// =================================================================================
#if POV_CHIP == LED_WS2812B
  #define POV_FL_CHIP WS2812B
  #define POV_BIT_NS 1250
  #define POV_CHIP_RESET_US 280
#elif POV_CHIP == LED_WS2812B_V5
  #if defined(FASTLED_VERSION) && FASTLED_VERSION >= 3010000
    #define POV_FL_CHIP WS2812BV5
    #define POV_BIT_NS 1225
  #else
    #define POV_FL_CHIP WS2812B
    #define POV_BIT_NS 1250
  #endif
  #define POV_CHIP_RESET_US 280
#elif POV_CHIP == LED_SK6812
  #define POV_FL_CHIP SK6812
  #define POV_BIT_NS 1200
  #define POV_CHIP_RESET_US 80
#elif POV_CHIP == LED_WS2811
  #define POV_FL_CHIP WS2811
  #define POV_BIT_NS 1250
  #define POV_CHIP_RESET_US 280
#elif POV_CHIP == LED_WS2813
  #define POV_FL_CHIP WS2813
  #define POV_BIT_NS 1280
  #define POV_CHIP_RESET_US 300
#elif POV_CHIP == LED_WS2815
  #define POV_FL_CHIP WS2815
  #define POV_BIT_NS 1890
  #define POV_CHIP_RESET_US 300
#else
  #error "Unknown POV_CHIP"
#endif

static CRGB s_leds[POV_NUM_LEDS_MAX];
static CLEDController *s_ctl = nullptr;
static const uint8_t *s_stage[2] = {nullptr, nullptr};
static const uint16_t *s_map = nullptr;
static uint16_t s_nMax = 0, s_n = 0;

static uint32_t resetUs() { return POV_RESET_US > 0 ? (uint32_t)POV_RESET_US : (uint32_t)POV_CHIP_RESET_US; }

static bool begin(uint16_t nMax, uint16_t n, uint32_t /*hz*/) {
  s_nMax = nMax;
  // Register the full buffer so FastLED sizes everything for nMax, then shrink.
  s_ctl = &FastLED.addLeds<POV_FL_CHIP, POV_PIN_DATA, POV_FASTLED_ORDER>(s_leds, nMax);
  FastLED.setDither(DISABLE_DITHER);
  FastLED.setCorrection(UncorrectedColor);
  FastLED.setTemperature(UncorrectedTemperature);
  FastLED.setBrightness(255);         // brightness is in our LUT
  FastLED.setMaxRefreshRate(0, false);  // we schedule columns ourselves
  s_n = n > nMax ? nMax : n;
  s_ctl->setLeds(s_leds, s_n);
  return true;
}

// Unused on single-wire chips; kept so both paths share one API.
__attribute__((unused)) static bool setClock(uint32_t) { return true; }
__attribute__((unused)) static int actualHz() { return (int)(1e9 / POV_BIT_NS); }

static void setCount(uint16_t n) {
  s_n = n > s_nMax ? s_nMax : n;
  s_ctl->setLeds(s_leds, s_n);
}

static uint32_t wireBits() { return 24u * s_n; }

// Data time + latch gap the chip needs between columns.
static uint32_t minColUs() {
  return (uint32_t)((uint64_t)s_n * 24u * POV_BIT_NS / 1000u) + resetUs() + 25;
}

// FastLED has one pixel buffer, so build() only records the column; send()
// encodes it right after the deadline. Costs a few tens of us of fixed latency.
// FastLED 3.10 copies the pixels into its own buffer and returns from show()
// while the RMT peripheral is still clocking them out; the next show() waits
// for that to finish. The column period already covers data time plus latch.
static void build(int i, const uint8_t *col, const uint16_t *map) {
  s_stage[i] = col;
  s_map = map;
}

static void send(int i) {
  const uint8_t *col = s_stage[i];
  const uint16_t n = s_n;
  if (!col) {
    for (uint16_t k = 0; k < n; ++k) s_leds[k] = CRGB(0, 0, 0);
  } else if (s_mode == RENDER_FULL) {
    for (uint16_t k = 0; k < n; ++k) {
      const uint8_t *s = col + (uint32_t)s_map[k] * 3u;
      s_leds[k] = CRGB(s_lut8[s[0]], s_lut8[s[1]], s_lut8[s[2]]);
    }
  } else {  // RENDER_PWM_SAFE (CURRENT falls back here): full on or off, no PWM gaps
    for (uint16_t k = 0; k < n; ++k) {
      const uint8_t *s = col + (uint32_t)s_map[k] * 3u;
      s_leds[k] = CRGB(s[0] >= 128 ? 255 : 0, s[1] >= 128 ? 255 : 0, s[2] >= 128 ? 255 : 0);
    }
  }
  FastLED.show();
}

static void waitDone() {}

static uint32_t errors() { return 0; }

static void blankAll() {
  const uint16_t keep = s_n;
  setCount(s_nMax);
  for (uint16_t k = 0; k < s_nMax; ++k) s_leds[k] = CRGB(0, 0, 0);
  FastLED.show();  // the next show() waits for this transmission to finish
  setCount(keep);
}
#endif

}  // namespace ledout

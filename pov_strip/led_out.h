// led_out.h - one column out to the strip, for every supported chip.
//
// Clocked chips (APA102 / SK9822 / HD107S): ESP-IDF SPI master with DMA and two
// frame buffers, so the next column is encoded while the current one is on the
// wire. Single-wire chips (WS281x / SK6812): ESP-IDF RMT with a bytes encoder,
// also double-buffered. No LED library; the esp32 core is the only dependency.
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
  #include "driver/rmt_tx.h"
  #include "driver/rmt_encoder.h"
  #include "soc/soc_caps.h"
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
static uint8_t s_ord[3] = {2, 1, 0};  // wire byte k <- source channel s_ord[k]

static void parseOrder(const char *o) {
  for (int k = 0; k < 3; ++k) {
    const char c = o[k];
    s_ord[k] = (c == 'R' || c == 'r') ? 0 : (c == 'G' || c == 'g') ? 1 : 2;
  }
}

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

// Output load of one source pixel: the three channel values the chip will drive,
// summed (0..765), scaled by the 5-bit current field on chips that use it as such.
// Times POV_MA_PER_CHANNEL / 255 this is the pixel's current.
static uint16_t pixelLoad(const uint8_t *s) {
  if (s_mode == RENDER_CURRENT) {
    uint8_t m = s[0] > s[1] ? s[0] : s[1];
    if (s[2] > m) m = s[2];
    const uint8_t lvl = s_lut5[m];
    if (!lvl) return 0;
    const uint16_t on = (uint16_t)(((uint16_t)s[0] * 2u >= m) + ((uint16_t)s[1] * 2u >= m) + ((uint16_t)s[2] * 2u >= m));
    return (uint16_t)(on * 255u * lvl / 31u);
  }
  uint16_t v;
  if (s_mode == RENDER_FULL) v = (uint16_t)(s_lut8[s[0]] + s_lut8[s[1]] + s_lut8[s[2]]);
  else v = (uint16_t)((s[0] >= 128 ? s_on : 0) + (s[1] >= 128 ? s_on : 0) + (s[2] >= 128 ? s_on : 0));
  return POV_CURRENT_GB ? (uint16_t)((uint32_t)v * s_gb / 31u) : v;
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
static uint32_t s_errs = 0;           // failed SPI queue calls (shown by "info")

static inline uint32_t frameBytes(uint16_t n) {
  const uint32_t endBytes = (n + 15u) / 16u + 1u;
  const uint32_t b = 4u + 4u * n + 4u + endBytes;
  return (b + 3u) & ~3u;
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
// Single-wire: ESP-IDF RMT. A bytes encoder turns each payload byte into 8
// symbols (one high/low pair per bit, MSB first); a copy encoder appends the
// latch (reset) as one all-low symbol, so "done" means the chip has latched.
// Two payload buffers in wire byte order; build() fills one while the other
// is on the wire. The peripheral is refilled from an interrupt (no DMA on the
// ESP32), which is fine without WiFi; the firmware never sends while it
// writes flash.
//
// Bit timings in ns (high, low) for a 0 bit and a 1 bit, and the latch in us.
// Both bits of a chip have the same period, POV_BIT_NS, which is what the
// column budget uses (core.js / core.py carry the same numbers).
// =================================================================================
#if POV_CHIP == LED_WS2812B
  // WS2812B datasheet nominal: T0H 0.4, T0L 0.85, T1H 0.8, T1L 0.45 us, RES > 50 us
  // (280 us covers the 2020+ parts). Same values Adafruit_NeoPixel uses on ESP32.
  #define POV_T0H 400
  #define POV_T0L 850
  #define POV_T1H 800
  #define POV_T1L 450
  #define POV_CHIP_RESET_US 280
#elif POV_CHIP == LED_WS2812B_V5
  // V5 datasheet ranges: T0H 220-380, T1H 580-1000, T0L 580-1000, T1L 580-1000, RES > 280 us.
  #define POV_T0H 300
  #define POV_T0L 925
  #define POV_T1H 600
  #define POV_T1L 625
  #define POV_CHIP_RESET_US 280
#elif POV_CHIP == LED_SK6812
  // SK6812 datasheet nominal: T0H 0.3, T0L 0.9, T1H 0.6, T1L 0.6 us (+-150 ns), Treset > 80 us
  #define POV_T0H 300
  #define POV_T0L 900
  #define POV_T1H 600
  #define POV_T1L 600
  #define POV_CHIP_RESET_US 80
#elif POV_CHIP == LED_WS2811
  // WS2811 high-speed mode nominal: T0H 0.25, T0L 1.0, T1H 0.6, T1L 0.65 us, RES > 50 us
  #define POV_T0H 250
  #define POV_T0L 1000
  #define POV_T1H 600
  #define POV_T1L 650
  #define POV_CHIP_RESET_US 280
#elif POV_CHIP == LED_WS2813
  // FastLED 3.10 WS2813 timing (320 / 960, 640 / 640) rounded to 25 ns ticks
  #define POV_T0H 325
  #define POV_T0L 950
  #define POV_T1H 650
  #define POV_T1L 625
  #define POV_CHIP_RESET_US 300
#elif POV_CHIP == LED_WS2815
  // FastLED 3.10 WS2815 timing (250 / 1640, 1340 / 550) rounded to 25 ns ticks
  #define POV_T0H 250
  #define POV_T0L 1650
  #define POV_T1H 1350
  #define POV_T1L 550
  #define POV_CHIP_RESET_US 300
#else
  #error "Unknown POV_CHIP"
#endif
#define POV_BIT_NS (POV_T0H + POV_T0L)
static_assert(POV_T0H + POV_T0L == POV_T1H + POV_T1L, "0 and 1 bits must have the same period");

#define POV_RMT_HZ 40000000u          // 25 ns ticks; 15-bit durations reach 819 us
#define POV_RMT_BLOCKS 2              // RMT memory blocks (fewer refill interrupts)
#define POV_NS_TICKS(ns) ((uint16_t)(((ns) * (POV_RMT_HZ / 1000000u) + 500u) / 1000u))

static rmt_channel_handle_t s_chan = nullptr;
static uint8_t *s_buf[2] = {nullptr, nullptr};   // payloads, 3 bytes per LED, wire order
static bool s_busy = false;
static uint16_t s_nMax = 0, s_n = 0;
static uint32_t s_errs = 0;           // failed transmit calls / timeouts (shown by "info")

// pixel bytes, then the latch symbol
struct PixelEncoder {
  rmt_encoder_t base;                 // must stay the first member (handle == &base)
  rmt_encoder_handle_t bytes;
  rmt_encoder_handle_t copy;
  rmt_symbol_word_t latch;
  int state;                          // 0: pixel bytes, 1: latch
};
static PixelEncoder s_enc;

static uint32_t resetUs() { return POV_RESET_US > 0 ? (uint32_t)POV_RESET_US : (uint32_t)POV_CHIP_RESET_US; }

static size_t IRAM_ATTR encodeCb(rmt_encoder_t *enc, rmt_channel_handle_t ch, const void *data, size_t size,
                                 rmt_encode_state_t *retState) {
  PixelEncoder *e = (PixelEncoder *)enc;
  unsigned out = RMT_ENCODING_RESET;
  rmt_encode_state_t st = RMT_ENCODING_RESET;
  size_t n = 0;
  if (e->state == 0) {
    n += e->bytes->encode(e->bytes, ch, data, size, &st);
    if (st & RMT_ENCODING_COMPLETE) e->state = 1;
    if (st & RMT_ENCODING_MEM_FULL) out |= RMT_ENCODING_MEM_FULL;
  }
  if (e->state == 1 && !(out & RMT_ENCODING_MEM_FULL)) {
    n += e->copy->encode(e->copy, ch, &e->latch, sizeof e->latch, &st);
    if (st & RMT_ENCODING_COMPLETE) { e->state = 0; out |= RMT_ENCODING_COMPLETE; }
    if (st & RMT_ENCODING_MEM_FULL) out |= RMT_ENCODING_MEM_FULL;
  }
  *retState = (rmt_encode_state_t)out;
  return n;
}

static esp_err_t resetCb(rmt_encoder_t *enc) {
  PixelEncoder *e = (PixelEncoder *)enc;
  rmt_encoder_reset(e->bytes);
  rmt_encoder_reset(e->copy);
  e->state = 0;
  return ESP_OK;
}

static esp_err_t delCb(rmt_encoder_t *enc) {
  PixelEncoder *e = (PixelEncoder *)enc;
  if (e->bytes) rmt_del_encoder(e->bytes);
  if (e->copy) rmt_del_encoder(e->copy);
  e->bytes = e->copy = nullptr;
  return ESP_OK;
}

static bool makeEncoder() {
  memset(&s_enc, 0, sizeof s_enc);
  s_enc.base.encode = encodeCb;
  s_enc.base.reset = resetCb;
  s_enc.base.del = delCb;
  rmt_bytes_encoder_config_t bc;
  memset(&bc, 0, sizeof bc);
  bc.bit0.level0 = 1; bc.bit0.duration0 = POV_NS_TICKS(POV_T0H);
  bc.bit0.level1 = 0; bc.bit0.duration1 = POV_NS_TICKS(POV_T0L);
  bc.bit1.level0 = 1; bc.bit1.duration0 = POV_NS_TICKS(POV_T1H);
  bc.bit1.level1 = 0; bc.bit1.duration1 = POV_NS_TICKS(POV_T1L);
  bc.flags.msb_first = 1;
  if (rmt_new_bytes_encoder(&bc, &s_enc.bytes) != ESP_OK) return false;
  rmt_copy_encoder_config_t cc;
  memset(&cc, 0, sizeof cc);
  if (rmt_new_copy_encoder(&cc, &s_enc.copy) != ESP_OK) return false;
  // latch: all low, split over the two halves of one symbol (each <= 32767 ticks)
  uint32_t half = resetUs() * (POV_RMT_HZ / 1000000u) / 2u;
  if (half > 32767u) half = 32767u;
  if (half < 1u) half = 1u;
  s_enc.latch.level0 = 0; s_enc.latch.duration0 = (uint16_t)half;
  s_enc.latch.level1 = 0; s_enc.latch.duration1 = (uint16_t)half;
  return true;
}

static void waitDone() {
  if (!s_busy || !s_chan) return;
  if (rmt_tx_wait_all_done(s_chan, 200) != ESP_OK) s_errs++;   // 720 LEDs take ~22 ms
  s_busy = false;
}

static bool begin(uint16_t nMax, uint16_t n, uint32_t /*hz*/) {
  s_nMax = nMax;
  for (int i = 0; i < 2; ++i) {
    s_buf[i] = (uint8_t *)malloc(3u * nMax);
    if (!s_buf[i]) return false;
    memset(s_buf[i], 0, 3u * nMax);
  }
  parseOrder(POV_SINGLE_ORDER);
  rmt_tx_channel_config_t ch;
  memset(&ch, 0, sizeof ch);
  ch.gpio_num = (gpio_num_t)POV_PIN_DATA;
  ch.clk_src = RMT_CLK_SRC_DEFAULT;
  ch.resolution_hz = POV_RMT_HZ;
  ch.mem_block_symbols = SOC_RMT_MEM_WORDS_PER_CHANNEL * POV_RMT_BLOCKS;
  ch.trans_queue_depth = 1;
  if (rmt_new_tx_channel(&ch, &s_chan) != ESP_OK) {
    ch.mem_block_symbols = SOC_RMT_MEM_WORDS_PER_CHANNEL;   // fewer blocks free: take one
    if (rmt_new_tx_channel(&ch, &s_chan) != ESP_OK) { s_chan = nullptr; return false; }
  }
  if (!makeEncoder()) return false;
  if (rmt_enable(s_chan) != ESP_OK) return false;
  s_n = n > nMax ? nMax : n;
  return true;
}

// Unused on single-wire chips; kept so both paths share one API.
__attribute__((unused)) static bool setClock(uint32_t) { return true; }
__attribute__((unused)) static int actualHz() { return (int)(1e9 / POV_BIT_NS); }

static void setCount(uint16_t n) {
  waitDone();
  s_n = n > s_nMax ? s_nMax : n;
}

static uint32_t wireBits() { return 24u * s_n; }

// Data time + latch the chip needs between columns, plus a fixed allowance.
static uint32_t minColUs() {
  return (uint32_t)((uint64_t)s_n * 24u * POV_BIT_NS / 1000u) + resetUs() + 25;
}

// Fills payload i (wire byte order) for `count` LEDs; col == nullptr -> black.
static void fill(int i, const uint8_t *col, const uint16_t *map, uint16_t count) {
  uint8_t *p = s_buf[i];
  if (!col) {
    memset(p, 0, 3u * count);
    return;
  }
  const uint8_t o0 = s_ord[0], o1 = s_ord[1], o2 = s_ord[2];
  if (s_mode == RENDER_FULL) {
    for (uint16_t k = 0; k < count; ++k, p += 3) {
      const uint8_t *s = col + (uint32_t)map[k] * 3u;
      p[0] = s_lut8[s[o0]]; p[1] = s_lut8[s[o1]]; p[2] = s_lut8[s[o2]];
    }
  } else {  // RENDER_PWM_SAFE (CURRENT falls back here): full on or off, no PWM gaps
    const uint8_t on = s_on;
    for (uint16_t k = 0; k < count; ++k, p += 3) {
      const uint8_t *s = col + (uint32_t)map[k] * 3u;
      p[0] = s[o0] >= 128 ? on : 0; p[1] = s[o1] >= 128 ? on : 0; p[2] = s[o2] >= 128 ? on : 0;
    }
  }
}

// col: this column's pixels (RGB888, image rows); map[i]: image row for LED i.
static void build(int i, const uint8_t *col, const uint16_t *map) { fill(i, col, map, s_n); }

static void transmit(int i, uint16_t count) {
  if (!s_chan || count == 0) return;
  waitDone();
  rmt_transmit_config_t tc;
  memset(&tc, 0, sizeof tc);
  tc.loop_count = 0;
  tc.flags.eot_level = 0;
  if (rmt_transmit(s_chan, &s_enc.base, s_buf[i], 3u * count, &tc) == ESP_OK) s_busy = true;
  else s_errs++;
}

static void send(int i) { transmit(i, s_n); }

static uint32_t errors() { return s_errs; }

static void blankAll() {
  waitDone();
  fill(0, nullptr, nullptr, s_nMax);
  transmit(0, s_nMax);
  waitDone();
}
#endif

}  // namespace ledout

// pov_types.h - constants and the image record shared by config.h,
// pov_image.h (exported by POV Studio) and the firmware.
#pragma once
#include <stdint.h>

// ---- LED chip types --------------------------------------------------------
// Clocked (data + clock), driven by ESP32 SPI + DMA
#define LED_APA102      1   // 8-bit PWM ~19.2 kHz; 5-bit global = slow PWM (~440 Hz), so it stays at 31
#define LED_SK9822      2   // 8-bit PWM (4.7 kHz measured, 1.2 kHz in one datasheet); 5-bit global = current
#define LED_HD107S      3   // 8-bit PWM ~26 kHz; 5-bit global = current
// Single data line, driven by ESP32 RMT
#define LED_WS2812B     10  // 800 kHz, PWM ~430 Hz on older parts
#define LED_WS2812B_V5  11  // 800 kHz, PWM ("refresh") 2 kHz, reset > 280 us
#define LED_SK6812      12  // 800 kHz, PWM ~1.1 kHz (RGB only)
#define LED_WS2811      13  // 800 kHz mode
#define LED_WS2813      14  // 800 kHz
#define LED_WS2815      15  // ~525 kHz (1.9 us/bit, FastLED timing)

#define LED_IS_CLOCKED(t)  ((t) < 10)

// ---- Viewing direction -----------------------------------------------------
// A glance left->right paints later columns further LEFT on the retina, so the
// image has to play right-to-left in time. Pick what your viewers will do.
#define POV_DIR_LTR   0  // viewers glance left -> right (sign / cue on the left)
#define POV_DIR_RTL   1  // viewers glance right -> left
#define POV_DIR_ALT   2  // alternate passes: LTR order, gap, RTL order, gap
#define POV_DIR_PAL   3  // palindrome: forward + reverse, then gap (any glance sees
                         // the image next to its mirror)

// ---- Render modes ------------------------------------------------------------
#define RENDER_FULL     0  // 8-bit color through gamma
#define RENDER_PWM_SAFE 1  // each channel fully on or off: no PWM gaps inside a column
#define RENDER_CURRENT  2  // SK9822 / HD107S only: PWM pinned on, per-LED brightness
                           // through the 5-bit current field (8 hues x 32 levels)

// ---- Image record ------------------------------------------------------------
// data: column-major RGB888. Column c starts at data + c*rows*3.
//       Row 0 is the TOP of the image.
typedef struct {
  const uint8_t *data;
  uint16_t rows;      // pixels along the strip
  uint16_t cols;      // time steps per pass
  uint32_t col_us;    // design column period
  uint32_t gap_us;    // dark time after each pass
  uint8_t  dir;       // POV_DIR_*
  float    length_m;  // strip length the image was sized for (0 = unknown)
  uint16_t hold_s;    // seconds on screen before the playlist advances
  uint16_t sweep_dps; // eye/head speed across the strip the column period was designed
                      // for, deg/s (0 = unknown: POV_DEFAULT_SWEEP_DPS is assumed)
} PovImage;

#define POV_DEFAULT_SWEEP_DPS 300

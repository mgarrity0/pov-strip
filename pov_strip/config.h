// config.h - hardware and default settings.
// Everything marked (runtime) can also be changed over serial and saved to flash.
// Names carry a POV_ prefix so they cannot collide with FastLED or the core.
#pragma once
#include "pov_types.h"

// ---- Strip -------------------------------------------------------------------
#define POV_CHIP              LED_WS2812B  // see pov_types.h. A clocked strip (APA102,
                                           // SK9822, HD107S) also needs CI on POV_PIN_CLOCK.

#define POV_LENGTH_M          2.0f   // (runtime "len")  1.0 to 5.0 m
#define POV_LEDS_PER_M        60     // (runtime "dens") 30, 60, 96, 144 ...
#define POV_NUM_LEDS_MAX      720    // buffer size: 5 m x 144/m. Raise for denser strips.

#define POV_DATA_IN_AT_BOTTOM 1      // 1: LED 0 is at the bottom of the vertical strip

// ---- Pins: GPIO numbers, not board labels --------------------------------------
// ESP32 DevKit: label D4 = GPIO4. XIAO ESP32S3 labels differ (D4 = GPIO5); its
// usual pair is data 9 (D10), clock 7 (D8).
#define POV_PIN_DATA          4      // D4
#define POV_PIN_CLOCK         18     // D18, clocked chips only

// ---- Clocked chips (APA102 / SK9822 / HD107S) -----------------------------------
#define POV_SPI_MHZ           16     // (runtime "mhz") start ~8, raise until the far end
                                     // glitches. SK9822 and HD107S datasheets list 30 MHz
                                     // max; APA102's datasheet gives no limit.
#define POV_CLOCKED_ORDER     "BGR"  // byte order on the wire after the 0xE0 header

// ---- Single-wire chips (WS281x / SK6812) -----------------------------------------
#define POV_FASTLED_ORDER     GRB    // FastLED EOrder token
#define POV_RESET_US          0      // 0 = chip default (80 us SK6812, 280-300 us others)

// ---- Rendering ------------------------------------------------------------------
#define POV_BRIGHTNESS        255    // (runtime "bri")
#define POV_GAMMA             2.4f   // (runtime "gamma")
#define POV_RENDER            RENDER_FULL  // (runtime "mode")

// ---- Timing defaults --------------------------------------------------------------
// The image header carries the column period, gap and direction it was designed
// for. These override it (runtime "col" / "gap" / "dir").
#define POV_COL_US_OVERRIDE   0      // 0 = use the image's column period
#define POV_GAP_US_OVERRIDE   -1     // -1 = use the image's gap (0 is a valid gap)
#define POV_DIR_OVERRIDE      -1     // -1 = use the image's direction, else POV_DIR_*

// Keep the image's aspect when the strip length changes at runtime by
// resampling the column count (1), or play the stored columns as-is (0).
#define POV_ASPECT_LOCK       1

#define POV_SERIAL_BAUD       115200

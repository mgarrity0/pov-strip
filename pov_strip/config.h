// config.h - hardware and default settings.
// Everything marked (runtime) can also be changed over serial and saved to flash.
// Names carry a POV_ prefix so they cannot collide with the esp32 core.
//
// Each value below is a default: a config_local.h next to this file (written by the
// Studio's Flash button from its Chip / Pins controls, not tracked by git) overrides
// any of them with its own #defines. Arduino IDE users can edit either file.
#pragma once
#include "pov_types.h"

#if defined(__has_include)
  #if __has_include("config_local.h")
    #include "config_local.h"
  #endif
#endif

// ---- Strip -------------------------------------------------------------------
#ifndef POV_CHIP
#define POV_CHIP              LED_WS2812B  // see pov_types.h. A clocked strip (APA102,
                                           // SK9822, HD107S) also needs CI on POV_PIN_CLOCK.
#endif
#ifndef POV_LENGTH_M
#define POV_LENGTH_M          2.0f   // (runtime "len")  1.0 to 5.0 m
#endif
#ifndef POV_LEDS_PER_M
#define POV_LEDS_PER_M        60     // (runtime "dens") 30, 60, 96, 144 ...
#endif
#ifndef POV_NUM_LEDS_MAX
#define POV_NUM_LEDS_MAX      720    // buffer size: 5 m x 144/m. Raise for denser strips.
#endif
#ifndef POV_DATA_IN_AT_BOTTOM
#define POV_DATA_IN_AT_BOTTOM 1      // 1: LED 0 is at the bottom of the vertical strip
#endif

// ---- Pins: GPIO numbers, not board labels --------------------------------------
// ESP32 DevKit: label D4 = GPIO4. XIAO ESP32S3 labels differ (D4 = GPIO5); its
// usual pair is data 9 (D10), clock 7 (D8). Adafruit Sparkle Motion: screw terminal
// 1 = GPIO21 (data), 2 = GPIO22 (clock), 3 = GPIO19, pads = GPIO23, all level-shifted.
#ifndef POV_PIN_DATA
#define POV_PIN_DATA          4      // D4
#endif
#ifndef POV_PIN_CLOCK
#define POV_PIN_CLOCK         18     // D18, clocked chips only
#endif

// ---- Clocked chips (APA102 / SK9822 / HD107S) -----------------------------------
#ifndef POV_SPI_MHZ
#define POV_SPI_MHZ           16     // (runtime "mhz") start ~8, raise until the far end
                                     // glitches. SK9822 and HD107S datasheets list 30 MHz
                                     // max; APA102's datasheet gives no limit.
#endif
#ifndef POV_CLOCKED_ORDER
#define POV_CLOCKED_ORDER     "BGR"  // byte order on the wire after the 0xE0 header
#endif

// ---- Single-wire chips (WS281x / SK6812) -----------------------------------------
#ifndef POV_SINGLE_ORDER
#define POV_SINGLE_ORDER      "GRB"  // byte order on the wire (WS2812B/SK6812 GRB, most WS2811 RGB)
#endif
#ifndef POV_RESET_US
#define POV_RESET_US          0      // latch (reset) time; 0 = chip default (80 us SK6812, 280-300 us others)
#endif

// ---- Rendering ------------------------------------------------------------------
#ifndef POV_BRIGHTNESS
#define POV_BRIGHTNESS        255    // (runtime "bri")
#endif
#ifndef POV_GAMMA
#define POV_GAMMA             2.4f   // (runtime "gamma")
#endif
#ifndef POV_RENDER
#define POV_RENDER            RENDER_FULL  // (runtime "mode")
#endif

// ---- Timing defaults --------------------------------------------------------------
// The image header carries the column period, gap and direction it was designed
// for. These override it (runtime "col" / "gap" / "dir").
#ifndef POV_COL_US_OVERRIDE
#define POV_COL_US_OVERRIDE   0      // 0 = use the image's column period
#endif
#ifndef POV_GAP_US_OVERRIDE
#define POV_GAP_US_OVERRIDE   -1     // -1 = use the image's gap (0 is a valid gap)
#endif
#ifndef POV_DIR_OVERRIDE
#define POV_DIR_OVERRIDE      -1     // -1 = use the image's direction, else POV_DIR_*
#endif

// Keep the image's aspect when the strip length changes at runtime by
// resampling the column count (1), or play the stored columns as-is (0).
#ifndef POV_ASPECT_LOCK
#define POV_ASPECT_LOCK       1
#endif

#ifndef POV_MA_PER_CHANNEL
#define POV_MA_PER_CHANNEL    20     // one LED channel at full, for the current limit ("amps"):
                                     // 5050 parts ~20 mA, 2020 parts ~12 mA
#endif

#ifndef POV_SERIAL_BAUD
#define POV_SERIAL_BAUD       115200
#endif

# pov-strip: notes for working on this repo

Stationary eye-scan POV on one LED strip. ESP32 firmware (Arduino, `pov_strip/`), a Python
host tool (`povstrip/`), and a browser Studio (`studio/`). README.md has the physics and
the user-facing workflow; this file has the invariants that must stay true across the three
implementations.

## Invariants

- Pixel data is column-major RGB888: column c starts at `c*rows*3`, row 0 is the top of the
  image. Same layout in `pov_image.h`, `.pov` files (48-byte header, `pov_store.h`), and
  `POV.areaResample` output.
- `POV_DIR_LTR` means viewers glance left to right, so the firmware plays the right-most
  column first (`runCycle`). `studio/core.js` `columnAt` must agree.
- Column period = max(design period, chip minimum). Chip minimum: clocked
  `frameBytes(n)*8/spiHz + 25 us`; single-wire `n*24*bitNs + reset + 25 us`. The three
  copies: `led_out.h minColUs`, `core.js chipMinColUs`, `povstrip/core.py chip_min_col_us`.
- Image width is preserved across column periods: displayed columns =
  `im.cols * im.col_us / colUs * (lengthM / im.length_m)` (`applyImage`).
- Resampling when the display grid differs from the stored image: area average in linear
  light through the output gamma, integer-overlap weights. `resampleImage` (C),
  `POV.areaResample` (JS), `core.area_resample` (Python) must agree within 2/255:
  run `cd tools/check_resampler && node check.js` and `python3 test/test_core_vs_js.py`
  if present.
- Serial console: every command ends with a line `ok`, `ok <n>` or `err <reason>`; boot ends
  with `ready`. `put` protocol is specified at the top of `pov_store.h`; `povstrip/protocol.py`
  and `povstrip/fakedev.py` implement both ends.
- PWM-safe mode drives channels fully on or off; brightness only applies through the 5-bit
  current field on SK9822/HD107S. `led_out.h setRender` and `core.js makeRender` match.
- config.h macros are prefixed `POV_` so they cannot collide with the esp32 core.
- The firmware depends on the esp32 Arduino core only (ESP-IDF SPI master and RMT drivers); no
  LED library. FastLED was dropped because its RMT driver fixes the LED count at `addLeds()`.

## Building and testing

- Firmware: `povstrip flash --build-only` (arduino-cli) or Arduino IDE. Compiles warning-free
  at `-Wall -Wextra` for ESP32 and ESP32-S3; keep it that way.
- Studio: edit `studio/studio.src.html` and `studio/core.js`, then `python3 tools/build_studio.py`
  (inlines core.js and the firmware sources for the project-zip export). Never edit
  `studio/pov-strip-studio.html` by hand.
- Demo image for the shipped config: `node tools/gen_default.js`.
- Tests: `node test/core_test.js`, `python3 test/test_protocol.py`, `povstrip serve --fake`.
- Hardware so far (2026-10-11): ESP32 DevKit (CH340) with WS2812B. Clocked chips and the other
  single-wire chips are untested on hardware.

## Style

- No em dashes anywhere (code comments, docs, UI text).
- Terse technical prose; numbers over adjectives.

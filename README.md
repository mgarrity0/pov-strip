# pov-strip

Stationary eye-scan persistence of vision on one LED strip, 1 to 5 m. The strip stays still and flashes one image column every few hundred microseconds. A viewer who glances past it, or shakes their head, sweeps those flashes across the retina and sees the picture for an instant.

Images live in the ESP32's own flash and go in over the USB cable. Flash the firmware once; after that everything is done from the `povstrip` tool and the Studio page it serves.

## Setup (once)

```
git clone https://github.com/mgarrity0/pov-strip.git
cd pov-strip
```

Then use the launcher, which makes its own Python environment the first time: `povstrip.cmd ...` on Windows (ports are `COM5` style), `./povstrip.sh ...` on macOS/Linux. The commands below are written as `povstrip ...`; prefix accordingly. (`pip install -e .` also works if you prefer a global `povstrip` command.) `povstrip ports` lists serial ports.

Firmware, any of these:

- The **Flash firmware** button in the Studio's "Strip over USB" panel (`povstrip serve`, or double-click `serve.cmd` on Windows).
- `povstrip flash --serial COM5` (or `/dev/cu.usbserial-XXXX`).
- Arduino IDE: open `pov_strip/pov_strip.ino`, board "ESP32 Dev Module", upload. The Arduino IDE section below has the details.

The first two use arduino-cli from PATH or the copy inside Arduino IDE 2, and install the esp32 core themselves if it is missing. The firmware needs no libraries.

`pov_strip/config.h` holds the hardware facts: chip (`POV_CHIP`, WS2812B by default), data pin (GPIO4 = D4 on a DevKit), clock pin for clocked chips, LEDs per meter, length. Change those, reflash. Everything else changes live.

## Every day

```
povstrip serve
```

opens the Studio in your browser with a "Strip over USB" panel: pick the port, Connect, tick the slides you want, **Send ticked slides to strip**. The page takes the strip's chip, length and density from the board when it connects, so the preview matches what is plugged in. Nothing is recompiled; the strip starts playing the new images as they arrive and keeps them across power cycles.

Preview: Glance and Shake simulate what the eye sees; **Strip** shows the line itself playing its columns at the playback speed (a 60 Hz screen merges columns shorter than a frame). At 1/20 speed you can sweep your own eyes across it at about 15°/s and see the picture, the same mechanism slowed down 20x.

Live tuning: once connected, the "Live on the strip" sliders (sweep speed, dark gap, brightness, direction) change the strip as you drag, no re-upload. Sweep speed is how fast the strip crosses your eye; the slides were made for the Eye speed in the design controls (300°/s by default, a glance). For a head shake set it lower: ±8° at 2 Hz peaks near 100°/s. The firmware stretches each pass and the gap by design/actual so the picture keeps its width. Apply strip settings or Send saves the values on the strip.

Without hardware: `povstrip serve --fake` simulates a strip.

Updating: `git pull`, then start `povstrip serve` (or `serve.cmd`) again; it asks the server that is still running to quit and takes its port. The page itself is re-read from disk on every reload, firmware changes need the Flash button, Python changes need that restart.

Command line:

```
povstrip push photo.png --height 0.8 --hold 8     # render for the strip and upload into slot 1
povstrip push logo.png --slot 2 --dir pal --len 2 --dens 60 --chip WS2812B
povstrip ls | info | wipe | rm 2
povstrip cmd len 2.5                              # any console command
povstrip cmd speed 100                            # live: tune for a slower head shake
povstrip monitor                                  # raw serial console
```

## Files

| Path | What |
|---|---|
| `pov_strip/pov_strip.ino` | Column scheduler, playlist, serial console, settings in NVS, resampler |
| `pov_strip/pov_store.h` | Images on LittleFS: `/img/01.pov` .. `/img/32.pov`, the `put` upload protocol |
| `pov_strip/led_out.h` | Clocked chips over ESP-IDF SPI + DMA; single-wire chips over ESP-IDF RMT (bytes encoder). Both double-buffered |
| `pov_strip/config.h` | Chip, length, LEDs/m, pins, render defaults |
| `pov_strip/pov_types.h` | Chip, direction and render constants, `PovImage` record |
| `pov_strip/pov_image.h` | Built-in demo (one heart) played when nothing has been uploaded |
| `povstrip/` | Host tool: `protocol.py` (serial client), `server.py` (Studio + API), `flash.py` (arduino-cli), `core.py` (design math, resampler), `fakedev.py` (simulated strip) |
| `studio/studio.src.html`, `studio/core.js` | The Studio page source; `tools/build_studio.py` builds `studio/pov-strip-studio.html` |
| `tools/`, `test/` | Build scripts, resampler cross-check, protocol test |

Chips: APA102, SK9822, HD107S (data + clock); WS2812B, WS2812B-V5, SK6812 (RGB), WS2811 (800 kHz), WS2813, WS2815 (single wire).

## Wiring

- Defaults: data GPIO4 (D4 on an ESP32 DevKit), clock GPIO18 (D18, clocked chips only). XIAO ESP32S3: data 9 (D10), clock 7 (D8); its D4 label is GPIO5.
- Level-shift data and clock to 5 V (74AHCT125 on 5 V). Keep the MCU-to-strip lead short at 16 MHz and above; a 33 to 100 ohm series resistor at the driver helps.
- `POV_DATA_IN_AT_BOTTOM 1` if the controller sits at the base of a vertical strip.
- 5 m strips: inject 5 V at both ends. A full-white column on 300 LEDs at 20 mA per channel is 18 A; POV duty is low but the column peak is real.

## The numbers that matter

Square pixels need one column per LED-angle of eye travel:

    column period = (LED pitch / viewing distance) / eye speed
    2 m x 60/m, viewer at 5 m, eye at 300 deg/s  ->  637 us (1.57 kHz)

Time to clock one column out:

| Chip | Per column | 120 LEDs | 300 LEDs |
|---|---|---|---|
| APA102 / SK9822 / HD107S | 32 bits per LED + frames, at the SPI clock | 275 us at 16 MHz | 639 us |
| WS2812B | 30 us per LED + 280 us latch | 3.9 ms (256 columns/s) | 9.3 ms |

When the chip is slower than the design, the firmware plays fewer, longer columns so the image keeps its width (lower horizontal resolution). On 120 WS2812B LEDs the heart is 16 columns wide. Single-wire chips suit short, low-density strips; past about 1 m x 60/m use a clocked chip.

Single-wire bit timings (high/low ns for a 0 bit, for a 1 bit; latch): WS2812B 400/850, 800/450, 280 us (datasheet nominal, the values Adafruit_NeoPixel uses on ESP32); WS2812B-V5 300/925, 600/625, 280 us (inside the V5 datasheet ranges); SK6812 300/900, 600/600, 80 us (datasheet nominal); WS2811 250/1000, 600/650, 280 us (datasheet high-speed mode); WS2813 325/950, 650/625, 300 us and WS2815 250/1650, 1350/550, 300 us (FastLED 3.10 timings rounded to 25 ns). The strip latches as part of each column's transmission, so a late column cannot run into the previous one. Change them in `led_out.h` if a strip disagrees; WS2812B is the only chip run on hardware so far.

Resampling: images uploaded from the Studio are already on the strip's grid and play pixel for pixel. If the strip, density or column period differ from what an image was made for (a `len` change, a slower chip), the firmware resamples it once into RAM: each displayed pixel is the area average of the stored pixels it covers, in linear light through the output gamma, so a thin line keeps its share of the light instead of vanishing. `info` says when this is happening.

PWM: a column shorter than a few PWM periods shows gaps in mid-tones. APA102 PWM is ~19.2 kHz; SK9822 measured 4.7 kHz (one datasheet says 1.2 kHz); WS2812 ~430 Hz on older parts, WS2812B-V5 2 kHz; SK6812 1.1 kHz. On WS2812B at 3.9 ms columns that is 1.7 cycles per column: the Studio preview shows the dashes.

- `mode safe`: each channel fully on or off, so no PWM gaps. `bri` has no effect on chips without current control in this mode.
- `mode current` (SK9822, HD107S): PWM pinned on, brightness through the 5-bit current field. 8 hues x 32 levels.
- APA102's 5-bit global brightness is a second, slow PWM (~440 Hz), so the firmware keeps it at 31 and dims in the 8-bit value.

Direction: a left-to-right glance paints later columns further left, so `ltr` plays the right-most column first. Glancing the other way shows a mirror image. `alt` alternates passes; `pal` plays the image then its mirror so either direction catches a correct copy.

Gap: after each pass the strip goes dark. With pass T, gap G and glance duration D, the chance one glance shows exactly one whole image is (D - T)/(T + G) for G >= D - T; the Studio sets G = D - T per slide.

Sweep speed: an image is painted across the retina at the viewer's angular speed, so its width is pass time x speed. Each image carries the speed it was designed for (header byte 17, `sweep_dps` in `PovImage`). `speed <deg/s>` tells the firmware the real speed; it scales the wanted column period and the image's gap by design/actual, and the column count follows (`applyImage`), so a slower head shake gets a longer pass of the same angular width, using more of the stored columns when the chip had been the limit.

Viewers resolve flicker during saccades up to a mean of ~2 kHz (Roberts & Wilkins 2013), so column rates far above that blur horizontally.

## Serial console

115200 baud, line ending New Line. Every command ends with `ok` or `err <reason>`, which is what the tool keys on.

    info                 numbers for the current setup
    len <m>              strip length, e.g. len 3.5 (columns rescale to keep the aspect)
    dens <n>             LEDs per meter
    col <us> | col 0     column period override | image's
    gap <us> | gap img   dark time after each pass
    speed <deg/s> | speed img   how fast the eye or head crosses the strip (live tuning:
                         slower than the design speed stretches pass and gap, see below)
    dir ltr|rtl|alt|pal|img
    bri <0-255>  gamma <x>  mode full|safe|current
    img <n> | next       select image; auto on|off for the playlist
    mhz <f>              SPI clock (clocked chips)
    test | test off      static frame: 3 red LEDs at the data-in end, then R/G/B/W quarters
    save | load | reset  settings in flash (dropped automatically when config.h changes)
    ls | rm <n> | wipe   stored images
    put <slot> <bytes> <crc32>   upload (the tool does this; format in pov_store.h)

## Arduino IDE

1. File > Preferences > Additional boards manager URLs: `https://espressif.github.io/arduino-esp32/package_esp32_index.json`
2. Tools > Board > Boards Manager: install **esp32** by Espressif Systems, version 3.x.
3. Open `pov_strip/pov_strip.ino`. Tools > Board > esp32 > **ESP32 Dev Module**, Partition Scheme **Huge APP**. Upload. No libraries to install.
4. Tools > Serial Monitor: 115200 baud, line ending **New Line**. Type `help`.

Images stored on the strip use the partition's SPIFFS area (1.5 MB with the default scheme, 1 MB with Huge APP), so the partition scheme no longer matters for image count unless you compile images in.

Built with arduino-cli 1.5.1 (the build engine inside Arduino IDE 2) and esp32 3.3.12, Huge APP, no warnings from the sketch at `-Wall -Wextra`: ESP32 Dev Module 359,408 bytes (WS2812B) / 370,848 (APA102); ESP32-S3 365,920 / 375,568. The firmware's resampler, the Studio's and the Python tool's were run on the same inputs (`tools/check_resampler`). The serial protocol was exercised against the simulated strip (`test/`). Hardware so far: an ESP32 DevKit (CH340) driving WS2812B.

Why not FastLED: its ESP32 RMT driver (3.10 and later) fixes the strip length at `addLeds()`, and this firmware changes the active LED count at runtime (`len`, `dens`), so every column failed an assert once the two differed. Driving the RMT peripheral directly removes the dependency and the version drift.

## PlatformIO

`pio run -t upload`, then `pio device monitor` (115200). Default env `esp32dev` uses the Huge APP partition. Envs for `xiao_esp32s3` and `esp32s3_devkitc` too.

## Sources

- cpldcpu, SK9822 vs APA102 (PWM, global brightness, latching, frame format): https://cpldcpu.com/2016/12/13/sk9822-a-clone-of-the-apa102/
- cpldcpu, SK6812 (PWM vs WS2812): https://cpldcpu.com/2016/03/09/the-sk6812-another-intelligent-rgb-led/
- WS2812B-V5 datasheet: https://learn.watterott.com/kb/ws281x/WS2812B-v5.pdf
- SK9822 datasheet: https://yggdrasil.ti.bfh.ch/_static/datasheets/yggdrasil/SK9822.pdf
- HD107S datasheet: https://www.superlightingled.com/PDF/HD107S-Datasheet.pdf
- Roberts & Wilkins 2013, flicker during saccades above 1 kHz: https://repository.essex.ac.uk/3842/
- ESP-IDF SPI master (GPIO matrix limits, DMA): https://docs.espressif.com/projects/esp-idf/en/stable/esp32s3/api-reference/peripherals/spi_master.html

// pov_strip.ino - stationary eye-scan (saccade) persistence of vision on one LED strip.
//
// The strip stands still and flashes one image column at a time, a few hundred
// microseconds each. A viewer who glances past it (or shakes their head) sweeps
// those flashes across the retina and sees the whole image for an instant.
//
// Hardware and defaults: config.h. Images: uploaded over serial into the ESP32's
// own flash (pov_store.h, used by the povstrip tool), or compiled in from
// pov_image.h when nothing has been uploaded.
// Serial console (115200): type "help". Every command ends with "ok" or "err ...".
#include <Arduino.h>
#include <Preferences.h>
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "config.h"
#include "pov_image.h"
#include "pov_store.h"
#include "led_out.h"

#define POV_VERSION "2.1"
#define COMPILED_COUNT ((uint16_t)(sizeof(POV_IMAGES) / sizeof(POV_IMAGES[0])))
static const uint32_t MAX_COLS = 8192;

// ---- settings (defaults from config.h, overridable over serial, saved in NVS) ----
struct Settings {
  float lengthM;
  uint16_t density;
  uint32_t colUs;    // 0 = image's value
  int32_t gapUs;     // -1 = image's value
  int8_t dir;        // -1 = image's value
  uint8_t bri;
  float gamma;
  uint8_t mode;
  uint16_t img;
  bool autoAdvance;
  float spiMHz;
  uint16_t sweepDps; // viewer's eye/head speed across the strip; 0 = the image's design speed
};
static Settings S;
static Preferences prefs;

// ---- derived state ---------------------------------------------------------------
static uint16_t g_n = 0;                    // active LEDs
static uint16_t g_rowMap[POV_NUM_LEDS_MAX];     // LED index -> source row
static uint16_t g_identity[POV_NUM_LEDS_MAX];   // LED index -> LED index (test frames)
static uint16_t *g_colMap = nullptr;        // displayed column -> source column
static uint32_t g_cols = 0;                 // displayed columns per pass
// Source of the displayed pixels: the stored image, or a copy resampled to the
// displayed grid (see resampleImage).
static const uint8_t *g_src = nullptr;
static uint16_t g_srcRows = 0, g_srcCols = 0;
static uint8_t *g_resampled = nullptr;
static bool g_nearest = false;              // nearest-pixel mapping in use
static uint32_t g_colUs = 0;                // effective column period
static uint32_t g_wantUs = 0;               // requested column period
static uint32_t g_gapUs = 0;
static float g_stretch = 1.0f;              // pass and gap time scale from the sweep speed
static uint8_t g_dir = POV_DIR_LTR;
static uint8_t g_mode = RENDER_FULL;
static uint16_t g_img = 0;                  // playlist index
static PovImage g_cur;                      // the image being played
static char g_curName[24] = "";
static uint8_t *g_curBuf = nullptr;         // RAM copy of a stored image (freed on switch)
static bool g_curFromStore = false;
static uint32_t g_imgSince = 0;
static int64_t g_deadline = 0;
static int g_buf = 0;
static uint32_t g_late = 0, g_passes = 0;
static int64_t g_lastYield = 0;
static bool g_test = false;
static uint8_t *g_testCol = nullptr;

static const char *dirName(uint8_t d) {
  switch (d) {
    case POV_DIR_LTR: return "ltr";
    case POV_DIR_RTL: return "rtl";
    case POV_DIR_ALT: return "alt";
    case POV_DIR_PAL: return "pal";
  }
  return "?";
}
static const char *modeName(uint8_t m) {
  return m == RENDER_FULL ? "full" : m == RENDER_PWM_SAFE ? "safe" : "current";
}

// ---- settings storage ----------------------------------------------------------
// Settings stored with "save" survive uploads until config.h or the image set
// changes; then the new defaults win, so edits made in the IDE take effect.
static bool g_savedStale = false;

static uint32_t fnv1a(uint32_t h, const void *p, size_t n) {
  const uint8_t *b = (const uint8_t *)p;
  while (n--) {
    h ^= *b++;
    h *= 16777619u;
  }
  return h;
}

static uint32_t configHash() {
  const float f[] = {(float)POV_LENGTH_M, (float)POV_GAMMA, (float)POV_SPI_MHZ};
  const int32_t v[] = {POV_CHIP, POV_LEDS_PER_M, POV_COL_US_OVERRIDE, POV_GAP_US_OVERRIDE,
                       POV_DIR_OVERRIDE, POV_BRIGHTNESS, POV_RENDER, POV_PIN_DATA, POV_PIN_CLOCK};
  uint32_t h = fnv1a(2166136261u, f, sizeof f);
  return fnv1a(h, v, sizeof v);
}

static void defaults() {
  S.lengthM = POV_LENGTH_M;
  S.density = POV_LEDS_PER_M;
  S.colUs = POV_COL_US_OVERRIDE;
  S.gapUs = POV_GAP_US_OVERRIDE;
  S.dir = POV_DIR_OVERRIDE;
  S.bri = POV_BRIGHTNESS;
  S.gamma = POV_GAMMA;
  S.mode = POV_RENDER;
  S.img = 0;
  S.autoAdvance = true;
  S.spiMHz = POV_SPI_MHZ;
  S.sweepDps = 0;
}

static void loadSettings() {
  defaults();
  g_savedStale = false;
  if (!prefs.begin("pov", true)) return;  // nothing saved yet
  if (prefs.getUInt("cfg", 0) != configHash()) {
    g_savedStale = true;  // saved under a different config.h or image set
    prefs.end();
    return;
  }
  S.lengthM = prefs.getFloat("len", S.lengthM);
  S.density = prefs.getUShort("dens", S.density);
  S.colUs = prefs.getULong("col", S.colUs);
  S.gapUs = prefs.getLong("gap", S.gapUs);
  S.dir = prefs.getChar("dir", S.dir);
  S.bri = prefs.getUChar("bri", S.bri);
  S.gamma = prefs.getFloat("gam", S.gamma);
  S.mode = prefs.getUChar("mode", S.mode);
  S.img = prefs.getUShort("img", S.img);
  S.autoAdvance = prefs.getBool("auto", S.autoAdvance);
  S.spiMHz = prefs.getFloat("mhz", S.spiMHz);
  S.sweepDps = prefs.getUShort("spd", S.sweepDps);
  prefs.end();
}

static void saveSettings() {
  prefs.begin("pov", false);
  prefs.putFloat("len", S.lengthM);
  prefs.putUShort("dens", S.density);
  prefs.putULong("col", S.colUs);
  prefs.putLong("gap", S.gapUs);
  prefs.putChar("dir", S.dir);
  prefs.putUChar("bri", S.bri);
  prefs.putFloat("gam", S.gamma);
  prefs.putUChar("mode", S.mode);
  prefs.putUShort("img", g_img);
  prefs.putBool("auto", S.autoAdvance);
  prefs.putFloat("mhz", S.spiMHz);
  prefs.putUShort("spd", S.sweepDps);
  prefs.putUInt("cfg", configHash());
  prefs.end();
}

static void clearSettings() {
  prefs.begin("pov", false);
  prefs.clear();
  prefs.end();
}

// ---- geometry and timing ----------------------------------------------------------
static uint16_t ledsFor(float lengthM, uint16_t density) {
  long n = lroundf(lengthM * density);
  if (n < 1) n = 1;
  if (n > POV_NUM_LEDS_MAX) n = POV_NUM_LEDS_MAX;
  return (uint16_t)n;
}

// ---- resampling -----------------------------------------------------------------------
// When the displayed grid (LEDs x columns) is smaller than the stored image in either
// direction, picking the nearest stored pixel drops whole rows or columns: a thin line
// can vanish or show at full strength depending on where it falls. Instead the image
// is resampled once into RAM: each displayed pixel is the area average of the stored
// pixels it covers, taken in linear light (through the output gamma) so the emitted
// energy is kept. A thin line then shows at reduced strength instead of disappearing.
// Pure upsampling (a longer strip) just repeats pixels and needs no RAM.
static uint16_t s_lin16[256];

static uint8_t encode16(uint32_t lin) {  // inverse of s_lin16, nearest entry
  uint16_t lo = 0, hi = 255;
  while (lo < hi) {
    const uint16_t mid = (uint16_t)((lo + hi) / 2);
    if (s_lin16[mid] < lin) lo = (uint16_t)(mid + 1);
    else hi = mid;
  }
  if (lo > 0 && lin - s_lin16[lo - 1] < s_lin16[lo] - lin) lo--;
  return (uint8_t)lo;
}

static void resampleImage(const PovImage &im, uint8_t *dst, uint16_t rows, uint32_t cols, float gamma) {
  if (gamma < 0.5f) gamma = 0.5f;
  for (int v = 0; v < 256; ++v) s_lin16[v] = (uint16_t)lroundf(powf(v / 255.0f, gamma) * 65535.0f);
  const uint32_t SC = im.cols, SR = im.rows;
  const uint64_t norm = (uint64_t)SC * SR;
  // Displayed column kc covers stored columns [kc*SC/cols, (kc+1)*SC/cols); the same
  // for rows. In units of 1/cols (1/rows) every overlap is an integer.
  for (uint32_t kc = 0; kc < cols; ++kc) {
    const uint64_t a = (uint64_t)kc * SC, b = (uint64_t)(kc + 1) * SC;
    const uint32_t i0 = (uint32_t)(a / cols), i1 = (uint32_t)((b - 1) / cols);
    for (uint32_t kr = 0; kr < rows; ++kr) {
      const uint64_t ya = (uint64_t)kr * SR, yb = (uint64_t)(kr + 1) * SR;
      const uint32_t j0 = (uint32_t)(ya / rows), j1 = (uint32_t)((yb - 1) / rows);
      uint64_t acc[3] = {0, 0, 0};
      for (uint32_t j = j0; j <= j1; ++j) {
        const uint64_t lo = (uint64_t)j * rows, hi = lo + rows;
        const uint64_t wy = (yb < hi ? yb : hi) - (ya > lo ? ya : lo);
        for (uint32_t i = i0; i <= i1; ++i) {
          const uint64_t clo = (uint64_t)i * cols, chi = clo + cols;
          const uint64_t w = wy * ((b < chi ? b : chi) - (a > clo ? a : clo));
          const uint8_t *p = im.data + ((uint32_t)i * SR + j) * 3u;
          acc[0] += w * s_lin16[p[0]];
          acc[1] += w * s_lin16[p[1]];
          acc[2] += w * s_lin16[p[2]];
        }
      }
      uint8_t *o = dst + (kc * rows + kr) * 3u;
      o[0] = encode16((uint32_t)(acc[0] / norm));
      o[1] = encode16((uint32_t)(acc[1] / norm));
      o[2] = encode16((uint32_t)(acc[2] / norm));
    }
  }
}

// Rebuilds the pixel source, row/column maps and timing for the current image and strip.
static void applyImage() {
  const PovImage &im = g_cur;

  // Sweep speed: the image spans im.cols * im.col_us of eye travel at its design
  // speed. A viewer moving slower (a head shake) needs the pass stretched by
  // design / actual so the picture keeps its width on the retina; the gap scales
  // with it so the cycle stays in step with the motion.
  const uint16_t designDps = im.sweep_dps ? im.sweep_dps : POV_DEFAULT_SWEEP_DPS;
  g_stretch = S.sweepDps ? (float)designDps / (float)S.sweepDps : 1.0f;

  // Timing: never faster than the chip can clock out a column.
  g_wantUs = S.colUs ? S.colUs : (uint32_t)lroundf(im.col_us * g_stretch);
  if (g_wantUs < 1) g_wantUs = 1;
  const uint32_t minUs = ledout::minColUs();
  g_colUs = g_wantUs < minUs ? minUs : g_wantUs;
  g_gapUs = S.gapUs >= 0 ? (uint32_t)S.gapUs : (uint32_t)lroundf(im.gap_us * g_stretch);
  g_dir = S.dir >= 0 ? (uint8_t)S.dir : im.dir;

  // Columns: keep the designed width and aspect. The image spans
  // im.cols * im.col_us * stretch of time; at another column period that takes a
  // different number of columns. A longer strip is taller, so it needs
  // proportionally more columns.
  double cols = im.cols;
#if POV_ASPECT_LOCK
  if (im.length_m > 0.01f) cols *= S.lengthM / im.length_m;
  if (im.col_us > 0) cols *= (double)im.col_us * g_stretch / (double)g_colUs;
#endif
  uint32_t c = (uint32_t)lround(cols);
  if (c < 1) c = 1;
  if (c > MAX_COLS) c = MAX_COLS;
  if (c != g_cols || !g_colMap) {
    free(g_colMap);
    g_colMap = (uint16_t *)malloc(c * sizeof(uint16_t));
    if (!g_colMap) {  // out of RAM: play stored columns 1:1
      c = im.cols;
      g_colMap = (uint16_t *)malloc(c * sizeof(uint16_t));
    }
  }
  g_cols = c;

  // Pixel source: the stored image, or a resampled copy when the display grid is
  // smaller in either direction.
  free(g_resampled);
  g_resampled = nullptr;
  g_nearest = false;
  g_src = im.data;
  g_srcRows = im.rows;
  g_srcCols = im.cols;
  if (g_n != im.rows || c != im.cols) {
    if (g_n < im.rows || c < im.cols) {
      const size_t need = (size_t)g_n * c * 3u;
      uint8_t *buf = (uint8_t *)heap_caps_malloc(need, MALLOC_CAP_SPIRAM);
      if (!buf && heap_caps_get_largest_free_block(MALLOC_CAP_8BIT) > need + 32768u)
        buf = (uint8_t *)malloc(need);
      if (buf) {
        resampleImage(im, buf, g_n, c, S.gamma);
        g_resampled = buf;
        g_src = buf;
        g_srcRows = g_n;
        g_srcCols = (uint16_t)c;
      } else {
        g_nearest = true;  // not enough RAM: rows/columns get skipped
      }
    } else {
      g_nearest = true;    // upsampling only: repeating pixels loses nothing
    }
  }

  // Maps from LED / displayed column to the source. Identity (with the data-in
  // orientation) when the source is the resampled copy.
  for (uint16_t i = 0; i < g_n; ++i) {
    const uint32_t pos = POV_DATA_IN_AT_BOTTOM ? (uint32_t)(g_n - 1 - i) : i;
    uint32_t r = ((2u * pos + 1u) * g_srcRows) / (2u * g_n);
    if (r >= g_srcRows) r = g_srcRows - 1;
    g_rowMap[i] = (uint16_t)r;
  }
  for (uint32_t k = 0; k < c; ++k) {
    uint32_t s = ((2u * k + 1u) * g_srcCols) / (2u * c);
    if (s >= g_srcCols) s = g_srcCols - 1;
    g_colMap[k] = (uint16_t)s;
  }
}

static void applyStrip() {
  const uint16_t n = ledsFor(S.lengthM, S.density);
  if (n < g_n) ledout::blankAll();  // clear LEDs past the new end
  g_n = n;
  ledout::setCount(n);
  applyImage();
}

static void applyRender() {
  g_mode = ledout::setRender(S.mode, S.bri, S.gamma);
  if (g_resampled) applyImage();  // the resampled copy depends on gamma
}

// ---- playlist ----------------------------------------------------------------------
static uint16_t imageCount() { return povstore::count() ? povstore::count() : COMPILED_COUNT; }

// Makes playlist entry i the current image: a stored slot when any exist, else a
// compiled-in image. A stored slot that cannot be read falls back to the compiled set.
static void selectImage(uint16_t i) {
  const uint16_t n = imageCount();
  g_img = n ? (uint16_t)(i % n) : 0;
  g_imgSince = millis();
  free(g_resampled);
  g_resampled = nullptr;
  g_src = nullptr;
  free(g_curBuf);
  g_curBuf = nullptr;
  g_curFromStore = false;
  strcpy(g_curName, "");
  if (povstore::count()) {
    const char *why = "";
    if (povstore::load(povstore::slotAt((uint8_t)g_img), g_cur, g_curName, &g_curBuf, &why)) {
      g_curFromStore = true;
    } else {
      Serial.printf("slot %u failed to load (%s); playing the compiled-in image\n",
                    (unsigned)povstore::slotAt((uint8_t)g_img), why);
      g_cur = POV_IMAGES[0];
      strncpy(g_curName, "compiled-in", sizeof g_curName);
    }
  } else {
    g_cur = POV_IMAGES[g_img % COMPILED_COUNT];
    strncpy(g_curName, "compiled-in", sizeof g_curName);
  }
  applyImage();
}

// ---- engine -----------------------------------------------------------------------
static inline void spinUntil(int64_t t) {
  while (esp_timer_get_time() < t) {
  }
}

static inline const uint8_t *colPtr(const PovImage &, int k) {
  return g_src + (uint32_t)g_colMap[k] * g_srcRows * 3u;
}

// Plays displayed columns start, start+step, ... `count` of them, one per
// column period. Column k+1 is encoded while column k is on the wire.
static void pass(const PovImage &im, int start, int step, int count, bool blankEnd) {
  int b = g_buf;
  if (count <= 0) {
    if (blankEnd) {
      spinUntil(g_deadline);
      ledout::build(b, nullptr, g_rowMap);
      ledout::send(b);
      ledout::waitDone();
    }
    return;
  }
  ledout::build(b, colPtr(im, start), g_rowMap);
  for (int k = 0; k < count; ++k) {
    spinUntil(g_deadline);
    if (esp_timer_get_time() - g_deadline > (int64_t)(g_colUs / 4)) g_late++;
    ledout::send(b);
    if (k + 1 < count) ledout::build(b ^ 1, colPtr(im, start + step * (k + 1)), g_rowMap);
    else if (blankEnd) ledout::build(b ^ 1, nullptr, g_rowMap);
    ledout::waitDone();
    b ^= 1;
    g_deadline += g_colUs;
  }
  if (blankEnd) {  // last column gets its full period, then dark
    spinUntil(g_deadline);
    ledout::send(b);
    ledout::waitDone();
  }
  g_buf = b;
}

static void gapWait() {
  const int64_t end = g_deadline + g_gapUs;
  const int64_t now = esp_timer_get_time();
  if (end - now > 3000) {  // strip is dark: give the CPU back
    vTaskDelay(pdMS_TO_TICKS((uint32_t)((end - now - 2000) / 1000)));
    g_lastYield = esp_timer_get_time();
  }
  spinUntil(end);
  g_deadline = end;
}

static void runCycle() {
  const PovImage &im = g_cur;
  const int c = (int)g_cols;
  const int64_t now = esp_timer_get_time();
  if (now - g_deadline > 2000) g_deadline = now + 50;  // fell behind: restart the clock
  switch (g_dir) {
    case POV_DIR_RTL:
      pass(im, 0, 1, c, true);
      gapWait();
      break;
    case POV_DIR_ALT:
      pass(im, c - 1, -1, c, true);
      gapWait();
      pass(im, 0, 1, c, true);
      gapWait();
      break;
    case POV_DIR_PAL:
      pass(im, 0, 1, c, false);
      pass(im, c - 2, -1, c - 1, true);
      gapWait();
      break;
    default:  // POV_DIR_LTR: glance left -> right, so play right column first
      pass(im, c - 1, -1, c, true);
      gapWait();
      break;
  }
  g_passes++;
}

static void advancePlaylist() {
  if (imageCount() < 2 || !S.autoAdvance) return;
  const uint16_t hold = g_cur.hold_s;
  if (hold && millis() - g_imgSince >= (uint32_t)hold * 1000u) selectImage(g_img + 1);
}

// ---- static test frame: wiring, chip type and color order -------------------------
// Data-in end: 3 LEDs red, then quarters red / green / blue / white at ~25%.
static void showTest() {
  if (!g_testCol) g_testCol = (uint8_t *)malloc(POV_NUM_LEDS_MAX * 3);
  if (!g_testCol) return;
  for (uint16_t i = 0; i < g_n; ++i) {
    uint8_t *p = g_testCol + i * 3u;
    const uint8_t q = (uint8_t)((4u * i) / g_n);
    const uint8_t v = 140;  // ~25% after gamma 2.4
    p[0] = (q == 0 || q == 3) ? v : 0;
    p[1] = (q == 1 || q == 3) ? v : 0;
    p[2] = (q == 2 || q == 3) ? v : 0;
    if (i < 3) { p[0] = 255; p[1] = 0; p[2] = 0; }
  }
  ledout::waitDone();
  ledout::build(0, g_testCol, g_identity);
  ledout::send(0);
  ledout::waitDone();
  g_buf = 1;
}

// ---- console ------------------------------------------------------------------------
static void printInfo() {
  const PovImage &im = g_cur;
  const uint32_t minUs = ledout::minColUs();
  Serial.printf("\n-- POV strip %s --\nchip %s (%s)", POV_VERSION, ledout::chipName(),
                LED_IS_CLOCKED(POV_CHIP) ? "clocked" : "single-wire");
#if LED_IS_CLOCKED(POV_CHIP)
  Serial.printf(", SPI %.2f MHz asked, %.2f MHz actual", S.spiMHz, ledout::actualHz() / 1e6);
#endif
  Serial.printf("\nstrip %.2f m x %u/m = %u LEDs (max %u)\n", S.lengthM, S.density, g_n,
                (unsigned)POV_NUM_LEDS_MAX);
  Serial.printf("wire %lu bits per column -> min column %lu us (%.0f columns/s max)\n",
                (unsigned long)ledout::wireBits(), (unsigned long)minUs, 1e6 / minUs);
  Serial.printf("image %u/%u%s: %s, %u rows x %u cols, sized for %.2f m, hold %u s\n", g_img + 1,
                imageCount(), g_curFromStore ? "" : " (compiled-in)", g_curName, im.rows, im.cols,
                im.length_m, im.hold_s);
  Serial.printf("column %lu us (%.0f Hz); %lu columns per pass = %.1f ms; gap %.1f ms%s\n",
                (unsigned long)g_colUs, 1e6 / g_colUs, (unsigned long)g_cols,
                g_cols * (double)g_colUs / 1000.0, g_gapUs / 1000.0, S.gapUs < 0 ? " (image)" : "");
  if (g_colUs > g_wantUs)
    Serial.printf("  chip-limited: wanted %lu us; fewer, longer columns keep the width\n",
                  (unsigned long)g_wantUs);
  {
    const uint16_t designDps = im.sweep_dps ? im.sweep_dps : POV_DEFAULT_SWEEP_DPS;
    if (S.sweepDps)
      Serial.printf("sweep %u deg/s (image designed for %u%s: pass and gap x%.2f)\n", S.sweepDps, designDps,
                    im.sweep_dps ? "" : " assumed", g_stretch);
    else
      Serial.printf("sweep img: the image's design speed, %u deg/s%s\n", designDps, im.sweep_dps ? "" : " assumed");
  }
  if (g_resampled)
    Serial.printf("  resampled to %u x %lu in RAM (area average); export for this strip to skip that\n",
                  g_n, (unsigned long)g_cols);
  else if (g_nearest && (g_n < im.rows || g_cols < im.cols))
    Serial.println("  WARNING: not enough RAM to resample, rows/columns are being skipped; export for this strip");
  Serial.printf("dir %s%s, render %s, gamma %.2f, bri %u, auto %s\n", dirName(g_dir),
                S.dir < 0 ? " (image)" : "", modeName(g_mode), S.gamma, S.bri,
                S.autoAdvance ? "on" : "off");
  Serial.printf("passes %lu, late columns %lu", (unsigned long)g_passes, (unsigned long)g_late);
  if (ledout::errors()) Serial.printf(", LED output errors %lu", (unsigned long)ledout::errors());
  Serial.println();
}

static void printHelp() {
  Serial.println(
      "\ncommands:\n"
      "  info                 numbers for the current setup\n"
      "  len <m>              strip length, e.g. len 3.5\n"
      "  dens <n>             LEDs per meter\n"
      "  col <us> | col 0     column period override | use image's\n"
      "  gap <us> | gap img   dark time after each pass\n"
      "  speed <deg/s> | speed img   how fast the viewer's eye or head crosses the strip;\n"
      "                       slower than the image's design speed stretches pass and gap\n"
      "  dir ltr|rtl|alt|pal|img\n"
      "  bri <0-255>   gamma <x>   mode full|safe|current\n"
      "  img <n> | next       select image (1-based)\n"
      "  auto on|off          playlist advance\n"
      "  mhz <f>              SPI clock (clocked chips)\n"
      "  test | test off      static wiring / color-order frame\n"
      "  save | load | reset  NVS settings\n"
      "  ls | rm <n> | wipe   images stored on the ESP32 (uploaded by the povstrip tool)");
}

static void handleCommand(char *line) {
  char *cmd = strtok(line, " \t");
  char *arg = strtok(nullptr, " \t");
  if (!cmd) return;
  for (char *p = cmd; *p; ++p) *p = (char)tolower(*p);

  if (!strcmp(cmd, "help") || !strcmp(cmd, "?")) { printHelp(); Serial.println("ok"); return; }
  if (!strcmp(cmd, "info")) { printInfo(); Serial.println("ok"); return; }
  if (!strcmp(cmd, "ping")) { Serial.println("ok"); return; }
  if (!strcmp(cmd, "ver")) { Serial.println("pov " POV_VERSION " " __DATE__); Serial.println("ok"); return; }
  if (!strcmp(cmd, "save")) { saveSettings(); Serial.println("saved"); Serial.println("ok"); return; }
  if (!strcmp(cmd, "load") || !strcmp(cmd, "reset")) {
    if (!strcmp(cmd, "reset")) { clearSettings(); defaults(); }
    else loadSettings();
#if LED_IS_CLOCKED(POV_CHIP)
    ledout::setClock((uint32_t)(S.spiMHz * 1e6f));
#endif
    applyRender();
    g_img = S.img;
    applyStrip();
    selectImage(g_img);
    printInfo();
    Serial.println("ok");
    return;
  }
  if (!strcmp(cmd, "test")) {
    g_test = !(arg && !strcmp(arg, "off"));
    if (g_test) showTest();
    Serial.println(g_test ? "test frame on ('test off' to resume)" : "POV resumed");
    Serial.println("ok");
    return;
  }
  if (!strcmp(cmd, "next")) { selectImage(g_img + 1); printInfo(); Serial.println("ok"); return; }
  if (!strcmp(cmd, "ls")) { povstore::list(Serial); Serial.println("ok"); return; }
  if (!strcmp(cmd, "wipe")) {
    povstore::wipe();
    selectImage(0);
    Serial.println("store emptied; playing the compiled-in image");
    Serial.println("ok");
    return;
  }
  if (!strcmp(cmd, "put")) {
    char *a2 = strtok(nullptr, " \t");
    char *a3 = strtok(nullptr, " \t");
    if (!arg || !a2 || !a3) { Serial.println("err usage: put <slot> <bytes> <crc32hex>"); return; }
    const uint8_t slot = (uint8_t)atoi(arg);
    const uint32_t bytes = (uint32_t)strtoul(a2, nullptr, 10);
    const uint32_t crc = (uint32_t)strtoul(a3, nullptr, 16);
    ledout::waitDone();
    ledout::build(0, nullptr, g_rowMap);   // dark while the transfer runs
    ledout::send(0);
    ledout::waitDone();
    g_buf = 1;
    const bool ok = povstore::receive(Serial, slot, bytes, crc);
    if (ok) {
      // play the new slot right away
      uint16_t idx = 0;
      for (uint8_t k = 0; k < povstore::count(); ++k) if (povstore::slotAt(k) == slot) idx = k;
      selectImage(idx);
    } else {
      selectImage(g_img);
    }
    return;   // receive() already printed ok/err
  }
  if (!strcmp(cmd, "rm")) {
    if (!arg) { Serial.println("err usage: rm <slot>"); return; }
    if (!povstore::remove((uint8_t)atoi(arg))) { Serial.println("err no such slot"); return; }
    selectImage(g_img);
    Serial.println("ok");
    return;
  }
  if (!arg) { Serial.println("err missing value; 'help' lists commands"); return; }

  if (!strcmp(cmd, "len")) {
    const float v = atof(arg);
    if (v < 0.05f) { Serial.println("err len: meters, e.g. len 2.5"); return; }
    S.lengthM = v;
    if (ledsFor(v, S.density) == POV_NUM_LEDS_MAX && lroundf(v * S.density) > POV_NUM_LEDS_MAX)
      Serial.printf("capped at POV_NUM_LEDS_MAX = %u\n", (unsigned)POV_NUM_LEDS_MAX);
    applyStrip();
  } else if (!strcmp(cmd, "dens")) {
    const int v = atoi(arg);
    if (v < 1) { Serial.println("err dens: LEDs per meter"); return; }
    S.density = (uint16_t)v;
    applyStrip();
  } else if (!strcmp(cmd, "col")) {
    S.colUs = (uint32_t)strtoul(arg, nullptr, 10);
    applyImage();
  } else if (!strcmp(cmd, "gap")) {
    S.gapUs = !strcmp(arg, "img") ? -1 : (int32_t)strtol(arg, nullptr, 10);
    applyImage();
  } else if (!strcmp(cmd, "speed")) {
    if (!strcmp(arg, "img")) S.sweepDps = 0;
    else {
      const long v = atol(arg);
      if (v < 10 || v > 5000) { Serial.println("err speed: 10 to 5000 deg/s, or img"); return; }
      S.sweepDps = (uint16_t)v;
    }
    applyImage();
  } else if (!strcmp(cmd, "dir")) {
    if (!strcmp(arg, "ltr")) S.dir = POV_DIR_LTR;
    else if (!strcmp(arg, "rtl")) S.dir = POV_DIR_RTL;
    else if (!strcmp(arg, "alt")) S.dir = POV_DIR_ALT;
    else if (!strcmp(arg, "pal")) S.dir = POV_DIR_PAL;
    else S.dir = -1;
    applyImage();
  } else if (!strcmp(cmd, "bri")) {
    const int v = atoi(arg);
    S.bri = (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v);
    applyRender();
  } else if (!strcmp(cmd, "gamma")) {
    S.gamma = atof(arg);
    if (S.gamma < 0.5f) S.gamma = 0.5f;
    applyRender();
  } else if (!strcmp(cmd, "mode")) {
    S.mode = !strcmp(arg, "safe") ? RENDER_PWM_SAFE : !strcmp(arg, "current") ? RENDER_CURRENT : RENDER_FULL;
    applyRender();
    if (g_mode != S.mode) Serial.println("current mode needs SK9822 or HD107S; using safe");
  } else if (!strcmp(cmd, "img")) {
    const int v = atoi(arg);
    if (v < 1 || v > imageCount()) { Serial.printf("err img: 1..%u\n", imageCount()); return; }
    selectImage((uint16_t)(v - 1));
  } else if (!strcmp(cmd, "auto")) {
    S.autoAdvance = !strcmp(arg, "on");
  } else if (!strcmp(cmd, "mhz")) {
#if LED_IS_CLOCKED(POV_CHIP)
    const float v = atof(arg);
    if (v < 0.1f || v > 40.0f) { Serial.println("err mhz: 0.1 to 40"); return; }
    S.spiMHz = v;
    if (!ledout::setClock((uint32_t)(v * 1e6f))) Serial.println("SPI clock change failed");
    applyImage();
#else
    Serial.println("err mhz only applies to clocked chips");
    return;
#endif
  } else {
    Serial.println("err unknown command; 'help' lists them");
    return;
  }
  if (g_test) showTest();
  printInfo();
  Serial.println("ok");
}

static void serviceSerial() {
  static char line[96];
  static uint8_t len = 0;
  static uint32_t lastRx = 0;
  static bool tipShown = false;
  while (Serial.available()) {
    const int c = Serial.read();
    lastRx = millis();
    if (c == '\r' || c == '\n') {
      if (len) {
        line[len] = 0;
        handleCommand(line);
        len = 0;
      }
    } else if (len < sizeof(line) - 1) {
      line[len++] = (char)c;
    }
  }
  // Serial Monitor on "No Line Ending": run the text after 1 s of quiet.
  if (len && millis() - lastRx > 1000) {
    line[len] = 0;
    handleCommand(line);
    len = 0;
    if (!tipShown) {
      Serial.println("(set the Serial Monitor line ending to New Line for instant commands)");
      tipShown = true;
    }
  }
}

// ---- Arduino entry points ------------------------------------------------------------
void setup() {
  Serial.setRxBufferSize(8192);   // room for one upload chunk and then some
  Serial.begin(POV_SERIAL_BAUD);
  Serial.setTimeout(3000);        // per chunk, during "put"
  for (uint16_t i = 0; i < POV_NUM_LEDS_MAX; ++i) g_identity[i] = i;
  loadSettings();
  g_n = ledsFor(S.lengthM, S.density);
  if (!ledout::begin(POV_NUM_LEDS_MAX, g_n, (uint32_t)(S.spiMHz * 1e6f))) {
    Serial.println("LED output init failed: check pins / POV_NUM_LEDS_MAX");
  }
  ledout::blankAll();
  applyRender();
  const bool store = povstore::begin();
  selectImage(S.img);
  delay(300);
  if (!store) Serial.println("\nno LittleFS partition: images cannot be uploaded, playing the compiled-in set");
  else if (povstore::freshlyFormatted()) Serial.println("\nimage store formatted (first boot on this partition layout)");
  if (g_savedStale) Serial.println("\nsaved settings skipped: config.h changed since 'save'");
  printInfo();
  printHelp();
  Serial.println("ready");
  g_lastYield = esp_timer_get_time();
  g_deadline = g_lastYield + 1000;
}

void loop() {
  serviceSerial();
  if (g_test) {
    delay(20);
    return;
  }
  advancePlaylist();
  runCycle();
  // Let the idle task run now and then (single-core chips watchdog it).
  if (esp_timer_get_time() - g_lastYield > 200000) {
    vTaskDelay(1);
    g_lastYield = esp_timer_get_time();
  }
}

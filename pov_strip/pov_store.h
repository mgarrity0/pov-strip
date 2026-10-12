// pov_store.h - images kept on the ESP32 itself (LittleFS), uploaded over serial.
//
// Slots /img/01.pov .. /img/32.pov play in order. A .pov file is a 48-byte header
// followed by column-major RGB888 (rows x cols x 3), the same layout as pov_image.h:
//
//   offset  size  field
//   0       4     "POV1"
//   4       2     rows      (uint16, LEDs along the strip at export)
//   6       2     cols      (uint16, columns per pass)
//   8       4     col_us    (uint32, design column period)
//   12      4     gap_us    (uint32, dark time after each pass)
//   16      1     dir       (POV_DIR_*)
//   17      1     sweep     (design eye/head speed across the strip in units of 5 deg/s;
//                            0 = unknown. 60 = 300 deg/s)
//   18      2     hold_s    (uint16, seconds before the playlist advances; 0 = stay)
//   20      4     length_m  (float32, strip length the image was sized for)
//   24      24    name      (UTF-8, zero padded)
//   48      ...   pixels
//
// Serial upload ("put"):
//   host:   put <slot> <bytes> <crc32 hex>\n      (slot 1..32, bytes = whole file)
//   device: go\n
//   host:   bytes, in chunks of at most 1024
//   device: .\n   after every chunk it has written
//   device: ok <bytes>\n  or  err <reason>\n
// CRC32 is the usual reflected 0xEDB88320 one (zlib, Python binascii.crc32).
// When no slots exist the compiled-in POV_IMAGES from pov_image.h play instead.
#pragma once
#include <Arduino.h>
#include <LittleFS.h>
#include <Preferences.h>
#include "esp_heap_caps.h"
#include "esp_partition.h"
#include "pov_types.h"

namespace povstore {

static const uint8_t SLOTS = 32;
static const uint32_t HEADER_BYTES = 48;
static const uint32_t MAX_FILE_BYTES = 2u * 1024u * 1024u;
static const uint32_t CHUNK = 1024;

static bool s_ok = false;          // filesystem mounted
static bool s_have[SLOTS + 1];     // slot present (1-based)
static uint8_t s_order[SLOTS];     // playlist: slot numbers in order
static uint8_t s_count = 0;
static uint32_t s_crcTable[256];

static void crcInit() {
  for (uint32_t n = 0; n < 256; ++n) {
    uint32_t c = n;
    for (int k = 0; k < 8; ++k) c = (c & 1u) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
    s_crcTable[n] = c;
  }
}
static uint32_t crcUpdate(uint32_t crc, const uint8_t *p, size_t n) {
  while (n--) crc = s_crcTable[(crc ^ *p++) & 0xFFu] ^ (crc >> 8);
  return crc;
}

static const char *slotPath(uint8_t slot) {
  static char path[20];
  snprintf(path, sizeof path, "/img/%02u.pov", (unsigned)slot);
  return path;
}

static void scan() {
  s_count = 0;
  for (uint8_t i = 1; i <= SLOTS; ++i) {
    s_have[i] = s_ok && LittleFS.exists(slotPath(i));
    if (s_have[i]) s_order[s_count++] = i;
  }
}

// A filesystem left on the flash by another project, or one formatted for a
// different partition layout, makes LittleFS abort at mount (assert in
// lfs_fs_grow when the old filesystem is larger than the partition). So the
// first boot after a layout change wipes the superblocks and formats fresh.
// The partition size seen last is kept in its own NVS namespace, which the
// console's "reset" does not touch.
static bool s_fresh = false;

static void prepareFlash() {
  const esp_partition_t *part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_SPIFFS, "spiffs");
  if (!part) return;
  Preferences p;
  if (!p.begin("povfs", false)) return;
  const uint32_t seen = p.getUInt("size", 0);
  if (seen != part->size) {
    esp_partition_erase_range(part, 0, 2u * 4096u);  // both LittleFS superblocks
    p.putUInt("size", part->size);
    s_fresh = true;
  }
  p.end();
}

static bool begin() {
  crcInit();
  prepareFlash();
  s_ok = LittleFS.begin(true);  // formats when nothing valid is there
  if (s_ok) LittleFS.mkdir("/img");
  scan();
  return s_ok;
}

static bool freshlyFormatted() { return s_fresh; }

static uint8_t count() { return s_count; }

static uint8_t slotAt(uint8_t idx) { return idx < s_count ? s_order[idx] : 0; }

static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t rd32(const uint8_t *p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }

// Parses a header; false if it is not a POV1 file with sane dimensions.
static bool parseHeader(const uint8_t *h, PovImage &im, char *name, uint32_t fileBytes) {
  if (memcmp(h, "POV1", 4) != 0) return false;
  im.rows = rd16(h + 4);
  im.cols = rd16(h + 6);
  im.col_us = rd32(h + 8);
  im.gap_us = rd32(h + 12);
  im.dir = h[16];
  im.sweep_dps = (uint16_t)h[17] * 5u;
  im.hold_s = rd16(h + 18);
  uint32_t lm = rd32(h + 20);
  memcpy(&im.length_m, &lm, 4);
  if (name) { memcpy(name, h + 24, 24); name[23] = 0; }
  if (im.rows < 1 || im.cols < 1 || im.cols > 8192 || im.dir > POV_DIR_PAL) return false;
  if (!(im.length_m >= 0.0f && im.length_m < 100.0f)) im.length_m = 0.0f;
  const uint64_t need = (uint64_t)im.rows * im.cols * 3u + HEADER_BYTES;
  return need == fileBytes;
}

// Loads slot `slot` into a new buffer (PSRAM when present). *buf must be freed by
// the caller. Returns false and prints nothing on failure; `why` gets a reason.
static bool load(uint8_t slot, PovImage &im, char *name, uint8_t **buf, const char **why) {
  *buf = nullptr;
  File f = LittleFS.open(slotPath(slot), "r");
  if (!f) { *why = "open"; return false; }
  uint8_t h[HEADER_BYTES];
  if (f.read(h, HEADER_BYTES) != HEADER_BYTES || !parseHeader(h, im, name, (uint32_t)f.size())) {
    f.close(); *why = "header"; return false;
  }
  const size_t bytes = (size_t)im.rows * im.cols * 3u;
  uint8_t *b = (uint8_t *)heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM);
  if (!b && heap_caps_get_largest_free_block(MALLOC_CAP_8BIT) > bytes + 48u * 1024u) b = (uint8_t *)malloc(bytes);
  if (!b) { f.close(); *why = "ram"; return false; }
  size_t got = 0;
  while (got < bytes) {
    const size_t n = f.read(b + got, bytes - got);
    if (n == 0) break;
    got += n;
  }
  f.close();
  if (got != bytes) { free(b); *why = "short"; return false; }
  im.data = b;
  *buf = b;
  return true;
}

static bool remove(uint8_t slot) {
  if (!s_ok || slot < 1 || slot > SLOTS) return false;
  const bool r = LittleFS.remove(slotPath(slot));
  scan();
  return r;
}

static void wipe() {
  if (!s_ok) return;
  for (uint8_t i = 1; i <= SLOTS; ++i) if (LittleFS.exists(slotPath(i))) LittleFS.remove(slotPath(i));
  LittleFS.remove("/img/tmp.pov");
  scan();
}

// Receives one file over Serial (protocol above). Prints go/./ok/err itself.
static bool receive(Stream &io, uint8_t slot, uint32_t total, uint32_t crcWant) {
  if (!s_ok) { io.println("err no filesystem"); return false; }
  if (slot < 1 || slot > SLOTS) { io.println("err slot 1..32"); return false; }
  if (total < HEADER_BYTES + 3 || total > MAX_FILE_BYTES) { io.println("err size"); return false; }
  if (LittleFS.exists("/img/tmp.pov")) LittleFS.remove("/img/tmp.pov");
  File f = LittleFS.open("/img/tmp.pov", "w");
  if (!f) { io.println("err open"); return false; }
  io.println("go");
  static uint8_t chunk[CHUNK];
  uint8_t header[HEADER_BYTES];
  uint32_t got = 0, crc = 0xFFFFFFFFu;
  const char *fail = nullptr;
  while (got < total) {
    const uint32_t want = (total - got) < CHUNK ? (total - got) : CHUNK;
    const size_t n = io.readBytes(chunk, want);
    if (n != want) { fail = "timeout"; break; }
    if (got < HEADER_BYTES) memcpy(header + got, chunk, (HEADER_BYTES - got) < n ? (HEADER_BYTES - got) : n);
    crc = crcUpdate(crc, chunk, n);
    if (f.write(chunk, n) != n) { fail = "write (filesystem full?)"; break; }
    got += n;
    io.print(".\n");
  }
  f.close();
  PovImage im;
  if (!fail && (crc ^ 0xFFFFFFFFu) != crcWant) fail = "crc";
  if (!fail && !parseHeader(header, im, nullptr, total)) fail = "header";
  if (fail) {
    LittleFS.remove("/img/tmp.pov");
    io.print("err "); io.println(fail);
    return false;
  }
  if (LittleFS.exists(slotPath(slot))) LittleFS.remove(slotPath(slot));
  if (!LittleFS.rename("/img/tmp.pov", slotPath(slot))) { io.println("err rename"); return false; }
  scan();
  io.print("ok "); io.println(total);
  return true;
}

// One line per slot: "<slot> <rows>x<cols> <bytes> <name>"
static void list(Stream &io) {
  if (!s_ok) { io.println("store: no filesystem (check the partition scheme)"); return; }
  io.printf("store: %u image%s, %lu of %lu kB used\n", (unsigned)s_count, s_count == 1 ? "" : "s",
            (unsigned long)(LittleFS.usedBytes() / 1024), (unsigned long)(LittleFS.totalBytes() / 1024));
  for (uint8_t k = 0; k < s_count; ++k) {
    const uint8_t slot = s_order[k];
    File f = LittleFS.open(slotPath(slot), "r");
    if (!f) continue;
    uint8_t h[HEADER_BYTES];
    PovImage im;
    char name[24];
    const bool okh = f.read(h, HEADER_BYTES) == HEADER_BYTES && parseHeader(h, im, name, (uint32_t)f.size());
    if (okh) io.printf("  %2u  %ux%u  %lu bytes  %s\n", (unsigned)slot, im.rows, im.cols, (unsigned long)f.size(), name);
    else io.printf("  %2u  (unreadable)\n", (unsigned)slot);
    f.close();
  }
}

}  // namespace povstore

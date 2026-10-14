// pov_resample.h - area-average resampling of a column-major RGB888 image.
//
// When the displayed grid (LEDs x columns) is smaller than the stored image in either
// direction, picking the nearest stored pixel drops whole rows or columns: a thin line
// can vanish or show at full strength depending on where it falls. Instead the image
// is resampled once into RAM: each displayed pixel is the area average of the stored
// pixels it covers, taken in linear light (through the output gamma) so the emitted
// energy is kept. A thin line then shows at reduced strength instead of disappearing.
// Pure upsampling (a longer strip) just repeats pixels and needs no RAM.
// (A header rather than the .ino because the Arduino preprocessor cannot prototype templates.)
#pragma once
#include <Arduino.h>
#include <math.h>

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

// fetch(i) returns stored column i (SR x RGB): a pointer into RAM, or a column read
// from flash. Columns are asked for in order within each displayed column, so a
// one-column reader re-reads only the column shared with the previous one.
// Accumulators: rows * 24 bytes from the heap (720 LEDs: 17 kB).
template <class Fetch>
static void resampleImage(Fetch fetch, uint32_t SR, uint32_t SC, uint8_t *dst, uint16_t rows, uint32_t cols,
                          float gamma) {
  if (gamma < 0.5f) gamma = 0.5f;
  for (int v = 0; v < 256; ++v) s_lin16[v] = (uint16_t)lroundf(powf(v / 255.0f, gamma) * 65535.0f);
  const uint64_t norm = (uint64_t)SC * SR;
  uint64_t *acc = (uint64_t *)malloc((size_t)rows * 3u * sizeof(uint64_t));
  if (!acc) return;
  // Displayed column kc covers stored columns [kc*SC/cols, (kc+1)*SC/cols); the same
  // for rows. In units of 1/cols (1/rows) every overlap is an integer.
  for (uint32_t kc = 0; kc < cols; ++kc) {
    const uint64_t a = (uint64_t)kc * SC, b = (uint64_t)(kc + 1) * SC;
    const uint32_t i0 = (uint32_t)(a / cols), i1 = (uint32_t)((b - 1) / cols);
    memset(acc, 0, (size_t)rows * 3u * sizeof(uint64_t));
    for (uint32_t i = i0; i <= i1; ++i) {
      const uint64_t clo = (uint64_t)i * cols, chi = clo + cols;
      const uint64_t wx = (b < chi ? b : chi) - (a > clo ? a : clo);
      const uint8_t *col = fetch(i);
      for (uint32_t kr = 0; kr < rows; ++kr) {
        const uint64_t ya = (uint64_t)kr * SR, yb = (uint64_t)(kr + 1) * SR;
        const uint32_t j0 = (uint32_t)(ya / rows), j1 = (uint32_t)((yb - 1) / rows);
        uint64_t *o = acc + (size_t)kr * 3u;
        for (uint32_t j = j0; j <= j1; ++j) {
          const uint64_t lo = (uint64_t)j * rows, hi = lo + rows;
          const uint64_t w = wx * ((yb < hi ? yb : hi) - (ya > lo ? ya : lo));
          const uint8_t *p = col + j * 3u;
          o[0] += w * s_lin16[p[0]];
          o[1] += w * s_lin16[p[1]];
          o[2] += w * s_lin16[p[2]];
        }
      }
    }
    for (uint32_t kr = 0; kr < rows; ++kr) {
      uint8_t *o = dst + (kc * rows + kr) * 3u;
      const uint64_t *s = acc + (size_t)kr * 3u;
      o[0] = encode16((uint32_t)(s[0] / norm));
      o[1] = encode16((uint32_t)(s[1] / norm));
      o[2] = encode16((uint32_t)(s[2] / norm));
    }
  }
  free(acc);
}


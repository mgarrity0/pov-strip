#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <vector>
#include <cstring>
typedef struct { const uint8_t *data; uint16_t rows; uint16_t cols; uint32_t col_us; uint32_t gap_us; uint8_t dir; float length_m; uint16_t hold_s; } PovImage;
#include "resample_extract.inc"
int main(int argc, char **argv) {
  // args: src.bin SR SC rows cols gamma out.bin
  FILE *f = fopen(argv[1], "rb"); uint32_t SR = atoi(argv[2]), SC = atoi(argv[3]), rows = atoi(argv[4]), cols = atoi(argv[5]); float gamma = atof(argv[6]);
  std::vector<uint8_t> src(SR * SC * 3); if (fread(src.data(), 1, src.size(), f) != src.size()) return 3; fclose(f);
  std::vector<uint8_t> dst(rows * cols * 3);
  const uint8_t *base = src.data();
  resampleImage([&](uint32_t i) { return base + (size_t)i * SR * 3u; }, SR, SC, dst.data(), rows, cols, gamma);
  // the same through a one-column "reader", as the firmware does from flash
  std::vector<uint8_t> dst2(rows * cols * 3), colbuf(SR * 3); int32_t cur = -1;
  resampleImage([&](uint32_t i) { if ((int32_t)i != cur) { memcpy(colbuf.data(), base + (size_t)i * SR * 3u, SR * 3u); cur = i; } return (const uint8_t *)colbuf.data(); }, SR, SC, dst2.data(), rows, cols, gamma);
  if (dst != dst2) { fprintf(stderr, "reader path differs\n"); return 2; }
  f = fopen(argv[7], "wb"); fwrite(dst.data(), 1, dst.size(), f); fclose(f);
  return 0;
}

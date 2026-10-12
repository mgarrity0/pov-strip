#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <vector>
typedef struct { const uint8_t *data; uint16_t rows; uint16_t cols; uint32_t col_us; uint32_t gap_us; uint8_t dir; float length_m; uint16_t hold_s; } PovImage;
#include "resample_extract.inc"
int main(int argc, char **argv) {
  // args: src.bin SR SC rows cols gamma out.bin
  FILE *f = fopen(argv[1], "rb"); uint32_t SR = atoi(argv[2]), SC = atoi(argv[3]), rows = atoi(argv[4]), cols = atoi(argv[5]); float gamma = atof(argv[6]);
  std::vector<uint8_t> src(SR * SC * 3); fread(src.data(), 1, src.size(), f); fclose(f);
  PovImage im = { src.data(), (uint16_t)SR, (uint16_t)SC, 0, 0, 0, 0, 0 };
  std::vector<uint8_t> dst(rows * cols * 3);
  resampleImage(im, dst.data(), rows, cols, gamma);
  f = fopen(argv[7], "wb"); fwrite(dst.data(), 1, dst.size(), f); fclose(f);
  return 0;
}

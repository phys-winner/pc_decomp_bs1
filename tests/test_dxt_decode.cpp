#include "../android/dxvk/dxt_decode.h"
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>

static unsigned checks;
#define CHECK(value) do { if (!(value)) { std::fprintf(stderr, "check failed at line %d\n", __LINE__); std::exit(3); } ++checks; } while (0)

static void put16(uint8_t* out, uint16_t value) { out[0] = uint8_t(value); out[1] = uint8_t(value >> 8); }
static void put32(uint8_t* out, uint32_t value) {
  out[0] = uint8_t(value); out[1] = uint8_t(value >> 8);
  out[2] = uint8_t(value >> 16); out[3] = uint8_t(value >> 24);
}

static void pixel(const uint8_t* actual, std::array<uint8_t, 4> expected) {
  static unsigned index;
  std::fprintf(stderr, "pixel %u: got %u %u %u %u; expected %u %u %u %u\n", index++,
    actual[0], actual[1], actual[2], actual[3], expected[0], expected[1], expected[2], expected[3]);
  for (unsigned i = 0; i < 4; ++i) {
    if (actual[i] != expected[i]) {
      std::fprintf(stderr, "channel %u: got %u expected %u\n", i, actual[i], expected[i]);
      std::exit(3);
    }
    ++checks;
  }
}

static void dxt1_color_and_alpha() {
  uint8_t block[8]{};
  put16(block, 0xf800); put16(block + 2, 0x07e0);
  put32(block + 4, 0xe4);
  std::array<uint8_t, 64> out{};
  dxt::decode(block, out.data(), 16, 4, 4, 0x31545844u);
  pixel(out.data(), {0, 0, 255, 255});
  pixel(out.data() + 4, {0, 255, 0, 255});
  pixel(out.data() + 8, {0, 85, 170, 255});
  pixel(out.data() + 12, {0, 170, 85, 255});

  put16(block, 0); put16(block + 2, 0xffff); put32(block + 4, 0xe4);
  dxt::decode(block, out.data(), 16, 4, 4, 0x31545844u);
  pixel(out.data(), {0, 0, 0, 255});
  pixel(out.data() + 4, {255, 255, 255, 255});
  pixel(out.data() + 8, {127, 127, 127, 255});
  pixel(out.data() + 12, {0, 0, 0, 0});
}

static void dxt3_explicit_alpha() {
  uint8_t block[16]{};
  block[0] = 0xf0; block[1] = 0x5a;
  put16(block + 8, 0xffff); put16(block + 10, 0);
  put32(block + 12, 0xe4);
  std::array<uint8_t, 64> out{};
  dxt::decode(block, out.data(), 16, 4, 4, 0x33545844u);
  pixel(out.data(), {255, 255, 255, 0});
  pixel(out.data() + 4, {0, 0, 0, 255});
  pixel(out.data() + 8, {170, 170, 170, 170});
  pixel(out.data() + 12, {85, 85, 85, 85});
  dxt::decode(block, out.data(), 16, 4, 4, 0x32545844u);
  pixel(out.data() + 8, {170, 170, 170, 170});
}

static void dxt5_interpolated_alpha() {
  uint8_t block[16]{};
  block[0] = 0; block[1] = 255;
  uint64_t indices = 0;
  for (unsigned i = 0; i < 16; ++i) indices |= uint64_t(i % 8) << (i * 3);
  for (unsigned i = 0; i < 6; ++i) block[2 + i] = uint8_t(indices >> (8 * i));
  put16(block + 8, 0xffff); put16(block + 10, 0);
  std::array<uint8_t, 64> out{};
  dxt::decode(block, out.data(), 16, 4, 4, 0x35545844u);
  const uint8_t expected[8]{0, 255, 51, 102, 153, 204, 0, 255};
  for (unsigned i = 0; i < 16; ++i) CHECK(out[i * 4 + 3] == expected[i % 8]);
  for (unsigned i = 0; i < 4; ++i) pixel(out.data() + i * 4, {255, 255, 255, expected[i]});

  block[0] = 255; block[1] = 0;
  dxt::decode(block, out.data(), 16, 4, 4, 0x34545844u);
  const uint8_t reverse[8]{255, 0, 182, 146, 109, 73, 36, 0};
  for (unsigned i = 0; i < 16; ++i) CHECK(out[i * 4 + 3] == reverse[i % 8]);
}

static void partial_blocks_preserve_neighbors() {
  uint8_t blocks[16]{};
  put16(blocks, 0xf800); put16(blocks + 2, 0x07e0);
  put16(blocks + 8, 0x07e0); put16(blocks + 10, 0xf800);
  std::array<uint8_t, 5 * 4 * 4> out;
  out.fill(0xcd);
  dxt::decode(blocks, out.data(), 20, 5, 3, 0x31545844u);
  for (unsigned y = 0; y < 3; ++y) {
    for (unsigned x = 0; x < 5; ++x) {
      pixel(out.data() + y * 20 + x * 4, x < 4 ? std::array<uint8_t, 4>{0, 0, 255, 255}
                                               : std::array<uint8_t, 4>{0, 255, 0, 255});
    }
  }
  for (unsigned i = 60; i < out.size(); ++i) CHECK(out[i] == 0xcd);
}

int main() {
  std::puts("dxt1_color_and_alpha");
  dxt1_color_and_alpha();
  std::puts("dxt3_explicit_alpha");
  dxt3_explicit_alpha();
  std::puts("dxt5_interpolated_alpha");
  dxt5_interpolated_alpha();
  std::puts("partial_blocks_preserve_neighbors");
  partial_blocks_preserve_neighbors();
  std::printf("DXT host checks passed: %u\n", checks);
  return 0;
}

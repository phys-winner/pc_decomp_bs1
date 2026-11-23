#pragma once
#include <cstddef>
#include <cstdint>

namespace dxt {

inline uint16_t u16(const uint8_t* p) { return uint16_t(p[0]) | (uint16_t(p[1]) << 8); }
inline uint32_t u32(const uint8_t* p) { return u16(p) | (uint32_t(u16(p + 2)) << 16); }

struct Pixel { uint8_t b, g, r, a; };

inline Pixel color(uint16_t value, uint8_t alpha = 255) {
  uint8_t r = (value >> 11) & 31, g = (value >> 5) & 63, b = value & 31;
  return Pixel{uint8_t((b << 3) | (b >> 2)), uint8_t((g << 2) | (g >> 4)),
               uint8_t((r << 3) | (r >> 2)), alpha};
}

inline Pixel mix(Pixel a, Pixel b, unsigned aw, unsigned bw, unsigned divisor) {
  const unsigned rounding = divisor == 3 ? 1 : 0;
  return Pixel{uint8_t((a.b * aw + b.b * bw + rounding) / divisor),
               uint8_t((a.g * aw + b.g * bw + rounding) / divisor),
               uint8_t((a.r * aw + b.r * bw + rounding) / divisor), 255};
}

inline void decode(const uint8_t* source, uint8_t* destination, uint32_t pitch,
                   uint32_t width, uint32_t height, uint32_t format) {
  const uint32_t blockBytes = format == 0x31545844u ? 8 : 16;
  for (uint32_t by = 0; by < (height + 3) / 4; ++by) {
    for (uint32_t bx = 0; bx < (width + 3) / 4; ++bx) {
      const uint8_t* block = source + (by * ((width + 3) / 4) + bx) * blockBytes;
      Pixel colors[4];
      uint8_t alphas[8];
      if (format == 0x31545844u) {
        colors[0] = color(u16(block));
        colors[1] = color(u16(block + 2));
        colors[2] = u16(block) > u16(block + 2) ? mix(colors[0], colors[1], 2, 1, 3)
                                               : mix(colors[0], colors[1], 1, 1, 2);
        colors[3] = u16(block) > u16(block + 2) ? mix(colors[0], colors[1], 1, 2, 3) : Pixel{0, 0, 0, 0};
        for (unsigned i = 0; i < 8; ++i) alphas[i] = 255;
      } else {
        colors[0] = color(u16(block + 8));
        colors[1] = color(u16(block + 10));
        colors[2] = mix(colors[0], colors[1], 2, 1, 3);
        colors[3] = mix(colors[0], colors[1], 1, 2, 3);
      }
      if (format == 0x32545844u || format == 0x33545844u) {
        for (unsigned i = 0; i < 8; ++i) {
          uint8_t a = (block[i / 2] >> ((i % 2) * 4)) & 15;
          alphas[i] = uint8_t(a | (a << 4));
        }
      } else if (format == 0x34545844u || format == 0x35545844u) {
        alphas[0] = block[0]; alphas[1] = block[1];
        if (alphas[0] > alphas[1]) {
          for (unsigned i = 2; i < 8; ++i)
            alphas[i] = uint8_t(((7 - i) * alphas[0] + (i - 1) * alphas[1] + 3) / 7);
        } else {
          for (unsigned i = 2; i < 6; ++i)
            alphas[i] = uint8_t(((6 - i) * alphas[0] + (i - 1) * alphas[1] + 2) / 5);
          alphas[6] = 0; alphas[7] = 255;
        }
      }
      for (uint32_t y = 0; y < 4 && by * 4 + y < height; ++y) {
        for (uint32_t x = 0; x < 4 && bx * 4 + x < width; ++x) {
          unsigned index = y * 4 + x;
          unsigned colorIndex = (u32(block + (format == 0x31545844u ? 4 : 12)) >> (index * 2)) & 3;
          uint8_t alpha = format == 0x31545844u ? colors[colorIndex].a : alphas[colorIndex];
          if (format == 0x34545844u || format == 0x35545844u) {
            uint64_t bits = 0;
            for (unsigned i = 0; i < 6; ++i) bits |= uint64_t(block[2 + i]) << (8 * i);
            alpha = alphas[(bits >> (index * 3)) & 7];
          }
          Pixel pixel = colors[colorIndex]; pixel.a = alpha;
          uint8_t* out = destination + (by * 4 + y) * pitch + (bx * 4 + x) * 4;
          out[0] = pixel.b; out[1] = pixel.g; out[2] = pixel.r; out[3] = pixel.a;
        }
      }
    }
  }
}

}

"""Research camera photo: D3DX10LoadTextureFromTexture scales the subject's box of the back buffer into the
256x256 staging texture (android/render10/r10_resample.h, r10_reflect.c) instead of copying 2944x1840 texels into it."""

from pathlib import Path
import re
import subprocess
import unittest

from tests.host_environment import CLANG, environment

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / "build/tests/r10-resample"

HARNESS = r'''
#include <assert.h>
#include <stdio.h>
#include "r10_resample.h"

#define GUARD 0xA5u

static uint8_t *image(uint32_t w, uint32_t h, size_t pitch)
{
    uint8_t *p = (uint8_t *)malloc(pitch * h + 64);
    memset(p, GUARD, pitch * h + 64);
    return p;
}

static void guards_intact(const uint8_t *p, uint32_t w, uint32_t h, size_t pitch)
{
    for (uint32_t y = 0; y < h; ++y)
        for (size_t x = (size_t)w * 4; x < pitch; ++x) assert(p[y * pitch + x] == GUARD);
    for (size_t i = 0; i < 64; ++i) assert(p[pitch * h + i] == GUARD);
}

int main(void)
{
    /* The photo of the Lenovo TB373FU: a subject box of the 2944x1840 back buffer into 256x256 (filter LINEAR 3). */
    const uint32_t bw = 2944, bh = 1840;
    const size_t bp = (size_t)bw * 4;
    uint8_t *back = (uint8_t *)malloc(bp * bh);
    for (uint32_t y = 0; y < bh; ++y)
        for (uint32_t x = 0; x < bw; ++x) {
            uint8_t *t = back + y * bp + (size_t)x * 4;
            t[0] = (uint8_t)(x * 255 / (bw - 1)); t[1] = (uint8_t)(y * 255 / (bh - 1)); t[2] = 40; t[3] = 200;
        }
    uint32_t box[6] = { 700, 300, 0, 2600, 1700, 1 }, rect[4];
    assert(r10_resample_rect(box, bw, bh, rect));
    assert(rect[0] == 700 && rect[1] == 300 && rect[2] == 1900 && rect[3] == 1400);
    const size_t dp = 256 * 4 + 16;
    uint8_t *photo = image(256, 256, dp);
    assert(!r10_resample_point(3));
    assert(r10_resample_rgba8(back + rect[1] * bp + rect[0] * 4, rect[2], rect[3], bp, photo, 256, 256, dp, 0, 0, 0, 0));
    guards_intact(photo, 256, 256, dp);
    /* The gradient survives: left/top edges near the box's start, right/bottom near its end, constant channels exact. */
    const uint8_t *first = photo, *last = photo + 255 * dp + 255 * 4;
    assert(abs((int)first[0] - (int)(700 * 255 / (bw - 1))) <= 3);
    assert(abs((int)first[1] - (int)(300 * 255 / (bh - 1))) <= 3);
    assert(abs((int)last[0] - (int)(2599 * 255 / (bw - 1))) <= 3);
    assert(abs((int)last[1] - (int)(1699 * 255 / (bh - 1))) <= 3);
    for (uint32_t y = 0; y < 256; ++y)
        for (uint32_t x = 0; x < 256; ++x) {
            const uint8_t *t = photo + y * dp + x * 4;
            assert(t[2] == 40 && t[3] == 200);
            if (x) assert(t[0] >= t[-4]);
            if (y) assert(t[1] >= photo[(y - 1) * dp + x * 4 + 1]);
        }

    /* Box beyond the screen is clipped; an empty one is refused. */
    uint32_t wide[6] = { 2800, 1700, 0, 3100, 1900, 1 }, empty[6] = { 10, 10, 0, 10, 20, 1 };
    assert(r10_resample_rect(wide, bw, bh, rect) && rect[2] == 144 && rect[3] == 140);
    assert(!r10_resample_rect(empty, bw, bh, rect));
    assert(r10_resample_rect(NULL, bw, bh, rect) && !rect[0] && !rect[1] && rect[2] == bw && rect[3] == bh);

    /* Minification averages what a texel covers: 4x1 -> 2x1. */
    uint8_t row[16] = { 0, 0, 0, 0, 100, 100, 100, 100, 200, 200, 200, 200, 255, 255, 255, 255 }, two[8];
    assert(r10_resample_rgba8(row, 4, 1, 16, two, 2, 1, 8, 0, 0, 0, 0));
    assert(two[0] == 50 && two[4] == 228);
    /* Point sampling takes the texel under the centre. */
    assert(r10_resample_point(2) && r10_resample_point(1) && !r10_resample_point(0xFFFFFFFFu));
    assert(r10_resample_rgba8(row, 4, 1, 16, two, 2, 1, 8, 1, 0, 0, 0));
    assert(two[0] == 100 && two[4] == 255);

    /* Magnification (a small subject on a small screen): corners keep the source's, the inside interpolates. */
    uint8_t quad[16] = { 0, 0, 0, 255, 255, 0, 0, 255, 0, 255, 0, 255, 255, 255, 0, 255 };
    const size_t up = 256 * 4;
    uint8_t *big = image(256, 256, up);
    assert(r10_resample_rgba8(quad, 2, 2, 8, big, 256, 256, up, 0, 0, 0, 0));
    guards_intact(big, 256, 256, up);
    assert(big[0] == 0 && big[1] == 0);
    assert(big[255 * 4] == 255 && big[255 * up + 1] == 255);
    assert(big[128 * up + 128 * 4] > 100 && big[128 * up + 128 * 4] < 155);

    /* B8G8R8X8 back buffer into an R8G8B8A8 texture: R and B exchanged, undefined X read as opaque. */
    uint8_t bgrx[4] = { 10, 20, 30, 0 }, rgba[4];
    assert(r10_resample_rgba8(bgrx, 1, 1, 4, rgba, 1, 1, 4, 0, 1, 1, 0));
    assert(rgba[0] == 30 && rgba[1] == 20 && rgba[2] == 10 && rgba[3] == 255);
    uint8_t rgba_in[4] = { 1, 2, 3, 4 }, x8[4];
    assert(r10_resample_rgba8(rgba_in, 1, 1, 4, x8, 1, 1, 4, 0, 0, 0, 1) && x8[3] == 255);

    /* Every axis' weights sum to 1 at odd ratios. */
    for (uint32_t ns = 1; ns < 40; ++ns)
        for (uint32_t nd = 1; nd < 40; ++nd)
            for (int point = 0; point < 2; ++point) {
                r10_resample_axis a;
                assert(r10_resample_axis_make(&a, ns, nd, point));
                for (uint32_t i = 0; i < nd; ++i) {
                    float sum = 0;
                    assert(a.first[i] < ns);
                    for (uint32_t k = 0; k < a.taps; ++k) {
                        if (a.weight[i * a.taps + k] != 0.0f) assert(a.first[i] + k < ns);
                        sum += a.weight[i * a.taps + k];
                    }
                    assert(sum > 0.999f && sum < 1.001f);
                }
                r10_resample_axis_free(&a);
            }
    assert(!r10_resample_rgba8(row, 0, 1, 16, two, 2, 1, 8, 0, 0, 0, 0));
    free(back); free(photo); free(big);
    puts("resample ok");
    return 0;
}
'''


class PhotoResampleTest(unittest.TestCase):
    def test_resampler(self):
        OUT.mkdir(parents=True, exist_ok=True)
        source, obj, exe = OUT / "resample.c", OUT / "resample.obj", OUT / "resample.exe"
        source.write_text(HARNESS, encoding="utf-8")
        env, linker = environment("x64")
        subprocess.run([str(CLANG), "/nologo", "/O1", "/std:c11", "/clang:--target=x86_64-pc-windows-msvc", "/c",
                        "/I" + str(ROOT / "android/render10"), str(source), "/Fo" + str(obj)], check=True, env=env, cwd=OUT)
        subprocess.run([str(linker), "/nologo", str(obj), "/OUT:" + str(exe)], check=True, env=env, cwd=OUT)
        result = subprocess.run([str(exe)], env=env, capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("resample ok", result.stdout)

    def test_wired_into_d3dx_and_backends(self):
        reflect = (ROOT / "android/render10/r10_reflect.c").read_text(encoding="utf-8")
        header = (ROOT / "android/render10/r10.h").read_text(encoding="utf-8")
        vulkan = (ROOT / "android/render10/r10_vulkan.c").read_text(encoding="utf-8")
        gles = (ROOT / "android/render10/r10_gles.c").read_text(encoding="utf-8")
        load = reflect[reflect.index('D3DX10LoadTextureFromTexture, 3)'):]
        self.assertIn("load_resampled(", load)
        self.assertIn("r10_resample_rect(", load)
        self.assertIn("(*read_region)(", header)
        self.assertNotIn("read_rgba", header + vulkan + gles)
        self.assertRegex(vulkan, r"vk_present, vk_read_region, vk_upscale")
        self.assertRegex(gles, r"gles_read_region, gles_upscale")
        # Copies into staging textures stay inside the destination level on both backends.
        staging = vulkan[vulkan.index("GPU -> CPU: into a host buffer"):]
        self.assertTrue(re.search(r"if \(w > lw - x\) w = lw - x;\s+if \(h > lh - y\) h = lh - y;", staging[:900]))
        staging = gles[gles.index("GPU -> CPU: the staging texture's shadow"):]
        self.assertTrue(re.search(r"if \(w > lw - x\) w = lw - x;\s+if \(h > lh - y\) h = lh - y;", staging[:1200]))
        self.assertNotIn("read_texture(src, src_sub, (uint8_t *)GPTR(dst->shadow", gles)


if __name__ == "__main__":
    unittest.main()

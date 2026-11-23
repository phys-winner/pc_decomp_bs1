"""The converted texture pack of the native Direct3D 10 renderer (android/render10/r10_pack.c).

The production r10_pack.c runs here against a recording backend and a small pack: a BC texture the device
cannot sample is created converted, whole levels the pack has upload its blocks (16 per 4x4, an opaque
ETC2 level gets RGBA8 EAC alpha blocks of 255), a level the pack lacks or a partial write moves the texture
to the decoded storage and rewrites the levels it held from the pack's copy of their original blocks,
converted copies keep the source's original, a copy between a converted and a decoded texture moves one level
(decoded from the original blocks, or the pack's converted copy of them) without restoring either side, a copy
the pack cannot follow restores first, and a level narrower than a block is looked up by its blocks whatever
row pitch the game passes (Bioshock.exe 0x109B3F40).
"""
from pathlib import Path
import subprocess
import unittest

from tests.host_environment import CLANG, environment

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / "build/tests/r10-pack"

HARNESS = r'''
#define _CRT_SECURE_NO_WARNINGS
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ut3tx.h"

#define R10_LOG(...) (printf("[r10] " __VA_ARGS__), printf("\n"))
enum { R10_FMT_COLOR = 0, R10_FMT_DEPTH, R10_FMT_BC, R10_FMT_TYPELESS };
enum { R10_DIM_BUFFER = 1, R10_DIM_TEXTURE2D = 3 };
enum { D3D10_USAGE_DEFAULT = 0, D3D10_USAGE_IMMUTABLE = 1, D3D10_USAGE_DYNAMIC = 2, D3D10_USAGE_STAGING = 3 };
enum { D3D10_BIND_SHADER_RESOURCE = 0x8, D3D10_BIND_RENDER_TARGET = 0x20 };
enum { D3D10_RESOURCE_MISC_GENERATE_MIPS = 0x1 };
enum { R10_PACK_ETC2 = 1, R10_PACK_ASTC = 2 };
#define R10_PACK_BLOCK_BYTES 16u
typedef struct r10_format { uint32_t dxgi; uint8_t kind, bytes, block, bc, srgb; const char *name; } r10_format;
typedef struct { uint32_t dim, width, height, depth, mips, layers, format, samples, quality, usage, bind, cpu, misc; } r10_resource_desc;
typedef struct r10_resource {
    r10_resource_desc desc; const r10_format *fmt; uint32_t subresources;
    uint8_t pack_eligible, converted; const uint8_t **pack_raw;
} r10_resource;
typedef struct r10_device r10_device;
typedef struct r10_backend {
    int (*samples)(const r10_format *f);
    int pack_formats;
    int (*resource_store)(r10_device *d, r10_resource *r);
    void (*resource_update)(r10_device *d, r10_resource *r, uint32_t sub, uint32_t x, uint32_t y, uint32_t z, uint32_t w,
                            uint32_t h, uint32_t depth, const void *data, uint32_t row_pitch, uint32_t slice_pitch);
    void (*copy)(r10_device *d, r10_resource *dst, uint32_t dst_sub, uint32_t x, uint32_t y, uint32_t z, r10_resource *src,
                 uint32_t src_sub, const uint32_t *box);
} r10_backend;
struct r10_device { const r10_backend *be; };
static r10_device *r10_dev;
static void r10_subresource_size(const r10_resource *r, uint32_t sub, uint32_t *w, uint32_t *h, uint32_t *d)
{
    uint32_t mip = sub % r->desc.mips;
    *w = r->desc.width >> mip ? r->desc.width >> mip : 1; *h = r->desc.height >> mip ? r->desc.height >> mip : 1; *d = 1;
}
static uint32_t r10_format_row_pitch(const r10_format *f, uint32_t w) { return ((w + 3) / 4) * f->bytes; }
static uint32_t r10_format_rows(const r10_format *f, uint32_t h) { (void)f; return (h + 3) / 4; }

/* ---------------------------------------------------------------- the pack (texture_cache.c's interface) */
static int family = R10_PACK_ASTC;
typedef struct { utx_entry e; const uint8_t *raw, *data; uint32_t w, h, fourcc; } fake_entry;
static fake_entry pack[8]; static unsigned pack_count;
static int void_texture_cache_family(void) { return family; }
static const utx_entry *void_texture_cache_find(uint32_t format, uint32_t w, uint32_t h, uint32_t levels, const uint8_t *data, uint32_t pitch)
{
    assert(levels == 1);
    uint32_t bytes = (uint32_t)utx_level_size(format, w, h);
    assert(pitch == ((w + 3) / 4) * (format == 0x31545844u ? 8u : 16u));   /* blocks back to back */
    for (unsigned i = 0; i < pack_count; ++i)
        if (pack[i].fourcc == format && pack[i].w == w && pack[i].h == h && !memcmp(pack[i].raw, data, bytes)) return &pack[i].e;
    return NULL;
}
static const uint8_t *void_texture_cache_raw(const utx_entry *e) { return ((const fake_entry *)e)->raw; }
static const uint8_t *void_texture_cache_data(const utx_entry *e) { return ((const fake_entry *)e)->data; }
/* The ranges the layer has read and copied away (their pages leave the process's mapping, texture_cache.c). */
typedef struct { const uint8_t *p; size_t bytes; } released;
static released dones[64]; static unsigned done_count;
static void void_texture_cache_done(const uint8_t *p, size_t bytes)
{
    if (done_count < 64) { dones[done_count].p = p; dones[done_count].bytes = bytes; }
    ++done_count;
}
static void add(uint32_t fourcc, uint32_t w, uint32_t h, const uint8_t *raw, const uint8_t *data, uint32_t gl_format)
{
    fake_entry *f = &pack[pack_count++];
    f->raw = raw; f->data = data; f->w = w; f->h = h; f->fourcc = fourcc;
    f->e.width = (uint16_t)w; f->e.height = (uint16_t)h; f->e.levels = 1; f->e.gl_format = gl_format;
}

/* ---------------------------------------------------------------- the recording backend */
typedef struct { r10_resource *r; uint32_t sub, w, h, pitch, converted; uint8_t first[32]; uint32_t bytes; } upload;
static upload uploads[64]; static unsigned upload_count, stores, copies;
static int samples_bc1, samples_all;
static int fake_samples(const r10_format *f) { return samples_all || (f->bc == 1 && samples_bc1); }
static int fake_store(r10_device *d, r10_resource *r) { (void)d; (void)r; ++stores; return 1; }
static void fake_update(r10_device *d, r10_resource *r, uint32_t sub, uint32_t x, uint32_t y, uint32_t z, uint32_t w, uint32_t h,
                        uint32_t depth, const void *data, uint32_t pitch, uint32_t slice)
{
    (void)d; (void)x; (void)y; (void)z; (void)depth;
    upload *u = &uploads[upload_count++];
    u->r = r; u->sub = sub; u->w = w; u->h = h; u->pitch = pitch; u->converted = r->converted; u->bytes = slice;
    memcpy(u->first, data, slice < sizeof u->first ? slice : sizeof u->first);
}
static void fake_copy(r10_device *d, r10_resource *dst, uint32_t ds, uint32_t x, uint32_t y, uint32_t z, r10_resource *src, uint32_t ss,
                      const uint32_t *box)
{ (void)d; (void)dst; (void)ds; (void)x; (void)y; (void)z; (void)src; (void)ss; (void)box; ++copies; }
static const r10_backend backend = { fake_samples, R10_PACK_ASTC | R10_PACK_ETC2, fake_store, fake_update, fake_copy };
static r10_device device = { &backend };

PACK_SOURCE

static const r10_format bc1 = { 72, R10_FMT_BC, 8, 4, 1, 1, "BC1_UNORM_SRGB" }, bc3 = { 77, R10_FMT_BC, 16, 4, 3, 0, "BC3_UNORM" };
static r10_resource texture(const r10_format *f, uint32_t w, uint32_t h, uint32_t mips)
{
    r10_resource r; memset(&r, 0, sizeof r);
    r.desc.dim = R10_DIM_TEXTURE2D; r.desc.width = w; r.desc.height = h; r.desc.depth = 1; r.desc.mips = mips; r.desc.layers = 1;
    r.desc.samples = 1; r.desc.usage = D3D10_USAGE_DEFAULT; r.desc.bind = D3D10_BIND_SHADER_RESOURCE; r.fmt = f; r.subresources = mips;
    r10_pack_prepare(&device, &r);
    return r;
}

int main(void)
{
    r10_dev = &device;
    static uint8_t raw8[8 * 8 / 16 * 8], raw4[8], raw2[8], astc8[4 * 16], astc4[16], astc2[16];
    for (unsigned i = 0; i < sizeof raw8; ++i) raw8[i] = (uint8_t)(i * 7 + 1);
    memset(raw4, 0x44, 8); memset(raw2, 0x22, 8); memset(astc8, 0xA8, sizeof astc8); memset(astc4, 0xA4, 16); memset(astc2, 0xA2, 16);
    add(0x31545844u, 8, 8, raw8, astc8, 0x93B0u); add(0x31545844u, 4, 4, raw4, astc4, 0x93B0u); add(0x31545844u, 2, 2, raw2, astc2, 0x93B0u);

    /* Eligible: converted from creation; a native BC format, a render target or a staging texture is not. */
    r10_resource t = texture(&bc1, 8, 8, 4);
    assert(t.pack_eligible && t.converted && t.pack_raw);
    samples_bc1 = 1; { r10_resource n = texture(&bc1, 8, 8, 4); assert(!n.converted); } samples_bc1 = 0;
    { r10_resource n = texture(&bc1, 8, 8, 4); r10_pack_release(&n); }

    /* Hits: the pack's blocks, 16 bytes a block; the 2x2 level arrives with the game's pitch of 4 (not 8). */
    r10_texture_write(&device, &t, 0, 0, 0, 0, 8, 8, 1, raw8, 16, 32);
    r10_texture_write(&device, &t, 1, 0, 0, 0, 4, 4, 1, raw4, 8, 8);
    r10_texture_write(&device, &t, 2, 0, 0, 0, 2, 2, 1, raw2, 4, 0);
    assert(upload_count == 3 && t.converted && !stores);
    assert(uploads[0].converted && uploads[0].pitch == 2 * 16 && uploads[0].bytes == 4 * 16 && !memcmp(uploads[0].first, astc8, 32));
    assert(uploads[2].pitch == 16 && !memcmp(uploads[2].first, astc2, 16));
    assert(t.pack_raw[0] == raw8 && t.pack_raw[1] == raw4 && t.pack_raw[2] == raw2 && !t.pack_raw[3]);
    /* Each hit lets the pages go once read: the lookup's raw level, then the uploaded converted level. */
    assert(done_count == 6 && dones[0].p == raw8 && dones[0].bytes == 32 && dones[1].p == astc8 && dones[1].bytes == 64);
    assert(dones[4].p == raw2 && dones[4].bytes == 8 && dones[5].p == astc2 && dones[5].bytes == 16);

    /* A converted copy keeps the blocks and the source's original. */
    r10_resource u = texture(&bc1, 8, 8, 4);
    r10_texture_copy(&device, &u, 1, 0, 0, 0, &t, 1, NULL);
    assert(copies == 1 && u.converted && u.pack_raw[1] == raw4 && !stores);

    /* A miss: decoded storage, the levels held rewritten from their original blocks, the written level skipped. */
    static uint8_t other[8];
    memset(other, 0x77, 8);
    upload_count = 0;
    r10_texture_write(&device, &t, 1, 0, 0, 0, 4, 4, 1, other, 8, 8);
    assert(stores == 1 && !t.converted && t.pack_eligible && t.pack_raw);
    assert(t.pack_raw[0] == raw8 && !t.pack_raw[1] && t.pack_raw[2] == raw2);   /* it keeps the originals it holds */
    assert(upload_count == 3);   /* levels 0 and 2 from the pack's raw copy, then level 1 as written */
    assert(!uploads[0].converted && uploads[0].sub == 0 && !memcmp(uploads[0].first, raw8, 32) && uploads[0].pitch == 16);
    assert(uploads[1].sub == 2 && !memcmp(uploads[1].first, raw2, 8));
    assert(uploads[2].sub == 1 && !memcmp(uploads[2].first, other, 8));
    /* Later writes of a texture that left the pack go to the decoded storage. */
    upload_count = 0;
    r10_texture_write(&device, &t, 0, 0, 0, 0, 8, 8, 1, raw8, 16, 32);
    assert(upload_count == 1 && !uploads[0].converted && stores == 1 && t.pack_raw[0] == raw8);
    r10_texture_write(&device, &t, 2, 0, 0, 0, 2, 2, 1, other, 4, 0);
    assert(!t.pack_raw[2]);                                     /* what the pack lacks is not tracked */

    /* From a decoded texture into a converted one, a level the pack has: its converted copy, nobody restored. */
    r10_resource v = texture(&bc1, 8, 8, 4);
    upload_count = 0;
    r10_texture_copy(&device, &v, 0, 0, 0, 0, &t, 0, NULL);
    assert(v.converted && stores == 1 && copies == 1 && upload_count == 1 && uploads[0].r == &v && uploads[0].converted);
    assert(!memcmp(uploads[0].first, astc8, 32) && v.pack_raw[0] == raw8);

    /* From a converted texture into a decoded one: the level's original blocks, decoded there; the source stays. */
    upload_count = 0;
    r10_texture_copy(&device, &t, 1, 0, 0, 0, &u, 1, NULL);
    assert(u.converted && stores == 1 && copies == 1 && upload_count == 1 && uploads[0].r == &t && !uploads[0].converted);
    assert(uploads[0].pitch == 8 && !memcmp(uploads[0].first, raw4, 8) && t.pack_raw[1] == raw4);

    /* A level the pack lacks, from a decoded texture into a converted one: the destination is restored (all but
     * the copied level) and the backend copies. */
    r10_texture_write(&device, &t, 1, 0, 0, 0, 4, 4, 1, other, 8, 8);
    assert(!t.pack_raw[1]);
    upload_count = 0;
    r10_texture_copy(&device, &u, 1, 0, 0, 0, &t, 1, NULL);
    assert(stores == 2 && !u.converted && copies == 2 && upload_count == 0 && !u.pack_raw[1]);   /* u held only level 1 */

    /* A partial write restores every level, the written one too. */
    r10_resource p = texture(&bc1, 8, 8, 4);
    r10_texture_write(&device, &p, 0, 0, 0, 0, 8, 8, 1, raw8, 16, 32);
    upload_count = 0;
    r10_texture_write(&device, &p, 0, 4, 4, 0, 4, 4, 1, raw4, 8, 8);
    assert(stores == 3 && !p.converted && upload_count == 2 && uploads[0].sub == 0 && !memcmp(uploads[0].first, raw8, 32));
    assert(!p.pack_raw[0]);

    /* ETC2: an opaque level's 8-byte blocks become RGBA8 EAC blocks with alpha 255 (base 255, multiplier 0). */
    family = R10_PACK_ETC2;
    static uint8_t raw16[2 * 16], etc[8];
    for (unsigned i = 0; i < sizeof raw16; ++i) raw16[i] = (uint8_t)(200 - i);
    for (unsigned i = 0; i < 8; ++i) etc[i] = (uint8_t)(0x10 + i);
    add(0x35545844u, 4, 8, raw16, etc, 0x9274u);
    static uint8_t etc2[16];
    memcpy(etc2, etc, 8); memcpy(etc2 + 8, etc, 8);
    pack[pack_count - 1].data = etc2;
    r10_resource e = texture(&bc3, 4, 8, 1);
    assert(e.converted);
    upload_count = 0;
    r10_texture_write(&device, &e, 0, 0, 0, 0, 4, 8, 1, raw16, 16, 32);
    assert(upload_count == 1 && uploads[0].pitch == 16 && uploads[0].bytes == 32);
    static const uint8_t opaque[8] = { 0xFF, 0, 0, 0, 0, 0, 0, 0 };
    assert(!memcmp(uploads[0].first, opaque, 8) && !memcmp(uploads[0].first + 8, etc, 8));
    assert(!memcmp(uploads[0].first + 16, opaque, 8) && !memcmp(uploads[0].first + 24, etc, 8));
    r10_pack_release(&e); r10_pack_release(&p); r10_pack_release(&v); r10_pack_release(&u); r10_pack_release(&t);

    /* Every color variant prefers original BC. The pack tracks both mips independently for every face. */
    static const uint32_t fourcc[] = { 0x31545844u, 0x33545844u, 0x35545844u };
    static const uint8_t raw_block[16] = { 3 }, gpu_block[16] = { 0x5a };
    for (family = R10_PACK_ETC2; family <= R10_PACK_ASTC; ++family) {
        uint32_t target = family == R10_PACK_ETC2 ? 0x9278u : 0x93B0u;
        for (unsigned bc = 1; bc <= 3; ++bc) {
            pack_count = 0;
            add(fourcc[bc-1], 4, 4, raw_block, gpu_block, target);
            add(fourcc[bc-1], 2, 2, raw_block, gpu_block, target);
            for (unsigned srgb = 0; srgb <= 1; ++srgb) {
                const r10_format format = { 70 + 3 * (bc - 1) + 1 + srgb, R10_FMT_BC, bc == 1 ? 8 : 16, 4, bc, srgb, "BC variant" };
                samples_all = 1;
                r10_resource cube = texture(&format, 4, 4, 2);
                cube.desc.layers = 6;
                cube.subresources = 12;
                assert(!cube.converted);
                samples_all = 0;
                r10_pack_prepare(&device, &cube);
                assert(cube.converted);
                upload_count = 0;
                for (uint32_t sub = 0; sub < cube.subresources; ++sub) {
                    uint32_t side = 4 >> (sub % 2);
                    r10_texture_write(&device, &cube, sub, 0, 0, 0, side, side, 1, raw_block, format.bytes, format.bytes);
                    assert(uploads[sub].converted && uploads[sub].sub == sub && uploads[sub].bytes == 16);
                    assert(!memcmp(uploads[sub].first, gpu_block, 16) && cube.pack_raw[sub] == raw_block);
                }
                assert(upload_count == 12);
                r10_pack_release(&cube);
            }
        }
    }
    printf("PASS r10_pack: direct BC, ETC2 and ASTC fallbacks for linear and sRGB cubemaps; level copies and restores\n");
    return 0;
}
'''


class R10PackTest(unittest.TestCase):
    def test_production_pack_layer(self):
        source = (ROOT / "android/render10/r10_pack.c").read_text(encoding="utf-8")
        body = "\n".join(line for line in source.splitlines() if not line.startswith("#include"))
        OUT.mkdir(parents=True, exist_ok=True)
        harness = OUT / "r10_pack_test.c"
        harness.write_text(HARNESS.replace("PACK_SOURCE", body), encoding="utf-8")
        env, linker = environment("x64")
        obj, exe = OUT / "r10_pack_test.obj", OUT / "r10_pack_test.exe"
        for command in ([str(CLANG), "/nologo", "/Od", "/W3", "/std:c11", "/clang:--target=x86_64-pc-windows-msvc",
                         "-Wno-unused-function", "/I" + str(ROOT / "android/texconv"), "/c", str(harness), "/Fo" + str(obj)],
                        [str(linker), "/nologo", str(obj), "/OUT:" + str(exe)],
                        [str(exe)]):
            result = subprocess.run(command, capture_output=True, text=True, env=env, timeout=120)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("PASS r10_pack", result.stdout)


if __name__ == "__main__":
    unittest.main()

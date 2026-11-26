"""Replay D3D10 Map/Unmap, the index bounds cache and both backends' stream uploads on host memory."""
from itertools import product
from pathlib import Path
import subprocess
import tempfile
import unittest

from tests.host_environment import CLANG, environment
from tests.test_mobile_shaders import function

RENDER = Path(__file__).resolve().parents[1] / "android/render10"

COMMON = r'''
#define _CRT_SECURE_NO_WARNINGS
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include "r10.h"

static r10_device device;
r10_device *r10_dev = &device;
r10_backend r10_gles_backend, r10_vulkan_backend;
static uint8_t guest[256], gpu[8192];
static uint32_t used;
#define GPTR(va) (guest + (va))
static int ensure_shadow(r10_device *d, r10_resource *r, uint32_t type)
{
    assert(r->shadow && r->shadow + r->desc.width <= sizeof guest);
    return 1;
}
void r10_subresource_size(const r10_resource *r, uint32_t sub, uint32_t *w, uint32_t *h, uint32_t *depth)
{
    abort(); /* texture-only path */
}
static uint32_t alloc_slice(uint32_t bytes, uint32_t align)
{
    uint32_t at = (used + align - 1) & ~(align - 1);
    assert(at + bytes <= sizeof gpu);
    used = at + bytes;
    return at;
}
static void void_log(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    putchar('\n');
}
/* These draws use only DXGI_FORMAT_R32_FLOAT. */
static const r10_format f32 = {.bytes = 4};
const r10_format *r10_format_get(uint32_t dxgi) { assert(dxgi == 41); return &f32; }
uint32_t r10_gles_vertex_bytes(uint32_t dxgi) { return r10_format_get(dxgi)->bytes; }
'''

VK_DRIVER = r'''
#define TEST_VULKAN 1
typedef unsigned VkBuffer, vk_alloc;
static uint64_t submit_serial = 1, completed_serial;
@TYPES@
typedef vk_buffer buffer;
static int ring_alloc(uint32_t bytes, uint32_t align, vk_slice *out)
{
    out->buffer = 1;
    out->offset = alloc_slice(bytes, align);
    out->map = gpu + out->offset;
    return 1;
}
@STREAM@
static uint8_t *prepare(r10_resource *r, uint32_t lo, uint32_t hi)
{
    vk_slice s;
    assert(stream_buffer(r, lo, hi, &s));
    return s.map;
}
'''

GL_DRIVER = r'''
typedef unsigned GLuint, GLenum;
@TYPES@
typedef gl_res buffer;
#define RING_SEGMENTS 4u
static uint32_t ring_generation = 1;
static uint8_t mirror[sizeof gpu], *ring_map = gpu, *ring_verify;
static GLuint ring_buffer = 1;
#define GL_COPY_WRITE_BUFFER 1
static uint32_t ring_alloc(uint32_t bytes, uint32_t align) { return alloc_slice(bytes, align); }
static void glBindBuffer(GLenum target, GLuint id) { assert(target == GL_COPY_WRITE_BUFFER && id == ring_buffer); }
static void glBufferSubData(GLenum target, uint32_t offset, uint32_t bytes, const void *data)
{
    assert(target == GL_COPY_WRITE_BUFFER && offset + bytes <= sizeof gpu);
    memcpy(gpu + offset, data, bytes);
}
@STREAM@
static uint8_t *prepare(r10_resource *r, uint32_t lo, uint32_t hi)
{
    uint32_t at = stream_buffer(r, lo, hi);
    assert(at != UINT32_MAX);
    if (ring_verify && lo < hi) assert(!memcmp(ring_verify + at + lo, gpu + at + lo, hi - lo));
    return gpu + at;
}
'''

REPLAY = r'''
static r10_resource resource(uint32_t bind, uint32_t va, uint64_t id, buffer *storage)
{
    r10_resource r = {0};
    r.desc.dim = R10_DIM_BUFFER;
    r.desc.usage = D3D10_USAGE_DYNAMIC;
    r.desc.cpu = D3D10_CPU_ACCESS_WRITE;
    r.desc.bind = bind;
    r.desc.width = r.shadow_size = 32;
    r.shadow = va;
    r.subresources = 1;
    r.obj.id = id;
    r.be = storage;
    return r;
}
static void write_bytes(r10_resource *r, uint32_t type, uint32_t offset, uint32_t n, uint8_t value)
{
    uint32_t pitch, slice;
    uint32_t va = r10_resource_map(&device, r, 0, type, 0, &pitch, &slice);
    assert(va && offset + n <= pitch && slice == r->desc.width);
    memset(GPTR(va) + offset, value, n);
    r10_resource_unmap(&device, r, 0);
}
static void check_bytes(const uint8_t *data, uint32_t lo, uint32_t hi, uint8_t expected)
{
    for (uint32_t i = lo; i < hi; ++i) assert(data[i] == expected);
}
static void vertex_stream(int fixed)
{
    buffer storage = {0};
    r10_resource vb = resource(D3D10_BIND_VERTEX_BUFFER, 16, 1, &storage);
    write_bytes(&vb, D3D10_MAP_WRITE_DISCARD, 0, 12, 0x11);
    uint8_t *first = prepare(&vb, 0, 4);
    check_bytes(first, 0, 4, 0x11);
    check_bytes(prepare(&vb, 8, 12), 8, 12, 0x11);
    write_bytes(&vb, D3D10_MAP_WRITE_NO_OVERWRITE, 4, 4, 0x55);
    uint8_t *gap = prepare(&vb, 4, 8);
    assert(gap == first);
    check_bytes(gap, 4, 8, fixed ? 0x55 : 0x11);

    /* Monotone appends share one slice; a DISCARD leaves the old slice intact. */
    for (int discard = 0; discard < 3; ++discard) {
        write_bytes(&vb, D3D10_MAP_WRITE_DISCARD, 0, 4, 0x64 + discard);
        uint8_t *next = prepare(&vb, 0, 4);
        assert(next != first);
        check_bytes(next, 0, 4, 0x64 + discard);
        for (uint32_t i = 4; i < 16; i += 4) {
            write_bytes(&vb, D3D10_MAP_WRITE_NO_OVERWRITE, i, 4, i);
            assert(prepare(&vb, i, i + 4) == next);
            check_bytes(next, i, i + 4, i);
        }
        check_bytes(first, 0, 4, discard ? 0x64 + discard - 1 : 0x11);
        first = next;
    }
    if (fixed) {
        write_bytes(&vb, D3D10_MAP_WRITE, 0, 4, 0x77);
        check_bytes(prepare(&vb, 0, 4), 0, 4, 0x77);
        check_bytes(first, 0, 4, 0x66);
    }
}
static void index_bounds(int fixed, int wide)
{
    buffer storage = {0};
    r10_resource ib = resource(D3D10_BIND_INDEX_BUFFER, 80, 2 + wide, &storage);
    uint32_t size = wide ? 4 : 2, lo, hi;
    device.st.ib = &ib;
    device.st.ib_format = wide ? 42 : 57;
    device.st.ib_offset = size;
    write_bytes(&ib, D3D10_MAP_WRITE_DISCARD, 0, ib.desc.width, 0);
    write_bytes(&ib, D3D10_MAP_WRITE_NO_OVERWRITE, 2 * size, 1, 7);
    index_range(&device, 1, 1, &lo, &hi);
    assert(lo == 7 && hi == 8);
    write_bytes(&ib, D3D10_MAP_WRITE_NO_OVERWRITE, 2 * size, 1, 3);
    index_range(&device, 1, 1, &lo, &hi);
    assert(lo == (fixed ? 3 : 7) && hi == lo + 1);
    if (fixed) {
        /* A new object can reuse the same address, version and unmap count. */
        ib = resource(D3D10_BIND_INDEX_BUFFER, 80, ib.obj.id + 100, &storage);
        write_bytes(&ib, D3D10_MAP_WRITE_DISCARD, 0, ib.desc.width, 0);
        write_bytes(&ib, D3D10_MAP_WRITE_NO_OVERWRITE, 2 * size, 1, 9);
        write_bytes(&ib, D3D10_MAP_WRITE_NO_OVERWRITE, 2 * size, 1, 6);
        index_range(&device, 1, 1, &lo, &hi);
        assert(lo == 6 && hi == 7);
    }
}
static uint8_t *bound_vb, *bound_ib;
static const uint8_t *visible(r10_device *d, const r10_draw *dr, int slot)
{
    uint8_t *bytes = slot < 0 ? bound_ib : bound_vb;
#ifndef TEST_VULKAN
    if (ring_verify) return ring_verify + (bytes - gpu);
#endif
    return bytes;
}
static void sparse_draw(int fixed)
{
    buffer vb_store = {0}, ib_store = {0};
    r10_resource vb = resource(D3D10_BIND_VERTEX_BUFFER, 16, 200, &vb_store);
    r10_resource ib = resource(D3D10_BIND_INDEX_BUFFER, 80, 201, &ib_store);
    r10_layout layout = {.count = 1, .slot_mask = 1};
    layout.el[0].format = 41;
    device.st.layout = &layout;
    device.st.vb[0].buf = &vb;
    device.st.vb[0].offset = device.st.vb[0].stride = 4;
    device.st.ib = &ib;
    device.st.ib_offset = 2;
    device.st.ib_format = 57;
    int gles = device.be == &r10_gles_backend;
    r10_draw dr = {.indexed = 1, .first = 1, .count = 2, .instances = 1, .base_vertex = -1};
    write_bytes(&vb, D3D10_MAP_WRITE_DISCARD, 0, 32, 0x31);
    write_bytes(&ib, D3D10_MAP_WRITE_DISCARD, 0, 32, 0);
    write_bytes(&ib, D3D10_MAP_WRITE_NO_OVERWRITE, 4, 1, 1);
    write_bytes(&ib, D3D10_MAP_WRITE_NO_OVERWRITE, 6, 1, 6);
    index_range(&device, 2, 1, &dr.index_lo, &dr.index_hi);
    dr.vertex_lo = dr.index_lo - 1;
    dr.vertex_hi = dr.index_hi - 1;
    uint32_t lo, hi;
    r10_stream_vertex_range(&device, &dr, 0, gles, &lo, &hi);
    assert(lo == 4 && hi == 28);
    bound_vb = prepare(&vb, lo, hi);
    bound_ib = prepare(&ib, 4, 8);
    /* Indices 1 and 6 consume [4,8) and [24,28). The changed hole is harmless. */
    write_bytes(&vb, D3D10_MAP_WRITE_NO_OVERWRITE, 16, 4, 0x73);
    r10_verify_stream(&device, &dr, visible, gles);
    assert(device.stream_stats.checked == 1 && !device.stream_stats.stale && !device.stream_stats.unavailable);

    /* Once the first draw has finished, the same IB address can name the changed record. */
    write_bytes(&ib, D3D10_MAP_WRITE_NO_OVERWRITE, 6, 1, 4);
    index_range(&device, 2, 1, &dr.index_lo, &dr.index_hi);
    dr.vertex_hi = dr.index_hi - 1;
    r10_stream_vertex_range(&device, &dr, 0, gles, &lo, &hi);
    bound_vb = prepare(&vb, lo, hi);
    bound_ib = prepare(&ib, 4, 8);
    r10_verify_stream(&device, &dr, visible, gles);
    assert(device.stream_stats.checked == 2 && device.stream_stats.stale == !fixed);
    assert(device.stream_stats.vb == !fixed && device.stream_stats.ib == !fixed && device.stream_stats.bounds == !fixed);

    /* Inject a missed upload to exercise the detail cap independently of the A/B run. */
    write_bytes(&vb, D3D10_MAP_WRITE_NO_OVERWRITE, 16, 4, 0x38);
    for (int i = 0; i < 25; ++i) r10_verify_stream(&device, &dr, visible, gles);
    assert(device.stream_stats.checked == 27 && device.stream_stats.stale == 25 + !fixed);
    assert(device.stream_stats.details == 20);
}
#ifdef TEST_VULKAN
static void frame_and_partial_submit(int fixed)
{
    buffer storage = {0};
    r10_resource vb = resource(D3D10_BIND_VERTEX_BUFFER, 16, 99, &storage);
    write_bytes(&vb, D3D10_MAP_WRITE_DISCARD, 0, 12, 0x41);
    uint8_t *old = prepare(&vb, 0, 12);
    ++device.frame;
    ++submit_serial;
    uint8_t *next = prepare(&vb, 0, 4);
    assert(old != next);
    write_bytes(&vb, D3D10_MAP_WRITE_NO_OVERWRITE, 4, 4, 0x72);
    assert(prepare(&vb, 4, 8) == next);
    check_bytes(next, 4, 8, fixed ? 0x72 : 0x41);
    check_bytes(old, 4, 8, 0x41);
    if (fixed) {
        /* Reset coverage lost [0,4); a submit now owns those previously recorded bytes. */
        ++submit_serial;
        uint8_t *partial = prepare(&vb, 0, 4);
        assert(partial != next);
        check_bytes(partial, 0, 4, 0x41);
    }
}
#endif
int main(int argc, char **argv)
{
    assert(argc == 3);
    int fixed = atoi(argv[1]);
    device.frame = 1;
    stream_flags(&device);
    assert(device.stream_fix == fixed);
#ifdef TEST_VULKAN
    device.be = &r10_vulkan_backend;
#else
    device.be = &r10_gles_backend;
    if (atoi(argv[2])) {
        ring_map = NULL;
        if (device.verify_stream) ring_verify = mirror;
    }
#endif
    vertex_stream(fixed);
    index_bounds(fixed, 0);
    index_bounds(fixed, 1);
    if (device.verify_stream) sparse_draw(fixed);
#ifdef TEST_VULKAN
    frame_and_partial_submit(fixed);
#endif
    return 0;
}
'''


def struct(source, name):
    return function(source, f"typedef struct {name} {{") + f" {name};"


class StreamStaleTest(unittest.TestCase):
    def test_perf_flags(self):
        main = (RENDER.parent / "host/android_main.c").read_text(encoding="utf-8")
        allowed = main.split("allowed[] = {", 1)[1].split("};", 1)[0]
        for name in ("BIOSHOCK_R10_VERIFY_STREAM", "BIOSHOCK_R10_STREAM_FIX"):
            self.assertIn(f'"{name}"', allowed)

    def test_stream_bytes_and_index_bounds(self):
        objects, device, present, vk, gles = [
            (RENDER / f"r10_{name}.c").read_text(encoding="utf-8")
            for name in ("objects", "device", "present", "vulkan", "gles")
        ]
        draw_helpers = device[device.index("static void stream_flags("):device.index("static void draw(r10_device *d, int indexed")]
        common = "\n".join([COMMON, function(present, "int r10_env_int("),
                            function(objects, "const uint8_t *r10_buffer_shadow("),
                            function(objects, "uint32_t r10_resource_map("), function(objects, "void r10_resource_unmap("), draw_helpers])
        vk_driver = VK_DRIVER.replace("@TYPES@", struct(vk, "vk_slice") + "\n" + struct(vk, "vk_buffer")).replace(
            "@STREAM@", function(vk, "static vk_buffer *buf(") + "\n" + function(vk, "static int stream_buffer("))
        gl_stream = [function(gles, s) for s in ("static void ring_write(", "static gl_res *res_be(",
                                                 "static void refresh_stream(", "static uint32_t stream_buffer(")]
        gl_driver = GL_DRIVER.replace("@TYPES@", struct(gles, "gl_res")).replace("@STREAM@", "\n".join(gl_stream))
        env, linker = environment("x64")
        with tempfile.TemporaryDirectory() as folder:
            out = Path(folder)
            (out / "com.h").write_text("typedef struct { unsigned guest; } void_com_object;\n"
                                       "typedef struct void_com_class void_com_class;\n", encoding="utf-8")
            for backend, driver in (("vulkan", vk_driver), ("gles", gl_driver)):
                with self.subTest(backend=backend):
                    source = out / f"{backend}.c"
                    source.write_text("\n".join([common, driver, REPLAY]), encoding="utf-8")
                    subprocess.run([str(CLANG), "/nologo", "/O1", "/std:c11", "/clang:--target=x86_64-pc-windows-msvc",
                                    "/c", "/I" + str(out), "/I" + str(RENDER), source.name], cwd=out, env=env, check=True)
                    subprocess.run([str(linker), "/nologo", f"{backend}.obj"], cwd=out, env=env, check=True)
                    uploads = (0, 1) if backend == "gles" else (0,)
                    for fixed, fallback, verify in product((0, 1), uploads, (False, True)):
                        with self.subTest(fixed=fixed, fallback=fallback, verify=verify):
                            child_env = env.copy()
                            child_env.pop("BIOSHOCK_R10_STREAM_FIX", None)
                            child_env.pop("BIOSHOCK_R10_VERIFY_STREAM", None)
                            if not fixed:
                                child_env["BIOSHOCK_R10_STREAM_FIX"] = "0"
                            if verify:
                                child_env["BIOSHOCK_R10_VERIFY_STREAM"] = "1"
                            result = subprocess.run([str(out / f"{backend}.exe"), str(fixed), str(fallback)],
                                                    env=child_env, text=True, capture_output=True, timeout=30)
                            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                            self.assertEqual(result.stdout.count("[r10] STREAM_STALE "), 20 if verify else 0)


if __name__ == "__main__":
    unittest.main()

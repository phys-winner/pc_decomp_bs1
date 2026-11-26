"""The native renderer's shader library without Direct3D bytecode (tools/build_shaderlib10.py, android/render10).

Python: the game's include handler is emulated (a recorded factory file, a shaders.spk file, an unresolved
line kept), a device request's pass and factory files are found from its resolved text, the record key's
family levels drop the scene's defines or keep only the literals, and an R10REQ2 request parses.

C: the production r10_shaderlib.c and r10_engine.c run against a library made by the production pack() (zlib
stored blocks) in a flat guest memory.  The FShaderCache::Find seam (the shader getter's call at Bioshock.exe 0x109AFABE) answers a cache
identity of ShaderCache.pcs10 with a token of its program, refuses a stale time stamp, answers a variant the
shipped cache lacks by its record key (the defines and the literals the factories recorded, formatted as
FShaderCompiler10 does), gives a missing variant its family's program and writes an R10REQ2 request whose
record key and defines Python reproduces, runs the original Find for another frame, and the Load seam
(0x10AF77F0) returns without reading the file.
"""
from pathlib import Path
import struct
import subprocess
import sys
import unittest
from unittest import mock

from tests.host_environment import CLANG, environment

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / "build/tests/r10-shaderlib"
sys.path.insert(0, str(ROOT / "tools"))
import build_shaderlib10 as lib  # noqa: E402
import build_shader_warmup as warmup  # noqa: E402

NAME = "LayerLighting.hlslMaterialFactory_Shader.hlslVertexFactory_NULL.hlslPSMain"
VARIANT = "Base.hlslMaterialFactory_Fluid.hlslVertexFactory_NULL.hlslPSMain"
GUID = (0x11, 0x22, 0x33, 0x44)
TIME = 1183689085
LITERALS = ("\tDiffuseBind.samplerSlot = 3;\\\n" "SpecularMaskBind.enabled = true;\\\n" "\tUseXGBRNormal = false;\\\n"
            "\tLayerCount = -2;\\\n")
DEFINES = [("MAX_INCIDENT_LIGHTS", "4"), ("RENDER_BASE_PASS", "1"), ("DEFINE_LITERALS", LITERALS), ("D3DDRV10", "1")]


def row(index, kind, sha):
    return {"hash": "%016x" % (0x1000 + index), "size": 64 + index, "checksum": "%032x" % index, "type": kind,
            "bindings": [[6, 160 * kind, 0x7FFFFFFF]], "combined": [], "ubos": [], "icb": "", "spirv": bytes(16 + index),
            "spirv_noclip": b"", "es": "#version 320 es\n// %d\n" % index, "interface": b"BSIF" + bytes(28),
            "push_constants": False, "clip_distance": False, "input_mask": 1, "output_mask": 1, "sha": sha}


def library():
    rows = [row(0, 0, "material-ps"), row(1, 1, "material-vs"), row(2, 0, "record"), row(3, 0, "family"),
            row(4, 0, "placeholder-ps"), row(5, 1, "placeholder-vs")]
    materials = [(NAME, GUID, TIME, "material-ps"), (NAME.replace("PSMain", "VSMain"), GUID, TIME, "material-vs")]
    records = [(lib.record_key(VARIANT, "ps_4_0", DEFINES), "record", TIME),
               (lib.record_key(VARIANT, "ps_4_0", DEFINES, 2), "family", TIME)]
    requests = [(0xFFFFFFFFFFFFFF01, "placeholder-vs"), (0xFFFFFFFFFFFFFF02, "placeholder-ps")]
    real = lib.zlib.compress
    with mock.patch.object(lib.zlib, "compress", lambda data, level=-1: real(data, 0)):
        data, key, sizes = lib.pack(rows, materials, records, requests, b"test")
    order = sorted(range(len(rows)), key=lambda i: (int(rows[i]["hash"], 16), rows[i]["size"]))
    return data, key, {rows[i]["sha"]: n for n, i in enumerate(order)}


HARNESS = r'''
#define _CRT_SECURE_NO_WARNINGS
#include <assert.h>
#include <direct.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define strcasecmp _stricmp
#define mkdir(path, mode) _mkdir(path)
#define R10_LOG(...) (printf("[r10] " __VA_ARGS__), printf("\n"))

/* ---------------------------------------------------------------- guest memory and registers */
#define BASE 0x01000000u
static uint8_t guest[1u << 22];
static uint32_t heap = BASE + 0x200000u;
#define GPTR(va) ((void *)(guest + ((uint32_t)(va) - BASE)))
#define MEM32(va) (*(uint32_t *)GPTR(va))
#define MEM16(va) (*(uint16_t *)GPTR(va))
static int void_vm_readable(uint32_t va, uint32_t n) { return va >= BASE && (uint64_t)va + n <= BASE + sizeof guest; }
static uint32_t void_galloc(uint32_t n) { uint32_t va = heap; heap += (n + 15) & ~15u; return va; }
static size_t void_gstrlen(uint32_t va) { return strlen((const char *)GPTR(va)); }
static const char *void_data_dir(void) { return "OUT_DIR"; }
static uint32_t g_eax, g_esp, g_ebx;
/* The original FShaderCache::Find: an empty cache (the engine then compiles), ret 8. */
static int original_calls;
void sub_10AF7D40(void) { ++original_calls; g_eax = 0; g_esp += 12; }

/* ---------------------------------------------------------------- zlib: the test library has stored blocks */
typedef unsigned long uLongf;
#define Z_OK 0
static int uncompress(uint8_t *out, uLongf *length, const uint8_t *in, uLongf size)
{
    uLongf at = 2, n = 0;
    for (;;) {
        uint8_t head = in[at];
        if ((head & 6) || at + 5 > size) return -1;
        uint16_t len = (uint16_t)(in[at + 1] | in[at + 2] << 8);
        at += 5;
        if (n + len > *length) return -1;
        memcpy(out + n, in + at, len);
        n += len; at += len;
        if (head & 1) break;
    }
    *length = n;
    return Z_OK;
}

typedef struct r10_define { const char *name, *value; } r10_define;
typedef struct r10_device {
    struct { uint32_t created, found, compiles, compile_misses, compile_fallbacks;
             uint32_t native_identity, native_record, native_misses, native_fallbacks, cache_loads_skipped, from_dxbc; } shader_stats;
} r10_device;
static r10_device device, *r10_dev = &device;
#include "r10_shaderlib.h"

SHADERLIB_SOURCE

ENGINE_SOURCE

/* ---------------------------------------------------------------- the frame of sub_109AF8E0 */
static uint32_t wide(const char *s)
{
    size_t n = strlen(s) + 1;
    uint32_t va = void_galloc((uint32_t)(2 * n));
    for (size_t i = 0; i < n; ++i) MEM16(va + 2 * (uint32_t)i) = (uint8_t)s[i];
    return va;
}
static uint32_t narrow(const char *s) { uint32_t va = void_galloc((uint32_t)strlen(s) + 1); strcpy((char *)GPTR(va), s); return va; }
static void put_fstring(uint32_t at, const char *s) { MEM32(at) = wide(s); MEM32(at + 4) = (uint32_t)strlen(s) + 1; MEM32(at + 8) = (uint32_t)strlen(s) + 1; }

static uint32_t descriptor, identity, stack;
static void frame(const char *pass, const char *factory, const char *entry, const char *name, int pixel, int lights,
                  int slot, uint32_t ret, uint32_t time, const uint32_t guid[4])
{
    descriptor = void_galloc(28);
    MEM32(descriptor) = (uint32_t)pixel;
    put_fstring(descriptor + 4, pass);
    put_fstring(descriptor + 16, entry);
    identity = void_galloc(0x44 + 24);
    put_fstring(identity, name);
    for (int i = 0; i < 4; ++i) MEM32(identity + 12 + 4 * i) = guid[i];
    uint32_t recorder = void_galloc(48);
    MEM32(identity + 0x44) = recorder;
    for (int i = 0; i < 4; ++i) MEM32(identity + 0x48 + 4 * i) = guid[i];   /* the compilation data's GUID */
    uint32_t files = void_galloc(16);
    MEM32(files) = wide("MATERIALFACTORY"); MEM32(files + 4) = wide(factory);
    MEM32(files + 8) = wide("VERTEXFACTORY"); MEM32(files + 12) = wide("VertexFactory_NULL.hlsl");
    MEM32(recorder) = files; MEM32(recorder + 4) = 2;
    uint32_t literals = void_galloc(64);
    char value[16];
    MEM32(literals) = narrow("DiffuseBind"); MEM32(literals + 4) = narrow("samplerSlot"); MEM32(literals + 8) = 1; MEM32(literals + 12) = (uint32_t)slot;
    MEM32(literals + 16) = narrow("SpecularMaskBind"); MEM32(literals + 20) = narrow("enabled"); MEM32(literals + 24) = 0; MEM32(literals + 28) = 1;
    MEM32(literals + 32) = 0; MEM32(literals + 36) = narrow("UseXGBRNormal"); MEM32(literals + 40) = 0; MEM32(literals + 44) = 0;
    MEM32(literals + 48) = 0; MEM32(literals + 52) = narrow("LayerCount"); MEM32(literals + 56) = 1; MEM32(literals + 60) = (uint32_t)-2;
    MEM32(recorder + 12) = literals; MEM32(recorder + 16) = 4;
    uint32_t defines = void_galloc(16);
    snprintf(value, sizeof value, "%i", lights);
    MEM32(defines) = narrow("MAX_INCIDENT_LIGHTS"); MEM32(defines + 4) = narrow(value);
    MEM32(defines + 8) = narrow("RENDER_BASE_PASS"); MEM32(defines + 12) = narrow("1");
    MEM32(recorder + 24) = defines; MEM32(recorder + 28) = 2;
    stack = void_galloc(64) + 32;
    MEM32(stack) = ret; MEM32(stack + 4) = identity; MEM32(stack + 8) = time;
    g_esp = stack; g_ebx = descriptor; g_eax = 0xDEADBEEFu;
}

static uint32_t found(void)
{
    assert(g_esp == stack + 12);
    uint32_t record = g_eax;
    assert(MEM32(record) == R10_TOKEN_SIZE);
    const r10_lib_shader *s = r10_shaderlib_token((const uint8_t *)GPTR(MEM32(record + 8)), MEM32(record));
    assert(s);
    return r10_shaderlib_index(s);
}

int main(void)
{
    static const uint32_t guid[4] = { 0x11, 0x22, 0x33, 0x44 }, other[4] = { 0x11, 0x22, 0x33, 0x45 };
    assert(r10_shaderlib_open() && r10_native_shaders());
    assert(r10_shaderlib_key() == LIBRARY_KEY);
    /* A record of ShaderCache.pcs10: by its cache identity. */
    frame("LayerLighting.hlsl", "MaterialFactory_Shader.hlsl", "PSMain", "LAYERLIGHTING.hlslMaterialFactory_Shader.hlslVertexFactory_NULL.hlslPSMain",
          1, 4, 3, 0xDEAD0000u, TIME, guid);
    assert((void_bioshock_shader_cache_find10(), original_calls == 0));
    assert(found() == INDEX_MATERIAL_PS);
    assert(device.shader_stats.native_identity == 1);
    /* The same token is handed out again; its bytes name the program in any copy. */
    uint32_t first = g_eax;
    frame("LayerLighting.hlsl", "MaterialFactory_Shader.hlsl", "PSMain", "LayerLighting.hlslMaterialFactory_Shader.hlslVertexFactory_NULL.hlslPSMain",
          1, 4, 3, 0xDEAD0000u, TIME, guid);
    assert((void_bioshock_shader_cache_find10(), original_calls == 0) && g_eax == first);
    uint64_t material = r10_name_key("LayerLighting.hlslMaterialFactory_Shader.hlslVertexFactory_NULL.hlslPSMain");
    assert(r10_shaderlib_material(material, guid, TIME + 1) == -1);
    /* Both GUIDs hash to the same recent slot, but only one is in the library. */
    uint32_t collision[4] = { guid[0], guid[1], guid[2], guid[3] ^ 4096 };
    assert(r10_shaderlib_material(material, collision, TIME) == -1);
    assert(r10_shaderlib_material(material, guid, TIME) == INDEX_MATERIAL_PS);
    uint8_t copy[R10_TOKEN_SIZE];
    memcpy(copy, GPTR(MEM32(first + 8)), sizeof copy);
    const r10_lib_shader *program = r10_shaderlib_token(copy, sizeof copy);
    assert(program && !r10_shaderlib_token(copy, sizeof copy - 1));
    assert(r10_shaderlib_by_hash(program->hash, program->type) == program);
    assert(!r10_shaderlib_by_hash(program->hash, !program->type));
    assert(!r10_shaderlib_by_hash(0, program->type));
    copy[8] ^= 1;
    assert(!r10_shaderlib_token(copy, sizeof copy));
    /* Another frame (its compilation data names another GUID): the original body. */
    frame("LayerLighting.hlsl", "MaterialFactory_Shader.hlsl", "PSMain", "LayerLighting.hlslMaterialFactory_Shader.hlslVertexFactory_NULL.hlslPSMain",
          1, 4, 3, 0xDEAD0000u, TIME, guid);
    MEM32(identity + 0x48) ^= 1;
    void_bioshock_shader_cache_find10();
    assert(original_calls == 1 && g_esp == stack + 12 && g_eax == 0);
    original_calls = 0;
    /* A variant the shipped cache lacks: by its record key. */
    frame("Base.hlsl", "MaterialFactory_Fluid.hlsl", "PSMain", "Base.hlslMaterialFactory_Fluid.hlslVertexFactory_NULL.hlslPSMain",
          1, 4, 3, 0xDEAD0000u, TIME, other);
    assert((void_bioshock_shader_cache_find10(), original_calls == 0));
    assert(found() == INDEX_RECORD);
    assert(device.shader_stats.native_record == 1);
    /* The same variant with another stamp: missing; the literals' family is time-checked too, so the placeholder. */
    frame("Base.hlsl", "MaterialFactory_Fluid.hlsl", "PSMain", "Base.hlslMaterialFactory_Fluid.hlslVertexFactory_NULL.hlslPSMain",
          1, 4, 3, 0xDEAD0000u, TIME + 1, other);
    assert((void_bioshock_shader_cache_find10(), original_calls == 0));
    assert(found() == INDEX_PLACEHOLDER_PS);
    /* Other lights and another sampler slot: missing; its literals' family answers, a request is written. */
    frame("Base.hlsl", "MaterialFactory_Fluid.hlsl", "PSMain", "Base.hlslMaterialFactory_Fluid.hlslVertexFactory_NULL.hlslPSMain",
          1, 2, 3, 0xDEAD0000u, TIME, other);
    assert((void_bioshock_shader_cache_find10(), original_calls == 0));
    assert(found() == INDEX_FAMILY);
    assert(device.shader_stats.native_misses == 2 && device.shader_stats.native_fallbacks == 1);
    /* A name its files do not make: no record path, the original body. */
    frame("Base.hlsl", "MaterialFactory_Fluid.hlsl", "PSMain", "Base.hlslMaterialFactory_Shader.hlslVertexFactory_NULL.hlslPSMain",
          1, 4, 3, 0xDEAD0000u, TIME, other);
    void_bioshock_shader_cache_find10();
    assert(original_calls == 1 && g_esp == stack + 12);
    original_calls = 0;
    /* FShaderCache::Load returns at once. */
    g_esp = stack; g_eax = 0;
    assert(void_bioshock_shader_cache_load() == 1 && g_eax == 1 && g_esp == stack + 8);
    assert(device.shader_stats.cache_loads_skipped == 1);
    printf("PASS r10_shaderlib\n");
    return 0;
}
'''


def strip_includes(path):
    return "\n".join(line for line in path.read_text(encoding="utf-8").splitlines() if not line.startswith("#include"))


class ShaderLibraryPythonTest(unittest.TestCase):
    FILES = {
        "pass.hlsl": ("Pass.hlsl", 30, b"#include <MATERIALFACTORY>\n#include <VERTEXFACTORY>\nbody;\n  # include \"Common.hlsl\"\n"
                                         b"#include <MISSING>\ntail"),
        "materialfactory_a.hlsl": ("MaterialFactory_A.hlsl", 40, b"#include \"Common.hlsl\"\nfactory a\n"),
        "materialfactory_b.hlsl": ("MaterialFactory_B.hlsl", 50, b"factory b\n"),
        "vertexfactory_null.hlsl": ("VertexFactory_NULL.hlsl", 20, b"vf\n"),
        "common.hlsl": ("Common.hlsl", 10, b"common\n"),
    }

    def test_gles_device_variants(self):
        original = """#version 320 es
#extension GL_EXT_clip_cull_distance : require
out float gl_ClipDistance[1];
layout(binding = 0, std140) uniform bs_vs_cb0
{
    vec4 m[4096];
} cb0;
void main()
{
    gl_Position = cb0.m[int(cb0.m[2].x)] + cb0.m[1];
    gl_ClipDistance[0] = gl_Position.w;
}
"""
        source, size = lib.es_device_variants(original)
        self.assertEqual(size, 65536)
        self.assertIn("#ifdef BS_CLIP_DISTANCE\n#extension GL_EXT_clip_cull_distance", source)
        self.assertNotIn("out float gl_ClipDistance", source)
        self.assertIn("#ifdef BS_CLIP_DISTANCE\n    gl_ClipDistance[0]", source)
        self.assertIn("#ifdef BS_CB0_TEXTURE\nlayout(binding = BS_CB0_UNIT)", source)
        self.assertIn("#define bs_c0(i) cb0.m[i]", source)
        self.assertIn("bs_c0(int(bs_c0(2).x)) + bs_c0(1)", source)

    def test_gles_clip_planes(self):
        # The same fragment shader can pair with a VS that writes fewer planes.
        # The shorter VS must initialize the extra lane so it cannot cause a discard.
        vs = """#version 320 es
#extension GL_EXT_clip_cull_distance : require
out float gl_ClipDistance[1];
layout(location = 15) out vec4 o15;
void main()
{
    o15 = vec4(1.0);
    gl_Position = o15;
    gl_ClipDistance[0] = o15.x;
}
"""
        previous = vs.replace("#extension GL_EXT_clip_cull_distance : require",
                              "#ifdef BS_CLIP_DISTANCE\n#extension GL_EXT_clip_cull_distance : require\n#endif")
        previous = previous.replace("out float gl_ClipDistance[1];\n", "")
        previous = previous.replace("    gl_ClipDistance[0] = o15.x;",
                                    "#ifdef BS_CLIP_DISTANCE\n    gl_ClipDistance[0] = o15.x;\n#endif")
        packed, _ = lib.es_device_variants(vs, clip_count=2)
        hardware, variants = packed.split("\n#if 0 // BS_CLIP_FALLBACK\n")
        fragment, vertex = variants.split("\n// BS_CLIP_VERTEX\n")
        self.assertEqual(hardware, previous)
        self.assertIn("out vec4 bs_link_reg15;", vertex)
        self.assertNotIn("location = 15", vertex)
        self.assertIn("out highp vec2 bs_clip_distances;", vertex)
        self.assertIn("bs_clip_distances[0] = o15.x;", vertex)
        self.assertIn("bs_clip_distances[1] = 0.0;", vertex)
        self.assertIn("in highp vec2 bs_clip_distances;", fragment)
        self.assertIn("if (any(lessThan(bs_clip_distances, vec2(0.0)))) discard;", fragment)
        self.assertNotIn("gl_ClipDistance", vertex + fragment)

    def test_warmup_prioritizes_gles_gameplay_programs(self):
        recipe = struct.pack("<QQ", 5, 6).ljust(warmup.RECIPE_SIZE, b"\0")
        vk = struct.pack("<Q", 20) + recipe
        gles = "\n".join(f"GLES first use frame {frame}: vs {vs:016x} ps {ps:016x}"
                         for frame, vs, ps in ((9, 1, 5), (11, 2, 5), (12, 3, 5), (12, 4, 5))).encode()
        seed = warmup.build([(vk, 10)], [], [(gles, 10)])
        self.assertEqual(struct.unpack_from("<4s5I", seed), (b"BSWP", 1, 4, 1, warmup.RECIPE_SIZE, 0))
        self.assertEqual(list(struct.iter_unpack("<QQ", seed[24:88])), [(3, 5), (4, 5), (2, 5), (5, 6)])
        self.assertEqual(seed[88:], recipe)

    def test_include_handler_and_identity(self):
        pairs = (("MATERIALFACTORY", "MaterialFactory_A.hlsl"), ("VERTEXFACTORY", "VertexFactory_NULL.hlsl"))
        identity = lib.Identity(self.FILES, set())
        text = identity.text("Pass.hlsl", pairs)
        self.assertEqual(text, b"\n\ncommon\n\nfactory a\n\n\nvf\n\nbody;\n\ncommon\n\n#include <MISSING>\ntail")
        self.assertEqual(identity(text, "PSMain"), ("pass.hlsl", (("MATERIALFACTORY", "materialfactory_a.hlsl"),
                                                                  ("VERTEXFACTORY", "vertexfactory_null.hlsl"))))
        self.assertEqual(lib.shader_name(self.FILES, "pass.hlsl", pairs, "PSMain"),
                         "Pass.hlslMaterialFactory_A.hlslVertexFactory_NULL.hlslPSMain")
        self.assertEqual(lib.shader_time(self.FILES, "pass.hlsl", pairs), 40)
        # A text no combination makes.
        with self.assertRaises(ValueError):
            identity(text + b"x", "PSMain")

    def test_record_key_levels(self):
        base = lib.record_key(VARIANT, "ps_4_0", DEFINES)
        self.assertEqual(base, lib.record_key(VARIANT.upper(), "ps_4_0", DEFINES))
        self.assertNotEqual(base, lib.record_key(VARIANT, "vs_4_0", DEFINES))
        lights = [("MAX_INCIDENT_LIGHTS", "2")] + DEFINES[1:]
        self.assertNotEqual(base, lib.record_key(VARIANT, "ps_4_0", lights))
        self.assertEqual(lib.record_key(VARIANT, "ps_4_0", DEFINES, 1), lib.record_key(VARIANT, "ps_4_0", lights, 1))
        other = [("RENDER_BASE_PASS", "0")] + DEFINES[:1] + DEFINES[2:]
        self.assertEqual(lib.record_key(VARIANT, "ps_4_0", DEFINES, 2), lib.record_key(VARIANT, "ps_4_0", other, 2))
        self.assertNotEqual(lib.record_key(VARIANT, "ps_4_0", DEFINES, 1), lib.record_key(VARIANT, "ps_4_0", other, 1))

    def test_record_request_parses(self):
        literal = "\tA.b = 1;\\\n"
        text = ("R10REQ2\nrecord=0123456789abcdef\nname=%s\npass=Base.hlsl\npair=MATERIALFACTORY:MaterialFactory_Fluid.hlsl\n"
                "pair=VERTEXFACTORY:VertexFactory_NULL.hlsl\nentry=PSMain\nprofile=ps_4_0\nflags=4096\n"
                "guid=00000011000000220000003300000045\ntime=7\ndefine=15:DEFINE_LITERALS%d:%s\ndefine=8:D3DDRV101:1\nend\n"
                % (VARIANT, len(literal), literal)).encode("latin-1")
        version, fields, pairs, defines, source = lib.parse_request(text)
        self.assertEqual((version, source, fields["pass"], fields["time"]), (2, None, "Base.hlsl", "7"))
        self.assertEqual(pairs, [("MATERIALFACTORY", "MaterialFactory_Fluid.hlsl"), ("VERTEXFACTORY", "VertexFactory_NULL.hlsl")])
        self.assertEqual(defines, [("DEFINE_LITERALS", literal), ("D3DDRV10", "1")])

    def test_interface_keeps_only_tables(self):
        def chunk(fourcc, data):
            return fourcc + struct.pack("<I", len(data)) + data
        parts = [chunk(b"RDEF", b"r" * 8), chunk(b"ISGN", b"i" * 12), chunk(b"SHDR", struct.pack("<I", 0x40050) + b"c" * 8),
                 chunk(b"OSGN", b"o" * 4), chunk(b"STAT", b"s" * 4)]
        at, offsets = 32 + 4 * len(parts), []
        for p in parts:
            offsets.append(at)
            at += len(p)
        dxbc = b"DXBC" + bytes(16) + struct.pack("<II", 1, at) + struct.pack("<I", len(parts)) + struct.pack(
            "<%dI" % len(parts), *offsets) + b"".join(parts)
        out = lib.interface(dxbc)
        self.assertEqual(out[:8], b"BSIF" + struct.pack("<I", 0x40050))
        chunks = lib.dxbc_chunks(b"DXBC" + out[4:])
        self.assertEqual(chunks, {b"RDEF": b"r" * 8, b"ISGN": b"i" * 12, b"OSGN": b"o" * 4})


sys.path.insert(0, str(ROOT / "tools/shaderlib10"))
import variants  # noqa: E402


class SettingsTwinsTest(unittest.TestCase):
    """tools/shaderlib10/variants.py: the GUID packing of the engine's field writers, the compiler's define list, the
    learned unrecorded bits and the twins of the settings fields."""
    LAYOUT = [("DiffuseBind.samplerSlot", "int"), ("DiffuseBind.channelID", "int"), ("TwoSided", "bool"),
              ("Lit.enabled", "bool"), ("MAX_INCIDENT_LIGHTS", "define"), ("USES_SPECULAR_CUBEMAP", "define"),
              (variants.HIDDEN, "bool"), ("ENABLE_ALPHATEST", "define")]

    def values(self, slot_value, cube, alpha, lights=5):
        v = dict(zip([n for n, k in self.LAYOUT], [slot_value, 2, 1, 0, lights, cube, 0, alpha]))
        return [(n, k, v[n]) for n, k in self.LAYOUT]

    def test_packing_and_defines(self):
        values = self.values(3, 1, 1)
        guid = variants.encode(values, 2)
        # slot 3 (2 bits), channel 2 (2 bits), TwoSided, Lit.enabled, lights 5 (3 bits), cube, the unrecorded bit, alpha
        self.assertEqual(guid, 0b1_0_1_101_0_1_10_11)
        self.assertIn((2, values), variants.decode(guid, self.LAYOUT))
        self.assertIsNone(variants.encode(self.values(4, 0, 0), 2))   # a slot value wider than its field
        self.assertEqual(variants.define_list(values), [
            ("MAX_INCIDENT_LIGHTS", "5"), ("USES_SPECULAR_CUBEMAP", "1"), ("ENABLE_ALPHATEST", "1"),
            ("DEFINE_LITERALS", "\tDiffuseBind.samplerSlot = 3;\\\n\tDiffuseBind.channelID = 2;\\\n"
                                "\tTwoSided = true;\\\nLit.enabled = false;\\\n"), ("D3DDRV10", "1")])
        # The device's own literal formats parse back into the same fields (the hidden bit is not recorded).
        recorded = [x for x in values if x[0] != variants.HIDDEN]
        self.assertEqual(variants.fields(variants.define_list(values)),
                         [x for x in recorded if x[1] != "define"] + [x for x in recorded if x[1] == "define"])

    def test_unrecorded_bit_is_learned(self):
        recorded = [(n, k) for n, k in self.LAYOUT if n != variants.HIDDEN]
        group = []
        for alpha in (0, 1):
            for lights in (1, 2, 3):
                values = self.values(1, alpha ^ 1, alpha, lights)
                group.append((variants.encode(values, 2), [x for x in values if x[0] != variants.HIDDEN]))
        hidden, fit = variants.hidden_bits(group)
        self.assertEqual((hidden, fit), ((recorded.index(("ENABLE_ALPHATEST", "define")),), 1.0))

    def test_twins_of_capable_materials(self):
        layouts = {"P.hlslF.hlslPSMain": {"shapes": [{"fields": [list(x) for x in self.LAYOUT]}], "pass": "P.hlsl",
                                          "pairs": [], "entry": "PSMain", "profile": "ps_4_0"}}

        def program(defines):
            d = dict(defines)
            # The cube map changes the code only for slot 3 materials (their literals use it).
            used = d["USES_SPECULAR_CUBEMAP"] if "samplerSlot = 3" in d["DEFINE_LITERALS"] else "x"
            return ("DXBC" + d["DEFINE_LITERALS"] + d["MAX_INCIDENT_LIGHTS"] + used).encode()

        class Pool:
            calls = 0

            def key(self, *job):
                return variants.CompilePool.key(*job)

            def run(self, jobs):
                Pool.calls += len(jobs)
                return {self.key(*job): program(job[4]) for job in jobs}, {}

        records = []
        # Material slot 3 is shipped with the cube map on (capable); material slot 1 only off (not capable).
        for slot_value, cube in ((3, 1), (1, 0)):
            values = self.values(slot_value, cube, 0)
            guid = variants.encode(values, 2)
            words = tuple((guid >> (32 * i)) & 0xFFFFFFFF for i in range(4))
            records.append(("P.hlslF.hlslPSMain", words, 77, program(variants.define_list(values))))
        # A record whose shipped code is not what its reading compiles to: never twinned.
        wrong = self.values(3, 1, 0, lights=2)
        wg = variants.encode(wrong, 2)
        records.append(("P.hlslF.hlslPSMain", tuple((wg >> (32 * i)) & 0xFFFFFFFF for i in range(4)), 77, b"DXBC other"))
        proven, twins, report = variants.shipped_variants(records, layouts, Pool(), material_summary={"tuples": {}})
        self.assertEqual(len(twins), 1, report)
        name, words, stamp, dxbc, profile, defines, rule = twins[0]
        guid = sum(w << (32 * i) for i, w in enumerate(words))
        self.assertEqual((name, stamp, profile, guid), ("P.hlslF.hlslPSMain", 77, "ps_4_0",
                                                         variants.encode(self.values(3, 0, 0), 2)))
        self.assertEqual(dict(defines)["USES_SPECULAR_CUBEMAP"], "0")
        self.assertEqual(report["records decoded and proven"], 2)   # both real records; the third compiles otherwise
        self.assertEqual(report["twins with code of their own"], 1)
        self.assertEqual((rule, len(proven)), ("cube map off", 2))

    def test_rule_slot_indices(self):
        layout = [("DiffuseBind.samplerSlot", "int"), ("OpacityBind.samplerSlot", "int"),
                  ("OpacityBind.channelID", "int"), ("ENABLE_ALPHATEST", "define")]
        values = [(n, k, v) for (n, k), v in zip(layout, (1, 0, 3, 0))]
        # The opacity map takes the diffuse map's index (the same texture): the width stays 1.
        twin, slot = variants._apply(values, 1, {"OpacityBind.samplerSlot": (0, 1)})
        self.assertEqual(([v for n, k, v in twin], slot), ([1, 1, 3, 0], 1))
        # Dropping the only texture leaves no slot (width 0); a gap in the indices is not modelled.
        twin, slot = variants._apply(values, 1, {"DiffuseBind.samplerSlot": (1, 0)})
        self.assertEqual(([v for n, k, v in twin], slot), ([0, 0, 3, 0], 0))
        self.assertIsNone(variants._apply(values, 1, {"OpacityBind.samplerSlot": (0, 2), "DiffuseBind.samplerSlot": (1, 0)}))
        self.assertIsNone(variants._apply(values, 1, {"OpacityBind.samplerSlot": (1, 0)}))   # the record has 0

    def test_translucent_records(self):
        fields = [(f, "int", 0) for f in variants.materials.FACTORY_FIELDS]
        key = variants.materials.factory_key(fields)
        name = "GlobalLitLighting.hlslMaterialFactory_Shader.hlslVertexFactory_NULL.hlsl"
        self.assertTrue(variants._blended(name + "PSMain", fields, {key}))
        self.assertFalse(variants._blended(name + "PSMain", fields, set()))
        self.assertFalse(variants._blended(name + "PSMain", fields + [("ENABLE_ALPHATEST", "define", 1)], {key}))
        self.assertTrue(variants._blended(name + "VSMain", [("USING_ANIMATORS", "define", 0)], set()))


import materials as game_materials  # noqa: E402
import packages as game_packages  # noqa: E402


class GameMaterialsTest(unittest.TestCase):
    """tools/shaderlib10/packages.py and materials.py: the property stream of package exports and the slot rules of
    FMaterialFactory_Shader."""

    def test_struct_size_leaves_out_member_size_bytes(self):
        names = ["None", "Opacity", "MaskMaterial", "Material", "Channel", "Glossiness"]
        p = {"names": names, "exports": [{"class": "Texture", "name": "Mask"}], "imports": []}

        def name(i):
            return bytes([i]) + struct.pack("<I", 0)
        # Opacity (struct MaskMaterial, size code 5): Material = export 1 (object, explicit size 1), Channel = 2;
        # the declared struct size counts the members without the explicit size byte of Material.
        inner = name(3) + bytes([0x55, 1, 1]) + name(4) + bytes([0x01, 2]) + name(0)
        stream = name(1) + bytes([0x5A]) + name(2) + bytes([len(inner) - 1]) + inner
        stream += name(5) + bytes([0x24]) + struct.pack("<f", 64.0) + name(0)
        r = game_packages.bt.Reader(stream)
        out = game_packages._to_dict(p, game_packages.tagged(p, r))
        self.assertEqual(out, {"Opacity": {"Material": ("Texture", "Mask"), "Channel": 2}, "Glossiness": 64.0})
        self.assertEqual(r.position, len(stream))

    def test_shader_slots(self):
        objects = {
            ("p", 1): ("Shader", {"Diffuse": ("Texture", "D"), "NormalMap": ("Texture", "D"),
                                  "Opacity": {"Material": ("Texture", "O"), "Channel": 1},
                                  "SpecularMask": {"Material": ("Shader", "S")},
                                  "Emissive": ("MaterialSequence", "Q"), "Glossiness": 8.0,
                                  "SpecularBrightness": 1.0}),
            ("p", 2): ("Texture", {"Format": 12}), ("p", 3): ("Texture", {"Format": 3}),
            ("p", 4): ("Shader", {}), ("p", 5): ("MaterialSequence", {"SequenceItems": [
                {"Material": ("Texture", "E")}, {"Material": ("Shader", "S")}]}),
            ("p", 6): ("Texture", {"Format": 3})}
        refs = {("Texture", "D"): ("p", 2), ("Texture", "O"): ("p", 3), ("Shader", "S"): ("p", 4),
                ("MaterialSequence", "Q"): ("p", 5), ("Texture", "E"): ("p", 6)}

        class World:
            def props(self, ref):
                return objects[ref][1]

            def export(self, ref):
                return {"class": objects[ref][0], "name": str(ref)}

            def resolve(self, ref, value):
                return None if value is None else refs[value]
        out = game_materials.shader_tuples(World(), ("p", 1))
        fields = game_materials.FACTORY_FIELDS
        got = {}
        for width, values in out:
            v = dict(zip(fields, values))
            got[(v["SelfIlluminationBind.samplerSlot"], v["UseSpecular"])] = (width, v)
        # The sequence resolves to a texture (index 3, width 2) or to a Shader (no slot, width 2 for two textures).
        self.assertEqual(sorted(got), [(0, 0), (0, 1), (3, 0), (3, 1)])
        width, v = got[(3, 1)]
        self.assertEqual((width, v["DiffuseBind.samplerSlot"], v["NormalMapBind.samplerSlot"],
                          v["OpacityBind.samplerSlot"], v["OpacityBind.channelID"], v["SpecularMaskBind.samplerSlot"],
                          v["SpecularMaskBind.channelID"], v["UseXGBRNormal"]), (2, 1, 1, 2, 0, 0, 3, 1))
        self.assertEqual(got[(0, 1)][0], 2)


class ShaderLibraryRuntimeTest(unittest.TestCase):
    def test_production_engine_seams(self):
        data, key, index = library()
        OUT.mkdir(parents=True, exist_ok=True)
        (OUT / "bs_shaderlib10.bin").write_bytes(data)
        requests = OUT / "r10-requests"
        if requests.is_dir():
            for old in requests.iterdir():
                old.unlink()
        harness = HARNESS.replace("OUT_DIR", OUT.as_posix()).replace("TIME", str(TIME))
        harness = harness.replace("LIBRARY_KEY", "0x%016xull" % key)
        for name, sha in (("MATERIAL_PS", "material-ps"), ("RECORD", "record"), ("FAMILY", "family"),
                          ("PLACEHOLDER_PS", "placeholder-ps")):
            harness = harness.replace("INDEX_" + name, "%du" % index[sha])
        harness = harness.replace("SHADERLIB_SOURCE", strip_includes(ROOT / "android/render10/r10_shaderlib.c"))
        harness = harness.replace("ENGINE_SOURCE", strip_includes(ROOT / "android/render10/r10_engine.c"))
        source = OUT / "r10_shaderlib_test.c"
        source.write_text(harness, encoding="utf-8")
        env, linker = environment("x64")
        env["BIOSHOCK_SHADERLIB10"] = str(OUT / "bs_shaderlib10.bin")
        obj, exe = OUT / "r10_shaderlib_test.obj", OUT / "r10_shaderlib_test.exe"
        for command in ([str(CLANG), "/nologo", "/Od", "/W3", "/std:c11", "/clang:--target=x86_64-pc-windows-msvc",
                         "-Wno-unused-function", "/I" + str(ROOT / "android/render10"), "/c", str(source), "/Fo" + str(obj)],
                        [str(linker), "/nologo", str(obj), "/OUT:" + str(exe)],
                        [str(exe)]):
            result = subprocess.run(command, capture_output=True, text=True, env=env, timeout=120)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("PASS r10_shaderlib", result.stdout)
        # The missing variants' requests (the stale stamp and the other lights): Python reproduces each record key
        # from what the device wrote, and the recorded variant's defines exactly.
        written = {}
        for path in sorted(requests.glob("*.req")):
            version, fields, pairs, defines, _ = lib.parse_request(path.read_bytes())
            self.assertEqual(version, 2)
            self.assertEqual(path.stem, fields["record"])
            self.assertEqual(fields["record"], "%016x" % lib.record_key(fields["name"], fields["profile"], defines))
            self.assertEqual(pairs, [("MATERIALFACTORY", "MaterialFactory_Fluid.hlsl"), ("VERTEXFACTORY", "VertexFactory_NULL.hlsl")])
            self.assertEqual((fields["pass"], fields["entry"], fields["guid"]), ("Base.hlsl", "PSMain", "00000011000000220000003300000045"))
            written[fields["time"]] = defines
        lights = [("MAX_INCIDENT_LIGHTS", "2")] + DEFINES[1:]
        self.assertEqual(written, {str(TIME + 1): DEFINES, str(TIME): lights}, result.stdout)
        self.assertEqual(result.stdout.count("COMPILE_MISS native"), 2)


if __name__ == "__main__":
    unittest.main()

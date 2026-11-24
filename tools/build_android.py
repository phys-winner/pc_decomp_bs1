"""Compile statically lifted BioShock code and the Win32 host for ARM64."""
import argparse
from concurrent.futures import ThreadPoolExecutor, as_completed
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
SDK = Path(os.environ.get("ANDROID_SDK_ROOT", Path.home() / "AppData/Local/Android/Sdk"))
LAUNCHER = next((path.resolve() for path in (Path(os.environ["PORT_LAUNCHER"]) if os.environ.get("PORT_LAUNCHER") else None,
                                              ROOT.parent / "launcher", ROOT / "android/launcher")
                 if path is not None and (path / "tools/launcher.py").is_file()), None)
LAUNCHER_HEADERS = ("panvk_identity.h", "replacements/crash_marker.h", "replacements/crc32_ieee.h",
                    "replacements/vulkan_driver.h")
# BioShock renders with its Direct3D 10 renderer on android/render10; the Direct3D 9 renderers (the port's
# GLES layer, the DXVK adapter, MojoShader) are in work/removed-d3d9-dxvk-20261119.


def sha(path):
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()

def launcher_native_inputs():
    return {name: sha(LAUNCHER / "native" / name) for name in LAUNCHER_HEADERS}


def x87_sources(generated, lifted, folder):
    """Generated sources with their x87 stack in C locals (tools/x87_locals.py).

    The plan is recomputed whenever a generated source or the tool changes;
    each rewritten source is compiled from `folder` (the original file stays
    untouched). Returns {original: rewritten copy} plus the plan's receipt."""
    sys.path.insert(0, str(ROOT / "tools"))
    import x87_locals
    inputs = hashlib.sha256((ROOT / "tools/x87_locals.py").read_bytes())
    for source in lifted:
        inputs.update(source.relative_to(ROOT).as_posix().encode())
        inputs.update(hashlib.sha256(source.read_bytes()).digest())
    inputs = inputs.hexdigest()
    stamp = x87_locals.OUT_DIR / "x87.stamp"
    if not x87_locals.X87_JSON.is_file() or not stamp.is_file() or stamp.read_text().strip() != inputs:
        x87_locals.analyse()
        stamp.write_text(inputs + "\n")
    plan = x87_locals.Plan()
    mapping = {}
    for source in lifted:
        if source == generated / "dispatch.c":
            continue
        text = source.read_bytes()
        rewritten = x87_locals.apply_x87(text, plan)
        if rewritten == text:
            continue
        target = folder / source.relative_to(generated)
        target.parent.mkdir(parents=True, exist_ok=True)
        if not target.is_file() or target.read_bytes() != rewritten:
            target.write_bytes(rewritten)
        mapping[source] = target
    return mapping, {"plan_sha256": sha(x87_locals.X87_JSON), "inputs_sha256": inputs,
                     "tool_sha256": sha(ROOT / "tools/x87_locals.py"), "sources": len(mapping),
                     "bodies": dict(sorted(plan.stats.items()))}


VK_SHADERS = ROOT / "build/render10/r10_vk_shaders.h"
SHADER_TOOLS = SDK / "ndk/27.2.12479018/shader-tools/windows-x86_64"


FSR1_HEADER = ROOT / "reference/fsr1/ffx-fsr/ffx_fsr1.h"

# The helpers of AMD's ffx_a.h that the float EASU/RCAS kernels of ffx_fsr1.h use.
FSR1_PREAMBLE = r"""#define AP1 bool
#define AF1 float
#define AF2 vec2
#define AF3 vec3
#define AF4 vec4
#define AU1 uint
#define AU2 uvec2
#define AU4 uvec4
#define ASU2 ivec2
#define AF1_(x) float(x)
#define AF2_(x) vec2(x)
#define AF3_(x) vec3(x)
#define AF4_(x) vec4(x)
#define AF1_AU1(x) uintBitsToFloat(AU1(x))
#define AF2_AU2(x) uintBitsToFloat(AU2(x))
float ASatF1(float x) { return clamp(x, 0.0, 1.0); }
float ARcpF1(float x) { return 1.0 / x; }
float AMax3F1(float x, float y, float z) { return max(x, max(y, z)); }
vec3 AMax3F3(vec3 x, vec3 y, vec3 z) { return max(x, max(y, z)); }
float AMin3F1(float x, float y, float z) { return min(x, min(y, z)); }
vec3 AMin3F3(vec3 x, vec3 y, vec3 z) { return min(x, min(y, z)); }
float APrxLoRcpF1(float a) { return uintBitsToFloat(uint(0x7ef07ebb) - floatBitsToUint(a)); }
float APrxLoRsqF1(float a) { return uintBitsToFloat(uint(0x5f347d74) - (floatBitsToUint(a) >> uint(1))); }
float APrxMedRcpF1(float a) { float b = uintBitsToFloat(uint(0x7ef19fff) - floatBitsToUint(a)); return b * (-b * a + 2.0); }
"""


# The half helpers of ffx_a.h for FSR_EASU_H (explicit 16-bit types: Vulkan shaderFloat16 + shaderInt16).
FSR1_HALF_PREAMBLE = r"""#extension GL_EXT_shader_explicit_arithmetic_types_float16 : require
#extension GL_EXT_shader_explicit_arithmetic_types_int16 : require
#define AH1 float16_t
#define AH2 f16vec2
#define AH3 f16vec3
#define AH4 f16vec4
#define AH1_(x) float16_t(x)
#define AH2_(x) f16vec2(x)
#define AH3_(x) f16vec3(x)
#define AH4_(x) f16vec4(x)
float16_t ARcpH1(float16_t x) { return float16_t(1.0) / x; }
f16vec2 ARcpH2(f16vec2 x) { return f16vec2(1.0) / x; }
f16vec2 ASatH2(f16vec2 x) { return clamp(x, f16vec2(0.0), f16vec2(1.0)); }
float16_t APrxLoRcpH1(float16_t a) { return uint16BitsToFloat16(uint16_t(0x7784) - float16BitsToUint16(a)); }
float16_t APrxLoRsqH1(float16_t a) { return uint16BitsToFloat16(uint16_t(0x59a3) - (float16BitsToUint16(a) >> uint16_t(1))); }
"""


def fsr1_easu_half():
    """FSR_EASU_H of ffx_fsr1.h (positions in fp32, the filter in fp16) for the Vulkan backend."""
    source = FSR1_HEADER.read_text(encoding="utf-8")
    start = source.index("#if defined(A_GPU)&&defined(A_HALF)&&defined(FSR_EASU_H)")
    block = source[start:]
    block = block[len("#if defined(A_GPU)&&defined(A_HALF)&&defined(FSR_EASU_H)"):block.index("\n#endif", block.index("void FsrEasuH("))]
    head = ("#version 450\n" + FSR1_HALF_PREAMBLE.split("\n", 2)[0] + "\n" + FSR1_HALF_PREAMBLE.split("\n", 2)[1] + "\n"
            "layout(push_constant) uniform Fsr { uvec4 con0, con1, con2, con3; } pc;\n"
            "layout(set = 0, binding = 0) uniform sampler2D src;\nlayout(location = 0) out vec4 color;\n")
    return (head + FSR1_PREAMBLE + FSR1_HALF_PREAMBLE.split("\n", 2)[2] +
            "AH4 FsrEasuRH(AF2 p) { return AH4(textureGather(src, p, 0)); }\n"
            "AH4 FsrEasuGH(AF2 p) { return AH4(textureGather(src, p, 1)); }\n"
            "AH4 FsrEasuBH(AF2 p) { return AH4(textureGather(src, p, 2)); }\n" + block +
            "\nvoid main() {\n    AH3 c;\n    FsrEasuH(c, AU2(gl_FragCoord.xy), pc.con0, pc.con1, pc.con2, pc.con3);\n"
            "    color = vec4(vec3(c), 1.0);\n}\n")


# Measured slower than textureGather on Adreno 618 (16.2 vs 10.0 ms at 2400x1080), kept for other GPUs.
# EASU's input callbacks as four texel fetches per 2x2 footprint, shared by the R, G and B callbacks of the
# same position (FsrEasuF asks R, G and B of each footprint in a row): 16 reads per pixel instead of 12 gathers
# of 4.  The footprint is clamped to the scene's region (its last texel in con3.zw), so the upscale never reads
# the back buffer beyond the scene.
EASU_FETCH = r"""ivec2 easu_base = ivec2(-65536);
vec4 easu_t00, easu_t10, easu_t01, easu_t11;
void easu_fetch(vec2 p) {
    ivec2 last = ivec2(CON3.zw);
    ivec2 b = ivec2(floor(p * vec2(textureSize(src, 0)) - 0.5));
    if (b == easu_base) return;
    easu_base = b;
    ivec2 b0 = clamp(b, ivec2(0), last), b1 = clamp(b + ivec2(1), ivec2(0), last);
    easu_t00 = texelFetch(src, b0, 0);
    easu_t10 = texelFetch(src, ivec2(b1.x, b0.y), 0);
    easu_t01 = texelFetch(src, ivec2(b0.x, b1.y), 0);
    easu_t11 = texelFetch(src, b1, 0);
}
/* textureGather order: (i0,j1), (i1,j1), (i1,j0), (i0,j0) */
AF4 FsrEasuRF(AF2 p) { easu_fetch(p); return AF4(easu_t01.r, easu_t11.r, easu_t10.r, easu_t00.r); }
AF4 FsrEasuGF(AF2 p) { easu_fetch(p); return AF4(easu_t01.g, easu_t11.g, easu_t10.g, easu_t00.g); }
AF4 FsrEasuBF(AF2 p) { easu_fetch(p); return AF4(easu_t01.b, easu_t11.b, easu_t10.b, easu_t00.b); }
"""


def fsr1_shaders():
    """GLSL of AMD FSR1 (MIT, reference/fsr1) for the native renderer: EASU and RCAS fragment shaders as
    (vulkan_easu, vulkan_rcas, es_easu, es_rcas).  The source image is binding 0; the constants of
    FsrEasuCon/FsrRcasCon are push constants on Vulkan and uniforms on OpenGL ES."""
    source = FSR1_HEADER.read_text(encoding="utf-8")
    easu = source.split("#if defined(A_GPU)&&defined(FSR_EASU_F)")[1].split("#endif")[0]
    rcas = source.split("#if defined(A_GPU)&&defined(FSR_RCAS_F)")[1].split("/" * 128)[0].rsplit("#endif", 1)[0]
    heads = {
        "vk": ("#version 450\nlayout(push_constant) uniform Fsr { uvec4 con0, con1, con2, con3; } pc;\n"
               "layout(set = 0, binding = 0) uniform sampler2D src;\nlayout(location = 0) out vec4 color;\n"),
        "es": ("#version 320 es\nprecision highp float;\nprecision highp int;\nprecision highp sampler2D;\n"
               "uniform uvec4 con0, con1, con2, con3;\nuniform sampler2D src;\nlayout(location = 0) out vec4 color;\n"),
    }
    cons = {"vk": "pc.con0, pc.con1, pc.con2, pc.con3", "es": "con0, con1, con2, con3"}
    out = {}
    for api, head in heads.items():
        out[(api, "easu")] = (head + FSR1_PREAMBLE +
            "AF4 FsrEasuRF(AF2 p) { return textureGather(src, p, 0); }\n"
            "AF4 FsrEasuGF(AF2 p) { return textureGather(src, p, 1); }\n"
            "AF4 FsrEasuBF(AF2 p) { return textureGather(src, p, 2); }\n" + easu +
            "\nvoid main() {\n    AF3 c;\n    FsrEasuF(c, AU2(gl_FragCoord.xy), %s);\n    color = AF4(c, 1.0);\n}\n" % cons[api])
        out[(api, "rcas")] = (head + FSR1_PREAMBLE + "#define FSR_RCAS_LIMIT (0.25-(1.0/16.0))\n" +
            "AF4 FsrRcasLoadF(ASU2 p) { return texelFetch(src, p, 0); }\n"
            "void FsrRcasInputF(inout AF1 r, inout AF1 g, inout AF1 b) {}\n" + rcas +
            "\nvoid main() {\n    AF1 r, g, b;\n    FsrRcasF(r, g, b, AU2(gl_FragCoord.xy), %s);\n    color = AF4(r, g, b, 1.0);\n}\n"
            % ("pc.con0" if api == "vk" else "con0"))
    return out[("vk", "easu")], out[("vk", "rcas")], out[("es", "easu")], out[("es", "rcas")]


def c_string(text):
    """A C string literal of `text`, one source line per line."""
    return "\n".join('"%s\\n"' % line.replace("\\", "\\\\").replace('"', '\\"') for line in text.split("\n"))


def vulkan_port_shaders():
    """SPIR-V of android/render10/shaders (glslc of the NDK) as C arrays for r10_vulkan.c, and the
    FSR1 passes of both native backends (SPIR-V for Vulkan, GLSL ES text for r10_gles.c)."""
    sources = [("present.vert", "r10_present_vert"), ("present.frag", "r10_present_frag"), ("debug.frag", "r10_debug_frag"),
               ("fsr.vert", "r10_fsr_vert")]
    folder = ROOT / "android/render10/shaders"
    digest = hashlib.sha256()
    for name, _ in sources:
        digest.update((folder / name).read_bytes())
    digest.update(FSR1_HEADER.read_bytes())
    digest.update(FSR1_PREAMBLE.encode())
    digest.update(FSR1_HALF_PREAMBLE.encode())
    digest.update(EASU_FETCH.encode())
    digest.update(b"vulkan1.0 with optional vulkan1.1 FP16")
    stamp = "/* inputs " + digest.hexdigest() + " */"
    if VK_SHADERS.is_file() and VK_SHADERS.read_text().startswith(stamp):
        return VK_SHADERS
    rows = [stamp, "/* Generated by tools/build_android.py from android/render10/shaders and AMD FSR1 (MIT, "
            "reference/fsr1); glslc of the NDK. */", "#pragma once", "#include <stdint.h>"]
    vk_easu, vk_rcas, es_easu, es_rcas = fsr1_shaders()
    work = ROOT / "build/render10/fsr1"
    work.mkdir(parents=True, exist_ok=True)
    (work / "fsr_easu.frag").write_text(vk_easu, encoding="utf-8")
    (work / "fsr_rcas.frag").write_text(vk_rcas, encoding="utf-8")
    (work / "fsr_easu_half.frag").write_text(fsr1_easu_half(), encoding="utf-8")
    jobs = [(folder / name, symbol) for name, symbol in sources] + [(work / "fsr_easu.frag", "r10_fsr_easu_frag"),
                                                                    (work / "fsr_rcas.frag", "r10_fsr_rcas_frag"),
                                                                    (work / "fsr_easu_half.frag", "r10_fsr_easu_half_frag")]
    for path, symbol in jobs:
        target = "1.1" if symbol == "r10_fsr_easu_half_frag" else "1.0"
        done = subprocess.run([str(SHADER_TOOLS / "glslc.exe"), "--target-env=vulkan" + target, "-O", "-mfmt=num",
                               str(path), "-o", "-"], capture_output=True, text=True)
        if done.returncode:
            raise RuntimeError("glslc %s:\n%s" % (path.name, done.stderr[-4000:]))
        rows.append("static const uint32_t %s[] = {%s};" % (symbol, done.stdout.strip().rstrip(",")))
    rows.append("static const char r10_fsr_easu_es[] =\n%s;" % c_string(es_easu))
    rows.append("static const char r10_fsr_rcas_es[] =\n%s;" % c_string(es_rcas))
    VK_SHADERS.parent.mkdir(parents=True, exist_ok=True)
    VK_SHADERS.write_text("\n".join(rows) + "\n")
    return VK_SHADERS


def native_dependencies(prepare=False):
    """The native libraries beside libbioshock.so: OpenAL Soft (audio), adrenotools with Mesa Turnip
    and Mesa PanVK (the custom Vulkan drivers of the native renderer).  No DXVK."""
    if prepare:
        for tool in ("build_native_audio.py", "build_turnip.py", "build_panvk_runtime.py"):
            subprocess.run([sys.executable, str(ROOT / "tools" / tool)], cwd=ROOT, check=True)
    pinned, runtime = {}, {}

    def pin(path, expected):
        if not path.is_file() or sha(path) != expected:
            raise ValueError(f"Native dependency hash mismatch or missing file: {path}")
        pinned[path] = expected

    def library(path, expected):
        pin(path, expected)
        data = path.read_bytes()[:20]
        if data[:6] != b"\x7fELF\x02\x01" or int.from_bytes(data[18:20], "little") != 183:
            raise ValueError(f"Native runtime must be little-endian AArch64 ELF64: {path}")
        if path.name in runtime and sha(runtime[path.name]) != expected:
            raise ValueError(f"Conflicting native runtime libraries: {path.name}")
        runtime[path.name] = path

    audio_manifest = ROOT / "dependencies-audio.json"
    audio = json.loads((ROOT / "build/native-audio/build-receipt.json").read_text())
    pin(audio_manifest, audio["manifest_sha256"])
    pin(ROOT / "tools/build_native_audio.py", audio["tool_sha256"])
    for name, expected in audio["runtime"].items():
        library(ROOT / "build/native-audio/runtime" / name, expected)
    licenses = []
    for name, expected in audio["licenses"].items():
        path = ROOT / "build/native-audio/licenses" / name
        pin(path, expected)
        licenses.append(path)
    turnip = json.loads((ROOT / "build/turnip-runtime/build-receipt.json").read_text())
    pin(ROOT / "tools/build_turnip.py", turnip["build_tool_sha256"])
    pin(ROOT / "dependencies-turnip.json", turnip["input_pins_sha256"])
    for item in turnip["outputs"]:
        library(ROOT / item["path"], item["sha256"])
    for item in turnip["licenses"]:
        path = ROOT / item["path"]
        pin(path, item["sha256"])
        licenses.append(path)
    panvk = json.loads((ROOT / "build/panvk-runtime/build-receipt.json").read_text())
    pin(ROOT / "dependencies-panvk.json", panvk["manifest_sha256"])
    pin(ROOT / "tools/build_panvk_runtime.py", panvk["tool_sha256"])
    for item in panvk["outputs"]:
        path = ROOT / item["path"]
        if path.suffix == ".so":
            library(path, item["sha256"])
        else:
            pin(path, item["sha256"])
            licenses.append(path)
    if set(runtime) != {"libopenal.so", "libc++_shared.so", "libadrenotools.so", "libhook_impl.so", "libmain_hook.so",
                        "libfile_redirect_hook.so", "libgsl_alloc_hook.so", "libut3_turnip.so", "libvulkan_panfrost.so"}:
        raise ValueError("Native bundle lacks the audio runtime or a custom Vulkan driver: %s" % sorted(runtime))
    # Build-time translators of the shader library (the APK holds their output, not their code).
    for path in (ROOT / "reference/dxvk/LICENSE", ROOT / "reference/spirv-cross/LICENSE"):
        pinned[path] = sha(path)
    licenses.append(ROOT / "reference/fsr1/license.txt")
    shaders = vulkan_port_shaders()
    pinned[shaders] = sha(shaders)
    return runtime, pinned, licenses


EXCLUDED_MODULES = {"d3dx9_33.dll"}   # the Direct3D 10 path never runs D3DX9; its imports bind to host shims


def filtered_generation(generated, out):
    """modules.inc and dispatch.c without EXCLUDED_MODULES (their lifted code is not compiled)."""
    out.mkdir(parents=True, exist_ok=True)
    specs = {row["file"]: row for row in json.loads((ROOT / "config/modules.json").read_text())["modules"]}
    excluded = [specs[name] for name in EXCLUDED_MODULES]
    lines = [line for line in (generated / "modules.inc").read_text().splitlines()
             if not any(line.strip().startswith('{"%s"' % spec["file"]) for spec in excluded)]
    modules = "\n".join(lines) + "\n"
    ranges = []
    for spec in excluded:
        base = int(spec["base"], 16)
        data = (generated / "input" / spec["file"]).read_bytes()
        lfanew = int.from_bytes(data[0x3C:0x40], "little")
        size = int.from_bytes(data[lfanew + 0x50:lfanew + 0x54], "little")
        ranges.append((base, base + size, spec["name"]))
    rows, count = [], 0
    for line in (generated / "dispatch.c").read_text().splitlines():
        s = line.strip()
        if s.startswith("#include") and any('"%s/' % name in s for _, _, name in ranges):
            continue
        if s.startswith("{0x") and s.endswith("},"):
            va = int(s[1:s.index("u,")], 16)
            if any(lo <= va < hi for lo, hi, _ in ranges):
                continue
            count += 1
        if s.startswith("const uint32_t recomp_dispatch_count"):
            line = "const uint32_t recomp_dispatch_count = %d;" % count
        rows.append(line)
    dispatch = "\n".join(rows) + "\n"
    for name, text in (("modules.inc", modules), ("dispatch.c", dispatch)):
        target = out / name
        if not target.is_file() or target.read_text() != text:
            target.write_text(text)
    return out / "modules.inc", out / "dispatch.c", count


# Call-site seams of the lifted Direct3D 10 renderer, applied to a copy of the generated source (as the
# x87 rewrite does): (address, original emission, replacement).
SEAMS = [
    # The original stream registration names the .blk and the exact byte range.
    (0x10B64EBB, "RECOMP_CALL(sub_10EC9E50); /* 0x10B64EBB: call 0x10ec9e50 */",
     "{ extern void void_bioshock_prefetch_bulk(uint32_t, uint32_t, uint32_t); "
     "void_bioshock_prefetch_bulk(MEM32(esp), MEM32(esp + 20), MEM32(esp + 24)); } "
     "RECOMP_CALL(sub_10EC9E50); /* 0x10B64EBB: call 0x10ec9e50 */"),
    (0x10B64F10, "RECOMP_CALL(sub_10953100); /* 0x10B64F10: call 0x10953100 */",
     "{ extern void void_bioshock_prefetch_bulk(uint32_t, uint32_t, uint32_t); "
     "void_bioshock_prefetch_bulk(MEM32(esp), MEM32(esp + 20), MEM32(esp + 24)); } "
     "RECOMP_CALL(sub_10953100); /* 0x10B64F10: call 0x10953100 */"),
    # The shadow map is R32_FLOAT color. Vulkan and GLES cannot use the NVIDIA D3D10 depth comparison path.
    (0x10983F04, "if (CMP_NE(_flag_a, _flag_b)) goto L_10983F16; /* 0x10983F04: jne 0x10983f16 */",
     "goto L_10983F16; /* 0x10983F04: use the original shader's float-texture shadow comparison */"),
    # The software shadow filter compares and interpolates individual texels itself.
    (0x109A120A, "MEM32(ebp + (-0x68)) = 0x94u; /* 0x109A120A: mov dword ptr [ebp - 0x68], 0x94 */",
     "MEM32(ebp + (-0x68)) = 0; /* 0x109A120A: D3D10_FILTER_MIN_MAG_MIP_POINT */"),
    # Scene-depth bindings set point min/mag but retain the previous sampler's mip filter.
    (0x10981711, "MEM32(ebx + esi * 4 + 0x3C8) = ecx; /* 0x10981711: mov dword ptr [ebx + esi*4 + 0x3c8], ecx */",
     "{ extern int void_r10_single_level_depth_srv(uint32_t); "
     "if ((ecx & 0x6000u) && !(MEM8(ebp + 0x10) & 3) && void_r10_single_level_depth_srv(MEM32(ebp + 0xC))) "
     "ecx &= ~0x6000u; } MEM32(ebx + esi * 4 + 0x3C8) = ecx; "
     "/* 0x10981711: raw scene depth requires point filtering at every level */"),
    # Keep authored particle fades. The globally forced backfade erases part of the cast effects.
    (0x10AAC3FA, "if (CMP_EQ(_flag_a, _flag_b)) goto L_10AAC4D4; /* 0x10AAC3FA: je 0x10aac4d4 */",
     "goto L_10AAC4D4; /* 0x10AAC3FA: use the emitter's authored fade */"),
    (0x109B7B69, "RECOMP_CALL(sub_109A66D0); /* 0x109B7B69: call 0x109a66d0 */",
     "{ extern void void_bioshock_resolve_quad10(void); RECOMP_CALL(void_bioshock_resolve_quad10); } "
     "/* 0x109B7B69: call 0x109a66d0; scene scale before HUD (bioshock_render10.c) */"),
    # FMOD Ex's DirectSound output description (FMOD_OUTPUTTYPE_DSOUND) describes the native AAudio
    # output plugin instead (bioshock_fmod_output.c); BIOSHOCK_FMOD_OUTPUT=dsound runs the original body.
    (0x10EAC923, "PUSH32(esp, esi); /* 0x10EAC923: push esi */",
     "{ extern int void_bioshock_fmod_output_description(void); RECOMP_REGS_OUT(); "
     "if (void_bioshock_fmod_output_description()) { RECOMP_REGS_IN(); { RECOMP_REGS_OUT(); return; } } } "
     "PUSH32(esp, esi); /* 0x10EAC923: push esi; FMOD output description, native AAudio plugin */"),
    # Bink's BinkOpenDirectSound (binkw32) hands Bink the native AAudio sound driver
    # (bioshock_bink_audio.c); BIOSHOCK_BINK_OUTPUT=dsound runs the original body.
    (0x1800F120, "eax = MEM32(0x18032A94); /* 0x1800F120: mov eax, dword ptr [0x18032a94] */",
     "{ extern int void_bioshock_bink_open_sound(void); RECOMP_REGS_OUT(); "
     "if (void_bioshock_bink_open_sound()) { RECOMP_REGS_IN(); { RECOMP_REGS_OUT(); return; } } } "
     "eax = MEM32(0x18032A94); /* 0x1800F120: mov eax, dword ptr [0x18032a94]; Bink sound driver, native AAudio */"),
    # The compact movie pack appends Russian ID 5. BinkOpen has read the actual track IDs here.
    (0x1800B132, "edi = MEM32(0x1802F068); /* 0x1800B132: mov edi, dword ptr [0x1802f068] */",
     "{ extern void void_bioshock_bink_select_track(uint32_t); void_bioshock_bink_select_track(ebp); } "
     "edi = MEM32(0x1802F068); /* 0x1800B132: read BinkOpen's track selection */"),
    (0x10BAC94D, "PUSH32(esp, edx); /* 0x10BAC94D: push edx */",
     "{ extern uint32_t void_bioshock_bink_volume_track(uint32_t, uint32_t); "
     "PUSH32(esp, void_bioshock_bink_volume_track(ecx, edx)); } "
     "/* 0x10BAC94D: apply the movie volume to the selected track */"),
    # D3DDrv10's FShaderCache on the native renderer's shader library (android/render10/r10_engine.c): the shader
    # getter's (sub_109AF8E0) call of Find is answered with tokens of library programs (SPIR-V and GLSL ES made on the
    # PC), Load does not read ShaderCache.pcs10; BIOSHOCK_R10_SHADERS=dxbc runs the original bodies.
    (0x109AFABE, "RECOMP_CALL(sub_10AF7D40); /* 0x109AFABE: call 0x10af7d40 */",
     "{ extern void void_bioshock_shader_cache_find10(void); RECOMP_CALL(void_bioshock_shader_cache_find10); } "
     "/* 0x109AFABE: call 0x10af7d40; FShaderCache::Find of the shader getter, native shader library */"),
    (0x10AF77F0, "PUSH32(esp, 0xFFFFFFFFu); /* 0x10AF77F0: push -1 */",
     "{ extern int void_bioshock_shader_cache_load(void); RECOMP_REGS_OUT(); "
     "if (void_bioshock_shader_cache_load()) { RECOMP_REGS_IN(); { RECOMP_REGS_OUT(); return; } } } "
     "PUSH32(esp, 0xFFFFFFFFu); /* 0x10AF77F0: push -1; FShaderCache::Load, native shader library */"),
    # UInventoryManager::CanReRoll refuses a Scrounger re-roll of a container that still holds an item
    # (bioshock_tweaks.c, port setting fixScrounger): the re-roll would discard it, Langford's safe included.
    (0x10CB85C0, "PUSH32(esp, 0xFFFFFFFFu); /* 0x10CB85C0: push -1 */",
     "{ extern int void_bioshock_reroll_blocked(void); RECOMP_REGS_OUT(); "
     "if (void_bioshock_reroll_blocked()) { RECOMP_REGS_IN(); { RECOMP_REGS_OUT(); return; } } } "
     "PUSH32(esp, 0xFFFFFFFFu); /* 0x10CB85C0: push -1; Scrounger re-roll guard */"),
    # UObject::ProcessEvent reads the clock around each outermost script event only for GScriptCycles, which
    # nothing reads (android/host/bioshock_script_clock.c): appCycles() answers 0 and QueryPerformanceCounter's
    # LARGE_INTEGER is zeroed (stdcall: its argument is popped, EAX = TRUE).  BIOSHOCK_SCRIPT_CYCLES=1 calls them.
    # The generator also inlined the body into sub_109EF150 (the probe-masked event call that jumps into it), the
    # copy the game thread runs most: both copies get the seams.
    (0x1096792E, "RECOMP_CALL(sub_1093CD60); /* 0x1096792E: call 0x1093cd60 */",
     "{ extern int void_bioshock_script_clock(void); if (void_bioshock_script_clock()) RECOMP_CALL(sub_1093CD60); "
     "else { RECOMP_FLAGS_OUT(); eax = 0; } } /* 0x1096792E: call 0x1093cd60; GScriptCycles only, no clock */", 2),
    (0x10967A04, "RECOMP_ICALL(MEM32(0x10FCC28C)); /* 0x10967A04: call dword ptr [0x10fcc28c] */",
     "{ extern int void_bioshock_script_clock(void); if (void_bioshock_script_clock()) RECOMP_ICALL(MEM32(0x10FCC28C)); "
     "else { RECOMP_FLAGS_OUT(); MEM32(MEM32(esp)) = 0; MEM32(MEM32(esp) + 4u) = 0; esp += 4; eax = 1; } } "
     "/* 0x10967A04: call dword ptr [0x10fcc28c] (QueryPerformanceCounter); GScriptCycles only, no clock */", 2),
]


def generated_sources(folder):
    if folder == ROOT / "build/host-smoke":
        return [folder / "dispatch.c"], {}
    specs = json.loads((ROOT / "config/modules.json").read_text())["modules"]
    included = [name for name in re.findall(r'^\s*\{\s*"([^"]+)"', (folder / "modules.inc").read_text(), re.M)
                if name not in EXCLUDED_MODULES]
    by_file = {row["file"]: row for row in specs}
    if not included or len(set(included)) != len(included) or set(included) - by_file.keys():
        raise ValueError("Generated module list is empty, duplicated or unknown")
    if not any(by_file[name].get("exe") for name in included):
        raise ValueError("Generated module list has no executable")
    sources, pinned = [folder / "dispatch.c"], {}
    for name in included:
        spec = by_file[name]
        directory = folder / spec["name"]
        report_path = directory / "report.json"
        report = json.loads(report_path.read_text())
        if report.get("status") != "GENERATED":
            raise ValueError(f"Static generation is incomplete: {name}")
        image = folder / "input" / spec["file"]
        if sha(image) != report.get("image_sha256") or report.get("original_sha256") != spec["sha256"]:
            raise ValueError(f"Generated input hash differs from its report: {name}")
        pinned[image] = sha(image)
        pinned[report_path] = sha(report_path)
        for relative in report["sources"]:
            source = (directory / relative).resolve()
            if source.parent != directory or source.suffix != ".c" or not source.is_file():
                raise ValueError(f"Invalid generated source: {relative}")
            sources.append(source)
    pinned[folder / "modules.inc"] = sha(folder / "modules.inc")
    pinned[folder / "dispatch.c"] = sha(folder / "dispatch.c")
    return sources, pinned


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--generated", type=Path, default=ROOT / "generated")
    parser.add_argument("--output", type=Path, default=ROOT / "build/android/bioshock-arm64")
    parser.add_argument("--jobs", type=int, default=min(8, os.cpu_count() or 1))
    parser.add_argument("--shared", action="store_true", help="Build the APK's libbioshock.so")
    parser.add_argument("--diagnostics", action="store_true", help="Record guest function-entry rings for crash investigations")
    parser.add_argument("--x87-locals", action="store_true",
                        help="Keep the x87 stack of every body with a known depth in C locals (tools/x87_locals.py)")
    parser.add_argument("--x87-check", action="store_true",
                        help="With --x87-locals: report every rewritten body whose x87 window effect differs (RECOMP_PROTOCOL_CHECK)")
    args = parser.parse_args()
    if args.jobs < 1:
        parser.error("--jobs must be positive")
    if LAUNCHER is None:
        parser.error("The launcher base is missing: set PORT_LAUNCHER or use the sibling launcher folder")
    launcher_headers = launcher_native_inputs()
    runtime, native_pinned, _ = native_dependencies(prepare=True)
    ndk = Path(os.environ.get("ANDROID_NDK_HOME", SDK / "ndk/27.2.12479018"))
    clang = ndk / "toolchains/llvm/prebuilt/windows-x86_64/bin/clang.exe"
    if not clang.is_file():
        parser.error(f"NDK compiler not found: {clang}")
    generated = args.generated.resolve()
    if not (generated / "modules.inc").is_file():
        parser.error(f"missing module list {generated / 'modules.inc'}")
    lifted, pinned = generated_sources(generated)
    pinned.update(native_pinned)
    out = args.output.resolve()
    if args.shared and out == ROOT / "build/android/bioshock-arm64":
        out = ROOT / "build/android/libbioshock.so"
    out.parent.mkdir(parents=True, exist_ok=True)
    filtered = out.parent / "gen"
    filtered_modules, filtered_dispatch, dispatch_count = filtered_generation(generated, filtered)
    lifted = [filtered_dispatch if source == generated / "dispatch.c" else source for source in lifted]
    compiler = subprocess.check_output([str(clang), "--version"], text=True)
    # RECOMP_PROFILE_HOT is diagnostic: it adds an out-of-line hot_profile.c call
    # to every lifted entry, call site and return (perf_optimization_295.md), so
    # only --diagnostics builds carry it.
    common = ["--target=aarch64-linux-android26", "-O2", "-fno-strict-aliasing", "-fwrapv",
              "-fvisibility=hidden", "-fPIC", "-ffp-contract=off", "-fno-math-errno", "-g0",
              "-DRECOMP_X87_WINDOW", "-DRECOMP_CALL_DIAG", "-DUT3_BIN=\"Builds/Release\"",
              "-Iandroid/runtime", "-Iandroid/host", "-I" + str(generated)]
    if args.diagnostics:
        common += ["-DRECOMP_TRACE", "-DRECOMP_PROFILE_HOT"]
    if args.shared:
        common.append("-DUT3_ANDROID_APP")
    # Lifted code includes only the runtime header and its own headers; the host also sees every header of
    # android/ (an edit of the renderer's headers does not rebuild the lifted code).
    def digest_of(paths):
        inputs = hashlib.sha256((str(clang) + compiler).encode())
        for path in paths:
            inputs.update(str(path).encode())
            inputs.update(path.read_bytes())
        return inputs.digest()
    generated_headers = sorted((ROOT / "android/runtime").glob("*.h")) + sorted(generated.rglob("*.h")) + [filtered_modules]
    generated_digest = digest_of(generated_headers)
    host_headers = sorted((ROOT / "android").rglob("*.h")) + [filtered_modules, ROOT / "reference/ut3/reference/dlmalloc/malloc.c", VK_SHADERS]
    host_headers.extend(LAUNCHER / "native" / name for name in launcher_headers)
    host_digest = digest_of(host_headers)
    tasks = []
    generated_flags = common + ["-w", "-DRECOMP_GENERATED_CODE", "-DRECOMP_GUEST_STATE_SEPARATE",
                                "-mllvm", "-basic-aa-separate-storage", "-include",
                                (ROOT / "android/runtime/guest_state_prefix.h").as_posix()]
    local_hot_flags = common + ["-w", "-DRECOMP_GENERATED_CODE", "-DRECOMP_LOCAL_REGS"]
    x87_map, x87_receipt = {}, None
    if args.x87_locals or args.x87_check:
        x87_map, x87_receipt = x87_sources(generated, lifted, out.parent / "x87src")
        x87_receipt["check"] = args.x87_check
        print(f"x87 locals: {x87_receipt['sources']} sources, {x87_receipt['bodies']}", flush=True)
    # The call-site seams of the renderer go into copies of their sources (after the x87 rewrite).
    seam_folder = out.parent / "seamsrc"
    for seam in SEAMS:
        # (address, before, after[, copies]): a body the generator also inlined into a caller (a tail jump
        # into it) is emitted once per copy, and every copy gets the seam.
        address, before, after = seam[:3]
        copies = seam[3] if len(seam) > 3 else 1
        marker = "/* 0x%08X:" % address
        hits = [s for s in lifted if s != filtered_dispatch and marker in s.read_text(errors="replace")]
        if len(hits) != copies:
            raise ValueError("seam %08X found in %d generated sources, expected %d" % (address, len(hits), copies))
        for source in hits:
            text = x87_map.get(source, source).read_text(errors="replace")
            if text.count(before) != 1:
                raise ValueError("seam %08X emission changed" % address)
            target = seam_folder / source.relative_to(generated)
            target.parent.mkdir(parents=True, exist_ok=True)
            patched = text.replace(before, after)
            if not target.is_file() or target.read_text(errors="replace") != patched:
                target.write_text(patched)
            x87_map[source] = target
    for source in lifted:
        flags = (common + ["-w", "-DRECOMP_GENERATED_CODE"] if source == filtered_dispatch
                 else local_hot_flags if source == generated / "bioshock/functions_0091.c" else generated_flags)
        if source == filtered_dispatch:
            flags = flags + ["-I" + str(generated)]
        if args.diagnostics and source in {generated / "bioshock/functions_0008.c", generated / "bioshock/functions_0021.c"}:
            flags = flags + ["-DRECOMP_TRACE"]
        if source in x87_map:
            # The copy includes its module's headers by quoted name.
            flags = flags + ["-I" + str(source.parent)] + (["-DRECOMP_PROTOCOL_CHECK"] if args.x87_check else [])
            source = x87_map[source]
        tasks.append((source, flags, generated_digest))
    ut3_only_host = {"touch_game.c", "touch_menu.c", "ui_hud.c", "ui_camera.c", "ui_list_scale.c", "loading.c"}
    host_flags = ["-I" + str(filtered), "-I" + str(LAUNCHER / "native")] + common + [
        "-std=gnu11", "-Wall", "-Wno-unused-function", "-Wno-unused-variable"]
    for source in sorted((ROOT / "android/host").glob("*.c")):
        if source.name in ut3_only_host:
            continue
        flags = list(host_flags)
        if source.name == "openal.c":
            flags.append("-Ireference/openal-soft/source/include")
        tasks.append((source, flags, host_digest))
    render_flags = host_flags + ["-Iandroid/render10", "-I" + str(VK_SHADERS.parent)]
    for source in sorted((ROOT / "android/render10").glob("*.c")):
        tasks.append((source, render_flags, host_digest))

    def compile_one(task):
        source, flags, base_digest = task
        relative = source.relative_to(ROOT)
        obj = out.parent / "obj" / relative.with_suffix(".o")
        obj.parent.mkdir(parents=True, exist_ok=True)
        stamp = obj.with_suffix(".json")
        included = {
            "core.c": ["android/host/callsite_cache.inc"],
        }
        extra = b"".join((ROOT / name).read_bytes() for name in included.get(source.name, ()))
        if source.name == "openal.c":
            extra = b"".join(header.read_bytes() for header in
                             sorted((ROOT / "reference/openal-soft/source/include").rglob("*.h")))
        digest = hashlib.sha256(base_digest + source.read_bytes() + json.dumps(flags).encode() + extra).hexdigest()
        if stamp.exists() and obj.exists():
            cached = json.loads(stamp.read_text())
            if cached.get("input") == digest and cached.get("object") == sha(obj):
                return obj, True
        result = subprocess.run([str(clang), *flags, "-c", str(source), "-o", str(obj)],
                                cwd=ROOT, text=True, capture_output=True)
        if result.returncode:
            raise RuntimeError(f"compile failed: {relative}\n{result.stdout}{result.stderr}")
        if result.stderr:
            print(result.stderr.rstrip(), flush=True)
        stamp.write_text(json.dumps({"input": digest, "object": sha(obj)}))
        return obj, False

    objects, cached = [], 0
    with ThreadPoolExecutor(max_workers=args.jobs) as pool:
        futures = [pool.submit(compile_one, task) for task in tasks]
        for index, future in enumerate(as_completed(futures), 1):
            try:
                obj, reused = future.result()
            except BaseException:
                for pending in futures:
                    pending.cancel()
                assert all(pending.running() or pending.done() for pending in futures), "compile failure left queued tasks"
                raise
            objects.append(obj)
            cached += reused
            if index % 25 == 0 or index == len(tasks):
                print(f"compiled {index}/{len(tasks)} ({cached} cached)", flush=True)
    if any(sha(path) != digest for path, digest in pinned.items()):
        raise ValueError("Generated module report, dispatch or PE image changed during compilation")
    if launcher_headers != launcher_native_inputs():
        raise ValueError("Launcher headers changed during native compilation")
    response = out.parent / "link.rsp"
    response.write_text("\n".join('"' + p.as_posix() + '"' for p in sorted(objects)))
    # A GNU build ID lets simpleperf/perfetto and llvm-symbolizer match a profile or a crash log to the
    # exact unstripped copy (wiki note 13, PF5c); the stripped shipping copy keeps the same note.
    subprocess.run([str(clang), common[0], "-Wl,-z,max-page-size=16384", "-Wl,--build-id=sha1",
                    *(["-shared", "-Wl,-z,defs"] if args.shared else []),
                    "@" + str(response), "-o", str(out), "-lm", "-ldl", "-llog", "-landroid",
                    "-lEGL", "-lGLESv3", "-laaudio", "-lz"], cwd=ROOT, check=True)
    receipt = {"output": str(out), "sha256": sha(out), "compiler": compiler,
               "target": common[0].split("=", 1)[1], "cflags": common, "generated_cflags": generated_flags,
               "excluded_modules": sorted(EXCLUDED_MODULES), "dispatch_entries": dispatch_count,
               "renderer": "OpenGL ES 3.2 and Vulkan 1.1 or 1.0 with VK_KHR_maintenance1 (android/render10)",
               "sources": len(tasks), "cached": cached,
               "generated": str(generated), "shared": args.shared,
               "native_dependencies": {path.relative_to(ROOT).as_posix(): digest for path, digest in native_pinned.items()},
               "launcher_native_headers": launcher_headers,
               "x87_locals": x87_receipt,
               "whole_game": "NOT YET VERIFIED"}
    out.with_suffix(".json").write_text(json.dumps(receipt, indent=2) + "\n")
    print(json.dumps(receipt, indent=2))


if __name__ == "__main__":
    main()

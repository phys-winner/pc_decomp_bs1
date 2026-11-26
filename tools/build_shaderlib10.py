"""Build-time shader library of the native Direct3D 10 renderer (android/render10).

    python -X utf8 -B tools/build_shaderlib10.py build      # build/render10/bs_shaderlib10.bin
    python -X utf8 -B tools/build_shaderlib10.py validate   # spirv-val of every Vulkan module

BioShock's D3D10 path ships every material shader compiled (ShaderCache.pcs10, DXBC vs_4_0/ps_4_0) and
compiles a few utility shaders from HLSL strings inside Bioshock.exe at run time (the Flash UI and the
Bink movie shaders, D3DX10CompileFromMemory).  This tool:

1. takes the unique DXBC containers of ShaderCache.pcs10, compiles the embedded HLSL with Windows'
   own d3dcompiler_47.dll (the same entry points, profiles and flags as the game passes) and the
   material variants the patched game asks for (requests pulled from devices) with the game's d3dx10_33;
2. translates every container on the PC with tools/shaderlib10/bs_shaderc.cpp (DXVK 2.4.1's DXBC
   compiler, then SPIRV-Cross): GLSL ES 3.20 text for OpenGL ES and, through glslc of the NDK,
   SPIR-V for Vulkan 1.0;
3. packs one library (layout in android/render10/r10_shaderlib.h) that holds no Direct3D bytecode: per
   program the SPIR-V, the GLSL ES and the interface tables the engine reads through D3D10ReflectShader
   (RDEF, ISGN, OSGN), each its own zlib stream, and three indexes the device looks up by:
   - the engine's own shader cache identity, as Bioshock.exe's FShaderCache keys ShaderCache.pcs10
     (sub_109AF8E0: the pass, factory and entry name, the 128-bit permutation GUID and the time stamp of
     the shaders.spk files): every record of the shipped cache;
   - the record key of a variant the shipped cache lacks: the name, the profile and the defines the
     engine's field writers recorded for it (the same list FShaderCompiler10 would hand the compiler);
   - the key of a D3DX10CompileFromMemory request (the embedded HLSL; the old path for A/B runs).

On the device the engine's cache lookup answers from the library (a seam of tools/build_android.py), the
engine never reads ShaderCache.pcs10 and never compiles a material shader; the game's shader objects are
tokens of library programs.  A variant the library lacks is drawn with its family's program and recorded
in <userdata>/r10-requests (the name, the pass, the factory files and the defines: no text) for this tool.
"""
import argparse
import concurrent.futures
import ctypes
import hashlib
import json
import os
import re
import shutil
import struct
import subprocess
import sys
import tempfile
import zlib
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
sys.path.insert(0, str(ROOT / "tools/shaderlib10"))
from port_mobile_shaders import unpack_cache  # noqa: E402
import variants  # noqa: E402

GAME = Path(os.environ.get("BIOSHOCK_GAME", r"C:\Games\BioShock"))
PCS10 = GAME / "Builds/Release/ShaderCache.pcs10"
SPK = GAME / "Builds/Release/shaders.spk"
EXE = GAME / "Builds/Release/Bioshock.exe"
# 64-bit build of the D3DX10 the game ships (DirectX redistributable, System32).
GAME_D3DX10 = os.path.join(os.environ.get("SystemRoot", r"C:\Windows"), "System32", "d3dx10_33.dll")
PINS = {"ShaderCache.pcs10": None, "Bioshock.exe": "7f2ee821ad60eaf08322b4bc6ebbf839d7fe010b4d0255ec12b5be4a2966f7f6"}
WORK = ROOT / "build/render10/shaderlib"
OUTPUT = ROOT / "build/render10/bs_shaderlib10.bin"
MANIFEST = ROOT / "build/render10/bs_shaderlib10.json"
DXVK = ROOT / "reference/dxvk"
SPIRV_CROSS = ROOT / "reference/spirv-cross"
COMPAT = ROOT / "tools/shaderlib10/compat"
TREE = WORK / "tree"
SDK = Path(os.environ.get("ANDROID_SDK_ROOT", Path.home() / "AppData/Local/Android/Sdk"))
SHADER_TOOLS = SDK / "ndk/27.2.12479018/shader-tools/windows-x86_64"
MAGIC = b"BS10SHL3"
VERSION = 3
# Compile flags of FShaderCompiler10::CompileShader (Bioshock.exe 0x109B9610: D3D10_SHADER_OPTIMIZATION_LEVEL1).
MATERIAL_FLAGS = 4096

# HLSL the game compiles at run time: (marker inside the string, entry, profile, flags1).  The
# strings are read from Bioshock.exe, so the library always matches the pinned executable.
EMBEDDED = [
    (b"VS_OUTPUT VSMainBitmapFill(float4", "VSMainColorFill", "vs_4_0", 0),
    (b"VS_OUTPUT VSMainBitmapFill(float4", "PSMainColorFill", "ps_4_0", 0),
    (b"VS_OUTPUT VSMainBitmapFill(float4", "VSMainBitmapFill", "vs_4_0", 0),
    (b"VS_OUTPUT VSMainBitmapFill(float4", "PSMainBitmapFill", "ps_4_0", 0),
    (b"VS_OUTPUT VSMain(float4 vPos : POSITION, float2 vTex : TEXCOORD)", "VSMain", "vs_4_0", 0),
    (b"VS_OUTPUT VSMain(float4 vPos : POSITION, float2 vTex : TEXCOORD)", "PSMain", "ps_4_0", 0),
]


def sha(path):
    with Path(path).open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def fnv1a64(data, h=1469598103934665603):
    for b in data:
        h = ((h ^ b) * 1099511628211) & 0xFFFFFFFFFFFFFFFF
    return h


def request_key(source, entry, profile, flags, defines=()):
    """The runtime's key of a D3DX10CompileFromMemory request (r10_shaderlib.c request_key)."""
    h = fnv1a64(source)
    for part in (entry.encode("latin-1"), profile.encode("latin-1"), struct.pack("<I", flags)):
        h = fnv1a64(b"\0" + part, h)
    for name, value in defines:
        h = fnv1a64(b"\1" + name.encode("latin-1") + b"=" + value.encode("latin-1"), h)
    return h


# Defines of a material shader that follow the scene, not the material (sub_109B2C10 and the shadow passes:
# light count, per-vertex luminance, specular cubemap, composite layers, shadow options).
SCENE_DEFINES = {"MAX_INCIDENT_LIGHTS", "PER_VERTEX_LUMINANCE", "USES_SPECULAR_CUBEMAP", "USING_COMPOSITE_LAYERS",
                 "NVIDIA_DX10_SHADOW_OPTS", "USE_VSM_SHADOWS"}


def family_keys(source, entry, profile, flags, defines):
    """Fallback keys of a request (r10_shaderlib.c r10_request_family): level 1 keeps every define but the scene's,
    level 2 only the material's literals.  A request the library lacks then gets a compiled variant of the same
    material instead of an invisible placeholder."""
    base = request_key(source, entry, profile, flags)
    one, two = fnv1a64(b"\2", base), fnv1a64(b"\3", base)
    for name, value in defines:
        item = b"\1" + name.encode("latin-1") + b"=" + value.encode("latin-1")
        if name not in SCENE_DEFINES:
            one = fnv1a64(item, one)
        if name == "DEFINE_LITERALS":
            two = fnv1a64(item, two)
    return one, two


def record_key(name, profile, defines, level=0):
    """The key of a material variant the engine asks for (r10_shaderlib.c r10_record_key): its cache name (the pass
    file, the factory files and the entry, as Bioshock.exe sub_109AF770 joins them), the profile and the defines in
    the order FShaderCompiler10 hands them to the compiler (the recorded defines, DEFINE_LITERALS, D3DDRV10).
    Level 1 leaves out the scene's defines and level 2 keeps only DEFINE_LITERALS: the family fallbacks."""
    h = fnv1a64(b"R10REC1\0" + name.lower().encode("latin-1") + b"\0" + profile.encode("latin-1") + b"\0" + bytes([level]))
    for define, value in defines:
        if (level == 1 and define in SCENE_DEFINES) or (level == 2 and define != "DEFINE_LITERALS"):
            continue
        h = fnv1a64(b"\1" + define.encode("latin-1") + b"=" + value.encode("latin-1"), h)
    return h


def name_key(name):
    """FNV-1a 64 of an engine cache name in lower case (r10_shaderlib.c r10_name_key; FString compares ignore case)."""
    return fnv1a64(name.lower().encode("latin-1"))


def spk_files():
    """shaders.spk as the engine's shader file registry sees it: lower-case name -> (name, time stamp, text)."""
    from port_mobile_shaders import unpack
    return {name.lower(): (name, ts, source) for name, flags, ts, source in unpack(SPK.read_bytes())}


def include_target(line):
    """(system, name) of an #include line, as r10_reflect.c resolve_includes reads it; None for any other line."""
    p = line.lstrip(b" \t")
    if not p.startswith(b"#"):
        return None
    p = p[1:].lstrip(b" \t")
    if len(p) <= 7 or not p.startswith(b"include"):
        return None
    p = p[7:].lstrip(b" \t")
    if p[:1] not in (b"<", b'"'):
        return None
    end = p.find(b">" if p[:1] == b"<" else b'"', 1)
    if end < 0 or end - 1 >= 256:
        return None
    return p[:1] == b"<", p[1:end].decode("latin-1")


def resolve_includes(text, pairs, files, depth=0):
    """The text the device hashed for a request (r10_reflect.c resolve_includes) with the game's include handler
    (FShaderCompiler10's ID3D10Include::Open, Bioshock.exe 0x109B9280): a name the engine recorded a file for
    (<MATERIALFACTORY>, <VERTEXFACTORY>) opens that file, any other name the shaders.spk file of that name; a
    name that opens nothing keeps its line.  pairs: {lower-case name: file name}."""
    out = bytearray()
    at = 0
    while at < len(text):
        end = text.find(b"\n", at)
        end = len(text) if end < 0 else end
        following = end + 1 if end < len(text) else end
        target = include_target(text[at:end]) if depth < 16 else None
        found = files.get(pairs.get(target[1].lower(), target[1]).lower()) if target else None
        if found is not None:
            out += b"\n" + resolve_includes(found[2], pairs, files, depth + 1) + b"\n"
        else:
            out += text[at:following]
        at = following
    return bytes(out)


def shader_name(files, pass_file, pairs, entry):
    """The engine's cache name (sub_109AF770): the pass file, the recorded factory files in order, the entry."""
    return files[pass_file.lower()][0] + "".join(files[value.lower()][0] for _, value in pairs) + entry


def shader_time(files, pass_file, pairs):
    """The time stamp the engine checks a cache record against (sub_10C41760): the newest of the pass file and the
    recorded factory files (every record of ShaderCache.pcs10 carries exactly this value)."""
    return max([files[pass_file.lower()][1]] + [files[value.lower()][1] for _, value in pairs])


class Identity:
    """Finds the pass and factory files of a device request whose text holds them already resolved (R10REQ1)."""

    def __init__(self, files, cached_names):
        self.files, self.cached = files, cached_names
        passes = sorted(n for n in files if not n.startswith(("materialfactory_", "vertexfactory_")))
        factories = sorted(n for n in files if n.startswith("materialfactory_"))
        vertex = sorted(n for n in files if n.startswith("vertexfactory_"))
        self.candidates = []
        for p in passes:
            self.candidates.append((p, ()))
            for v in vertex:
                self.candidates.append((p, (("VERTEXFACTORY", v),)))
                for f in factories:
                    self.candidates.append((p, (("MATERIALFACTORY", f), ("VERTEXFACTORY", v))))
            for f in factories:
                self.candidates.append((p, (("MATERIALFACTORY", f),)))
        self.texts = {}

    def text(self, pass_file, pairs):
        key = (pass_file.lower(), tuple((n.upper(), v.lower()) for n, v in pairs))
        if key not in self.texts:
            self.texts[key] = resolve_includes(self.files[key[0]][2], {n.lower(): v for n, v in key[1]}, self.files)
        return self.texts[key]

    def __call__(self, source, entry):
        """(pass file, pairs) whose resolved text is the request's text; of several (a pass that includes no
        factory) the one with the fewest files whose name the shipped cache knows."""
        hits = [(p, pairs) for p, pairs in self.candidates if self.text(p, pairs) == source]
        if len(hits) > 1:
            known = sorted((h for h in hits if shader_name(self.files, h[0], h[1], entry) in self.cached),
                           key=lambda h: len(h[1]))
            hits = known[:1] if known and (len(known) == 1 or len(known[0][1]) < len(known[1][1])) else []
        if len(hits) != 1:
            raise ValueError("the request's text matches %d pass and factory combinations" % len(hits))
        return hits[0]


def dxbc_chunks(blob):
    if blob[:4] != b"DXBC":
        raise ValueError("not a DXBC container")
    count, = struct.unpack_from("<I", blob, 28)
    chunks = {}
    for i in range(count):
        at, = struct.unpack_from("<I", blob, 32 + 4 * i)
        fourcc, size = blob[at:at + 4], struct.unpack_from("<I", blob, at + 4)[0]
        chunks.setdefault(fourcc, blob[at + 8:at + 8 + size])
    return chunks


INTERFACE_CHUNKS = (b"RDEF", b"ISGN", b"OSGN")


def interface(blob):
    """What the engine reads of a program through D3D10ReflectShader and CreateInputLayout, without the code: the
    version token of SHDR, then the RDEF, ISGN and OSGN chunks unchanged in a container of the DXBC layout with the
    magic BSIF (r10_shaderlib.h)."""
    chunks = dxbc_chunks(blob)
    code = chunks.get(b"SHDR") or chunks.get(b"SHEX")
    version = struct.unpack_from("<I", code, 0)[0] if code else 0
    kept = [(c, chunks[c]) for c in INTERFACE_CHUNKS if c in chunks]
    out = bytearray(b"BSIF" + struct.pack("<I", version) + b"\0" * 20 + struct.pack("<I", len(kept)))
    out += b"\0" * (4 * len(kept))
    for i, (fourcc, data) in enumerate(kept):
        struct.pack_into("<I", out, 32 + 4 * i, len(out))
        out += fourcc + struct.pack("<I", len(data)) + data + b"\0" * (-len(data) % 4)
    return bytes(out)


def program_type(blob):
    count, = struct.unpack_from("<I", blob, 28)
    for i in range(count):
        at, = struct.unpack_from("<I", blob, 32 + 4 * i)
        if blob[at:at + 4] in (b"SHEX", b"SHDR"):
            return struct.unpack_from("<I", blob, at + 8)[0] >> 16
    raise ValueError("DXBC without SHEX/SHDR")


def pcs10_shaders():
    """(records, unique containers): records are (name, GUID as four words, time stamp, SHA-256 of the DXBC)."""
    rows = unpack_cache(PCS10.read_bytes())
    unique, records = {}, []
    for name, key, timestamp, code, raw in rows:
        digest = hashlib.sha256(code).hexdigest()
        unique.setdefault(digest, code)
        records.append((name, struct.unpack("<4I", key), timestamp, digest))
    return records, [unique[k] for k in sorted(unique)]


def embedded_sources():
    image = EXE.read_bytes()
    if hashlib.sha256(image).hexdigest() != PINS["Bioshock.exe"]:
        raise ValueError("Bioshock.exe is not the pinned GOG 1.1 build")
    sources = {}
    for marker, entry, profile, flags in EMBEDDED:
        at = image.find(marker)
        if at < 0:
            raise ValueError("embedded HLSL not found: %r" % marker)
        start = image.rfind(b"\0", 0, at) + 1
        end = image.index(b"\0", at)
        sources[marker] = image[start:end]
    return sources


class Compiler:
    """D3DCompile of Windows' d3dcompiler_47.dll (System32), or with game=True D3DX10CompileFromMemory of
    d3dx10_33.dll, the D3DX version BioShock ships and calls: material sources the game compiles at run time
    rely on its leniency (d3dcompiler_47 refuses, for example, a variable named like a function)."""

    def __init__(self, game=False):
        self.game = game
        if game:
            self.dll = ctypes.WinDLL(GAME_D3DX10)
            self.compile = self.dll.D3DX10CompileFromMemory
            self.compile.argtypes = [ctypes.c_void_p, ctypes.c_size_t, ctypes.c_char_p, ctypes.c_void_p, ctypes.c_void_p,
                                     ctypes.c_char_p, ctypes.c_char_p, ctypes.c_uint, ctypes.c_uint, ctypes.c_void_p,
                                     ctypes.POINTER(ctypes.c_void_p), ctypes.POINTER(ctypes.c_void_p), ctypes.c_void_p]
        else:
            self.dll = ctypes.WinDLL("d3dcompiler_47.dll")
            self.compile = self.dll.D3DCompile
            self.compile.argtypes = [ctypes.c_void_p, ctypes.c_size_t, ctypes.c_char_p, ctypes.c_void_p, ctypes.c_void_p,
                                     ctypes.c_char_p, ctypes.c_char_p, ctypes.c_uint, ctypes.c_uint,
                                     ctypes.POINTER(ctypes.c_void_p), ctypes.POINTER(ctypes.c_void_p)]
        self.compile.restype = ctypes.c_long

    @staticmethod
    def blob(pointer):
        vtable = ctypes.cast(ctypes.cast(pointer, ctypes.POINTER(ctypes.c_void_p))[0], ctypes.POINTER(ctypes.c_void_p))
        get_pointer = ctypes.WINFUNCTYPE(ctypes.c_void_p, ctypes.c_void_p)(vtable[3])
        get_size = ctypes.WINFUNCTYPE(ctypes.c_size_t, ctypes.c_void_p)(vtable[4])
        release = ctypes.WINFUNCTYPE(ctypes.c_ulong, ctypes.c_void_p)(vtable[2])
        data = ctypes.string_at(get_pointer(pointer), get_size(pointer))
        release(pointer)
        return data

    def __call__(self, source, entry, profile, flags, defines=()):
        code, errors = ctypes.c_void_p(), ctypes.c_void_p()
        macros = None
        if defines:
            # D3D_SHADER_MACRO {Name, Definition}[], NULL-terminated
            keep = [(ctypes.c_char_p(n.encode("latin-1")), ctypes.c_char_p(v.encode("latin-1"))) for n, v in defines]
            array = (ctypes.c_char_p * (2 * len(keep) + 2))()
            for i, (n, v) in enumerate(keep):
                array[2 * i], array[2 * i + 1] = n.value, v.value
            macros = ctypes.cast(array, ctypes.c_void_p)
        if self.game:
            hr = self.compile(source, len(source), None, macros, None, entry.encode(), profile.encode(), flags, 0, None,
                              ctypes.byref(code), ctypes.byref(errors), None)
        else:
            hr = self.compile(source, len(source), None, macros, None, entry.encode(), profile.encode(), flags, 0,
                              ctypes.byref(code), ctypes.byref(errors))
        message = self.blob(errors).decode("utf-8", "replace") if errors.value else ""
        if hr < 0 or not code.value:
            raise RuntimeError("D3DCompile %s %s failed (%08x): %s" % (entry, profile, hr & 0xFFFFFFFF, message))
        return self.blob(code)


def zig():
    found = os.environ.get("ZIG") or shutil.which("zig")
    if not found:
        raise RuntimeError("zig (0.16) is required for the host translator: set ZIG or put zig on PATH")
    return found


def compiler_tree():
    """src/{dxbc,spirv,util} of pristine DXVK 2.4.1, then the stand-ins of tools/shaderlib10/compat."""
    pins = json.loads((ROOT / "dependencies-dxvk.json").read_text())["source_files"]
    files = {}
    for sub in ("dxbc", "spirv", "util"):
        for p in (DXVK / "src" / sub).rglob("*"):
            if p.is_file() and p.suffix in (".h", ".cpp", ".c"):
                name = p.relative_to(DXVK).as_posix()
                data = p.read_bytes()
                if name in pins and hashlib.sha256(data.replace(b"\r\n", b"\n")).hexdigest() != pins[name]:
                    raise ValueError("DXVK source hash mismatch: " + name)
                files[p.relative_to(DXVK / "src").as_posix()] = data
    for p in COMPAT.rglob("*"):
        if p.is_file() and p.suffix in (".h", ".cpp") and p.parent != COMPAT:
            files[p.relative_to(COMPAT).as_posix()] = p.read_bytes()
    for name, data in files.items():
        target = TREE / name
        if not target.exists() or target.read_bytes() != data:
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_bytes(data)
    sources = sorted(TREE / n for n in files if n.endswith(".cpp") and n.split("/")[0] in ("dxbc", "spirv"))
    # SPIRV-Cross, with one overlay: demote-to-helper (DXVK's discard) becomes GLSL's discard, which is
    # what OpenGL ES and Vulkan 1.1 drivers without VK_EXT_shader_demote_to_helper_invocation have.
    cross_dir = TREE / "spirv-cross"
    cross_dir.mkdir(parents=True, exist_ok=True)
    for p in SPIRV_CROSS.iterdir():
        if p.suffix in (".h", ".hpp", ".cpp"):
            data = p.read_text(encoding="utf-8")
            if p.name == "spirv_glsl.cpp":
                for before, after in CROSS_OVERLAY:
                    if data.count(before) != 1:
                        raise ValueError("SPIRV-Cross overlay context mismatch: " + before[:60])
                    data = data.replace(before, after)
            target = cross_dir / p.name
            if not target.exists() or target.read_text(encoding="utf-8") != data:
                target.write_text(data, encoding="utf-8", newline="\n")
    cross = [cross_dir / n for n in ("spirv_cfg.cpp", "spirv_cross.cpp", "spirv_cross_parsed_ir.cpp",
                                     "spirv_cross_util.cpp", "spirv_glsl.cpp", "spirv_parser.cpp")]
    return sources + cross + [COMPAT / "util_log.cpp", ROOT / "tools/shaderlib10/bs_shaderc.cpp"]


CROSS_OVERLAY = [
    ("\tcase OpDemoteToHelperInvocationEXT:\n\t\tif (!options.vulkan_semantics)\n"
     "\t\t\tSPIRV_CROSS_THROW(\"GL_EXT_demote_to_helper_invocation is only supported in Vulkan GLSL.\");\n"
     "\t\trequire_extension_internal(\"GL_EXT_demote_to_helper_invocation\");\n"
     "\t\tstatement(backend.demote_literal, \";\");\n\t\tbreak;\n",
     "\tcase OpDemoteToHelperInvocationEXT:\n\t\tstatement(\"discard;\");\n\t\tbreak;\n"),
    ("\t\tif (!options.vulkan_semantics)\n"
     "\t\t\tSPIRV_CROSS_THROW(\"GL_EXT_demote_to_helper_invocation is only supported in Vulkan GLSL.\");\n"
     "\t\trequire_extension_internal(\"GL_EXT_demote_to_helper_invocation\");\n"
     "\t\t// Helper lane state with demote is volatile by nature.\n"
     "\t\t// Do not forward this.\n"
     "\t\temit_op(ops[0], ops[1], \"helperInvocationEXT()\", false);\n",
     "\t\temit_op(ops[0], ops[1], \"gl_HelperInvocation\", false);\n"),
]


def host_exe():
    """The translator for the PC, built with zig's clang for x86_64-windows-gnu (Itanium ABI: DXVK
    allocates SPIR-V ids inside call arguments, so the evaluation order must match the device's)."""
    sources = compiler_tree()
    includes = [TREE, DXVK / "include/vulkan/include", DXVK / "include/spirv/include", TREE / "spirv-cross"]
    flags = ["-target", "x86_64-windows-gnu", "-std=c++17", "-O2", "-w", "-DVK_NO_PROTOTYPES", "-DNDEBUG", "-DNOMINMAX",
             *["-I" + str(p) for p in includes]]
    digest = hashlib.sha256(json.dumps(flags).encode())
    for d in includes:
        for p in sorted(Path(d).rglob("*.h*")):
            digest.update(p.as_posix().encode() + sha(p).encode())
    headers = digest.hexdigest()
    objdir = WORK / "obj"
    objdir.mkdir(parents=True, exist_ok=True)

    def one(src):
        key = hashlib.sha256((sha(src) + headers).encode()).hexdigest()[:16]
        obj = objdir / ("%s-%s.o" % (src.stem, key))
        if not obj.exists():
            tmp = obj.with_suffix(".tmp.o")
            done = subprocess.run([zig(), "c++", *flags, "-c", str(src), "-o", str(tmp)], capture_output=True, text=True)
            if done.returncode:
                raise RuntimeError("compile failed: %s\n%s" % (src, done.stderr[-6000:]))
            tmp.replace(obj)
        return obj

    with concurrent.futures.ThreadPoolExecutor(max_workers=os.cpu_count() or 4) as pool:
        objects = list(pool.map(one, sources))
    exe = WORK / "bs_shaderc.exe"
    subprocess.run([zig(), "c++", "-target", "x86_64-windows-gnu", *map(str, objects), "-o", str(exe)], check=True,
                   capture_output=True)
    return exe


def glslc_one(args):
    index, text, stage = args
    with tempfile.TemporaryDirectory(dir=WORK) as tmp:
        source = Path(tmp) / ("s." + stage)
        source.write_text(text, encoding="utf-8", newline="\n")
        out = Path(tmp) / "s.spv"
        done = subprocess.run([str(SHADER_TOOLS / "glslc.exe"), "--target-env=vulkan1.0", "-O", "-fshader-stage=" + stage,
                               str(source), "-o", str(out)], capture_output=True, text=True)
        if done.returncode:
            return index, None, done.stderr[-3000:]
        return index, out.read_bytes(), None


VARYING = re.compile(r"^layout\(location = (\d+)\) (invariant )?(/\*@Q\d+@\*/)?(in|out) (float|vec2|vec3|vec4|int|ivec2|ivec3|ivec4|uint|uvec2|uvec3|uvec4) (\w+);$")
WIDE = {"float": ("vec4", "x"), "vec2": ("vec4", "xy"), "vec3": ("vec4", "xyz"), "vec4": ("vec4", ""),
        "int": ("ivec4", "x"), "ivec2": ("ivec4", "xy"), "ivec3": ("ivec4", "xyz"), "ivec4": ("ivec4", ""),
        "uint": ("uvec4", "x"), "uvec2": ("uvec4", "xy"), "uvec3": ("uvec4", "xyz"), "uvec4": ("uvec4", "")}


def es_fixup(text, pixel):
    """OpenGL ES links a vertex output and a fragment input only when their types match exactly and
    neither or both are invariant; DXVK declares each register with the components its stage uses.
    Every varying becomes a four-component variable (a #define keeps SPIRV-Cross' name for the
    components), invariance moves to gl_Position, and `index = 0` (no dual-source blending) goes."""
    out = []
    for line in text.split("\n"):
        m = VARYING.match(line)
        if m and ((pixel and m.group(4) == "in") or (not pixel and m.group(4) == "out")):
            location, direction, kind, name = m.group(1), m.group(4), m.group(5), m.group(6)
            wide, swizzle = WIDE[kind]
            flat = "flat " if wide != "vec4" else ""
            if swizzle:
                out.append("layout(location = %s) %s%s %s bs_%s;" % (location, flat, direction, wide, name))
                out.append("#define %s bs_%s.%s" % (name, name, swizzle))
            else:
                out.append("layout(location = %s) %s%s %s %s;" % (location, flat, direction, wide, name))
            continue
        if line.startswith("layout(location = ") and ", index = 0)" in line:
            line = line.replace(", index = 0)", ")")
        out.append(line)
        if not pixel and line.startswith("#version"):
            pass
    text = "\n".join(out)
    if not pixel:
        # After the extension lines: GLSL ES wants #extension before any declaration.
        lines = text.split("\n")
        at = max(i for i, l in enumerate(lines) if l.startswith("#version") or l.startswith("#extension")) + 1
        lines.insert(at, "invariant gl_Position;")
        text = "\n".join(lines)
    return text


ES_CB0 = re.compile(r"layout\(binding = (?:0|16), std140\) uniform (bs_(?:vs|ps)_cb0)\s*"
                    r"\{\s*vec4 m\[(\d+)\];\s*\}\s*cb0;")
ES_CLIP_ARRAY = re.compile(r"^out float gl_ClipDistance\[(\d+)\];$", re.MULTILINE)
ES_CLIP_RESERVED = re.compile(r"\b(bs_unclipped_main|bs_clip_test|bs_clip_distances(?:_hi)?|bs_link_reg\d+)\b")
ES_LINK_REGISTER = re.compile(r"^layout\(location = (\d+)\) ((?:\w+ )*?)(in|out) ((?:\w+ )+)(\w+);$", re.MULTILINE)


def es_cb0_variant(text):
    block = ES_CB0.search(text)
    if not block:
        return text, 0

    name, size = block.group(1), int(block.group(2)) * 16
    body = text[block.end():]
    chars = list(body)
    for access in re.finditer(r"cb0\.m\[", body):
        depth = 1
        end = access.end()
        while end < len(body) and depth:
            depth += (body[end] == "[") - (body[end] == "]")
            end += 1
        if depth:
            raise ValueError("Unterminated GLSL constant-buffer index")
        chars[access.start():access.end()] = "bs_c0("
        chars[end - 1] = ")"
    declaration = (f"#ifdef BS_CB0_TEXTURE\n"
                   f"layout(binding = BS_CB0_UNIT) uniform highp usamplerBuffer {name}_data;\n"
                   f"highp vec4 bs_c0(int i) {{ return uintBitsToFloat(texelFetch({name}_data, i)); }}\n"
                   f"#else\n{block.group()}\n#define bs_c0(i) cb0.m[i]\n#endif\n")
    return text[:block.start()] + declaration + "".join(chars), size


def es_link_by_name(body, direction):
    """Some ES drivers overlap an implicit varying with an explicitly located register.
    Match every interstage register by name in the fallback, keeping attributes and color outputs explicit."""
    def link(m):
        location, qualifiers, io, kind, original = m.groups()
        if io != direction:
            return m[0]
        name = f"bs_link_reg{location}"
        return f"{qualifiers}{io} {kind}{name};\n#define {original} {name}"
    return ES_LINK_REGISTER.sub(link, body)


def es_clip_fallback(text, declared, clip_count):
    """Every clip-writing VS in the pack shares one smooth FS interface."""
    if not 1 <= declared <= clip_count <= 8:
        raise ValueError(f"Invalid clip distance count {declared} (library {clip_count})")
    body = text.split("\n", 1)[1]  # compile_game_shader supplies the version
    body = es_link_by_name(body, "out")
    body = re.sub(r"^#extension GL_EXT_clip_cull_distance[^\n]*\n", "", body, flags=re.MULTILINE)
    outputs, inputs, checks, lanes = [], [], [], []
    for first, name in ((0, "bs_clip_distances"), (4, "bs_clip_distances_hi")):
        width = min(4, clip_count - first)
        if width <= 0:
            break
        kind = "float" if width == 1 else f"vec{width}"
        outputs.append(f"out highp {kind} {name};")
        inputs.append(f"in highp {kind} {name};")
        checks.append(f"{name} < 0.0" if width == 1 else f"any(lessThan({name}, {kind}(0.0)))")
        lanes.extend([name] if width == 1 else [f"{name}[{i}]" for i in range(width)])
    body = ES_CLIP_ARRAY.sub("\n".join(outputs), body)
    writes = set()

    def redirect(match):
        index = int(match[1])
        if index >= declared:
            raise ValueError(f"Clip write {index} outside declaration [{declared}]")
        writes.add(index)
        return lanes[index]

    body = re.sub(r"\bgl_ClipDistance\[(\d+)\](?=\s*=)", redirect, body)
    if not writes or "gl_ClipDistance" in body:
        raise ValueError("Unrecognized SPIRV-Cross clip distance write")
    missing = [f"    {lane} = 0.0;" for i, lane in enumerate(lanes) if i not in writes]
    if missing:  # a shorter or sparse VS must never clip on an unassigned plane
        body, n = re.subn(r"(\bvoid main\(\)\s*\{)", r"\1\n" + "\n".join(missing), body)
        if n != 1:
            raise ValueError("Missing GLSL main for clip distance initialization")
    helper = "\n".join(inputs + ["void bs_clip_test() { if (" + " || ".join(checks) + ") discard; }"])
    # Keep the ordinary GLSL prefix byte-identical. r10_gles.c selects the pieces at stage compilation.
    return "\n#if 0 // BS_CLIP_FALLBACK\n" + helper + "\n// BS_CLIP_VERTEX\n" + body + "\n#endif\n"


def es_device_variants(text, clip_count=None, pixel=False):
    """Prepare the clip and constant-buffer variants without rewriting GLSL on the phone."""
    collision = ES_CLIP_RESERVED.search(text)
    if collision:
        raise ValueError("Reserved clip fallback identifier: " + collision[0])
    text, size = es_cb0_variant(text)
    lines = []
    fallback = ""
    for line in text.split("\n"):
        statement = line.lstrip()
        if statement.startswith("out float gl_ClipDistance["):
            declared = int(ES_CLIP_ARRAY.fullmatch(statement)[1])
            fallback = es_clip_fallback(text, declared, declared if clip_count is None else clip_count)
            continue  # Adreno rejects the redeclaration of the built-in.
        if statement.startswith(("#extension GL_EXT_clip_cull_distance", "gl_ClipDistance[")):
            lines.extend(("#ifdef BS_CLIP_DISTANCE", line, "#endif"))
        else:
            lines.append(line)
    original = "\n".join(lines)
    if pixel:
        body = original.partition("\n")[2]
        fragment = es_link_by_name(body, "in")
        if fragment != body:
            fallback = "\n#if 0 // BS_CLIP_FRAGMENT\n" + fragment + "\n#endif\n"
    return original + fallback, size


VK_VARYING = re.compile(r"^layout\(location = (\d+)\) (invariant )?(in|out) (float|vec2|vec3|vec4) (\w+);$")


def vk_fixup(text, pixel):
    """Vulkan 1.1 without maintenance4 forbids a vertex output with more components than the fragment
    input it feeds (VUID-RuntimeSpirv-maintenance4-06817; the Adreno driver then rasterizes nothing),
    and DXVK declares each register with the components its stage uses.  As for OpenGL ES, every
    varying becomes a vec4 and a #define keeps SPIRV-Cross' name for the components."""
    out = []
    for line in text.split("\n"):
        m = VK_VARYING.match(line)
        if m and ((pixel and m.group(3) == "in") or (not pixel and m.group(3) == "out")):
            location, invariant, direction, kind, name = m.groups()
            wide, swizzle = WIDE[kind]
            if swizzle:
                out.append("layout(location = %s) %s%s %s bs_%s;" % (location, invariant or "", direction, wide, name))
                out.append("#define %s bs_%s.%s" % (name, name, swizzle))
                continue
        out.append(line)
    return "\n".join(out)


def strip_clip(text):
    """The same shader without SV_ClipDistance (drivers without clip distances): the clip plane
    output is dropped, everything else is unchanged."""
    out = []
    for line in text.split("\n"):
        s = line.strip()
        if s.startswith("#extension GL_EXT_clip_cull_distance") or s.startswith("out float gl_ClipDistance["):
            continue
        if s.startswith("gl_ClipDistance["):
            continue
        out.append(line)
    return "\n".join(out)


def translate(records):
    """records: [(kind, dxbc)] -> translated rows from bs_shaderc plus Vulkan SPIR-V."""
    WORK.mkdir(parents=True, exist_ok=True)
    data = bytearray()
    for kind, blob in records:
        data += struct.pack("<II", kind, len(blob)) + blob + b"\0" * (-len(blob) % 4)
    (WORK / "input.bin").write_bytes(bytes(data))
    exe = host_exe()
    done = subprocess.run([str(exe), str(WORK / "input.bin"), str(WORK / "translated.jsonl")], capture_output=True, text=True)
    if done.returncode:
        raise RuntimeError("bs_shaderc failed:\n" + done.stderr[-6000:])
    summary = json.loads(done.stdout)
    rows = [json.loads(line) for line in (WORK / "translated.jsonl").read_text(encoding="utf-8").splitlines() if line]
    failed = [r for r in rows if "error" in r]
    if failed:
        raise RuntimeError("%d shaders failed to translate, first: %s %s" % (len(failed), failed[0]["hash"], failed[0]["error"]))
    qualified = [r["hash"] for r in rows if any(q.strip() for q in r["qualifiers"].values())]
    if qualified:
        raise RuntimeError("pixel shaders with interpolation qualifiers need the per-program VS text: %s" % qualified[:4])
    for r in rows:
        r["es"] = es_fixup(re.sub(r"/\*@Q\d+@\*/", "", r["es"]), r["type"] == 0)
        r["vk"] = vk_fixup(r["vk"], r["type"] == 0)
    jobs = []
    for i, r in enumerate(rows):
        stage = "frag" if r["type"] == 0 else "vert"
        jobs.append(((i, "spirv"), r["vk"], stage))
        if r["clip_distance"]:
            jobs.append(((i, "spirv_noclip"), strip_clip(r["vk"]), stage))
    errors = []
    with concurrent.futures.ThreadPoolExecutor(max_workers=os.cpu_count() or 4) as pool:
        for (index, field), spirv, error in pool.map(glslc_one, jobs):
            if error:
                errors.append((rows[index]["hash"], error))
            rows[index][field] = spirv
    if errors:
        (WORK / "glslc-errors.txt").write_text("\n\n".join("%s\n%s" % e for e in errors), encoding="utf-8")
        raise RuntimeError("glslc rejected %d shaders, first %s:\n%s" % (len(errors), errors[0][0], errors[0][1]))
    return summary, rows, done.stderr


QUAL_BITS = {"flat": 1, "centroid": 2, "sample": 4}


HEADER_SIZE, SHADER_SIZE, MATERIAL_SIZE, KEY_SIZE = 64, 128, 32, 16


def pack(rows, materials, records, requests, key_inputs, *, version=VERSION):
    """Layout of android/render10/r10_shaderlib.h: Header, Shader[] sorted by (DXBC hash, size), Material[] sorted
    by (name key, GUID), Record[] and Request[] sorted by key, then the data area (offsets from the start).

    rows: translated programs, each with "sha" (SHA-256 of its DXBC) and "interface"; materials: (name, GUID words,
    time stamp, sha); records: (record key, sha, time stamp); requests: (request key, sha)."""
    if version not in (2, VERSION):
        raise ValueError(f"Unsupported shader library version {version}")
    rows = sorted(rows, key=lambda r: (int(r["hash"], 16), r["size"]))
    index = {r["sha"]: i for i, r in enumerate(rows)}
    material_rows = sorted({(name_key(n), words, ts, index[s]) for n, words, ts, s in materials})
    by_identity = {}
    for key, words, ts, shader in material_rows:
        if by_identity.setdefault((key, words), (ts, shader)) != (ts, shader):
            raise ValueError("two programs for one cache identity %016x" % key)
    record_rows = sorted({(k, index[s], ts) for k, s, ts in records})
    request_rows = sorted({(k, index[s]) for k, s in requests})
    for table in (record_rows, request_rows):
        keys = [row[0] for row in table]
        if len(keys) != len(set(keys)):
            raise ValueError("a key names two programs")
    data_base = HEADER_SIZE + len(rows) * SHADER_SIZE + len(material_rows) * MATERIAL_SIZE
    data_base += (len(record_rows) + len(request_rows)) * KEY_SIZE
    data_base += -data_base % 16
    blob = bytearray()

    stored = {}

    def put(data):
        digest = hashlib.sha256(data).digest()
        if digest in stored:
            return stored[digest]
        while len(blob) % 4:
            blob.append(0)
        at = data_base + len(blob)
        blob.extend(data)
        stored[digest] = at
        return at

    def packed(data):
        if not data:
            return 0, 0, 0
        z = zlib.compress(data, 9)
        return put(z), len(z), len(data)

    clip_count = max((int(ES_CLIP_ARRAY.search(r["es"])[1]) for r in rows
                      if version >= 3 and r["type"] == 1 and r["clip_distance"]), default=0)
    shader_table = bytearray()
    for r in rows:
        if r["type"] not in (0, 1):
            raise ValueError("program %s is neither a pixel nor a vertex shader" % r["hash"])
        bindings = b"".join(struct.pack("<III", *b) for b in r["bindings"])
        combined = b"".join(struct.pack("<Bbbx", *c) for c in r["combined"])
        ubos = b"".join(struct.pack("<BB", *u) for u in r["ubos"])
        icb = bytes.fromhex(r["icb"])
        b_off = put(bindings)
        c_off = put(combined)
        u_off = put(ubos)
        i_off = put(icb) if icb else 0
        s_off, s_size, s_raw = packed(r["spirv"])
        n_off, n_size, n_raw = packed(r.get("spirv_noclip") or b"")
        es, es_cb0_bytes = es_device_variants(r["es"], clip_count, r["type"] == 0) if version >= 3 else (r["es"], 0)
        e_off, e_size, e_raw = packed(es.encode())
        f_off, f_size, f_raw = packed(r["interface"])
        flags = (1 if r["push_constants"] else 0) | (2 if r["clip_distance"] else 0)
        if version >= 3 and r["type"] == 1 and "gl_VertexID" in es:
            flags |= 4
        shader_table += struct.pack("<QI16sBBHII22I",
                                    int(r["hash"], 16), r["size"], bytes.fromhex(r["checksum"]), r["type"], flags,
                                    len(r["combined"]), r["input_mask"], r["output_mask"],
                                    b_off, len(r["bindings"]), c_off, u_off, len(r["ubos"]), i_off, len(icb),
                                    s_off, s_size, s_raw, n_off, n_size, n_raw, e_off, e_size, e_raw,
                                    f_off, f_size, f_raw, es_cb0_bytes, 0, 0)
    assert len(shader_table) == len(rows) * SHADER_SIZE, len(shader_table)
    material_table = b"".join(struct.pack("<Q4III", key, *words, ts, shader) for key, words, ts, shader in material_rows)
    record_table = b"".join(struct.pack("<QII", key, shader, ts) for key, shader, ts in record_rows)
    request_table = b"".join(struct.pack("<QII", key, shader, 0) for key, shader in request_rows)
    while len(blob) % 16:
        blob.append(0)
    tables = bytes(shader_table) + material_table + record_table + request_table
    library_key = int(hashlib.sha256(key_inputs + tables + bytes(blob)).hexdigest()[:16], 16)
    total = data_base + len(blob)
    at_materials = HEADER_SIZE + len(shader_table)
    at_records = at_materials + len(material_table)
    at_requests = at_records + len(record_table)
    magic = MAGIC if version == VERSION else b"BS10SHL2"
    header = struct.pack("<8sIIIIIIIIIIQII", magic, version, len(rows), HEADER_SIZE, len(material_rows), at_materials,
                         len(record_rows), at_records, len(request_rows), at_requests, data_base, library_key, total, 0)
    assert len(header) == HEADER_SIZE, len(header)
    out = header + tables
    out += b"\0" * (data_base - len(out))
    sizes = {"materials": len(material_rows), "records": len(record_rows), "requests": len(request_rows),
             "interface_bytes": sum(len(r["interface"]) for r in rows)}
    return out + bytes(blob), library_key, sizes


REQUESTS = ROOT / "build/render10/requests"   # <userdata>/r10-requests/*.req pulled from devices (game text: not committed)
PLACEHOLDERS = [(0xFFFFFFFFFFFFFF01, "float4 main() : SV_Position { return float4(0, 0, 0, 1); }", "vs_4_0"),
                (0xFFFFFFFFFFFFFF02, "float4 main() : SV_Target { return float4(0, 0, 0, 0); }", "ps_4_0")]


def parse_request(data):
    """A request of the device (r10_reflect.c, r10_shaderlib.c): R10REQ1 = header lines, define=<n>:<name><n>:<value>
    lines, source=<n> and the text with its includes resolved (a D3DX10CompileFromMemory call); R10REQ2 = a material
    variant the engine's cache lookup asked for (record, name, pass, pair=<name>:<file> lines, entry, profile, flags,
    guid, time, the defines, end), no text.  Returns (version, fields, pairs, defines, text or None)."""
    version = 1 if data.startswith(b"R10REQ1\n") else 2 if data.startswith(b"R10REQ2\n") else 0
    if not version:
        raise ValueError("not a native renderer request")
    at, fields, pairs, defines = 8, {}, [], []
    while True:
        end = data.index(b"\n", at)
        line = data[at:end].decode("latin-1")
        if line.startswith("define="):
            rest = data[at + 7:]
            n = int(rest[:rest.index(b":")])
            rest = rest[rest.index(b":") + 1:]
            name = rest[:n].decode("latin-1")
            rest = rest[n:]
            m = int(rest[:rest.index(b":")])
            rest = rest[rest.index(b":") + 1:]
            value = rest[:m].decode("latin-1")
            defines.append((name, value))
            at = len(data) - len(rest) + m + 1
            continue
        key, _, value = line.partition("=")
        at = end + 1
        if version == 1 and key == "source":
            return version, fields, pairs, defines, data[at:at + int(value)]
        if version == 2 and line == "end":
            return version, fields, pairs, defines, None
        if key == "pair":
            pairs.append(tuple(value.split(":", 1)))
        else:
            fields[key] = value


def collected_requests(compiler, files, cached_names, seen, programs):
    """The device's requests (variants ShaderCache.pcs10 lacks), compiled with the game's d3dx10_33.  Returns the
    request table rows (key of the resolved text: the D3DX10CompileFromMemory path), the record rows (record key and
    time stamp: the engine's cache lookup) and a report.  Programs new to the library are added to `programs`."""
    identity = Identity(files, cached_names)
    requests, records, request_families, record_families = [], [], [], []
    report = {"compiled": 0, "failed": [], "text_requests": 0, "record_requests": 0, "names": {}}
    done = {}
    for path in sorted(REQUESTS.glob("*.req")) if REQUESTS.is_dir() else []:
        version, fields, pairs, defines, source = parse_request(path.read_bytes())
        entry, profile, flags = fields["entry"], fields["profile"], int(fields["flags"])
        if version == 1:
            report["text_requests"] += 1
            key = request_key(source, entry, profile, flags, defines)
            if "%016x" % key != fields["key"]:
                raise ValueError("request key differs from the device's: %s" % path.name)
            pass_file, found = identity(source, entry)
            pairs = [(n, files[v][0]) for n, v in found]
        else:
            report["record_requests"] += 1
            pass_file = fields["pass"]
            source = identity.text(pass_file, pairs)
            key = request_key(source, entry, profile, flags, defines)
        name = shader_name(files, pass_file, pairs, entry)
        stamp = shader_time(files, pass_file, pairs)
        record = record_key(name, profile, defines)
        if version == 2:
            if fields["name"].lower() != name.lower() or int(fields["time"]) != stamp:
                raise ValueError("request %s names %s (time %s), its files give %s (time %d)" %
                                 (path.name, fields["name"], fields["time"], name, stamp))
            if fields["record"] != "%016x" % record:
                raise ValueError("record key differs from the device's: %s" % path.name)
        if record in done:
            continue
        try:
            dxbc = compiler(source, entry, profile, flags, defines)
        except RuntimeError as error:
            report["failed"].append({"request": path.name, "error": str(error)[:400]})
            continue
        digest = hashlib.sha256(dxbc).hexdigest()
        done[record] = digest
        report["compiled"] += 1
        report["names"][name] = report["names"].get(name, 0) + 1
        requests.append((key, digest))
        records.append((record, digest, stamp))
        request_families.append((family_keys(source, entry, profile, flags, defines), digest))
        record_families.append(((record_key(name, profile, defines, 1), record_key(name, profile, defines, 2)), digest, stamp))
        if digest not in seen:
            seen.add(digest)
            programs.append((2, dxbc))
    # Fallbacks: the first compiled variant (in request order) of each family, never shadowing an exact key.
    taken = {key for key, _ in requests}
    for keys, digest in request_families:
        for key in keys:
            if key not in taken:
                taken.add(key)
                requests.append((key, digest))
                report["fallbacks"] = report.get("fallbacks", 0) + 1
    taken = {key for key, _, _ in records}
    for keys, digest, stamp in record_families:
        for key in keys:
            if key not in taken:
                taken.add(key)
                records.append((key, digest, stamp))
                report["record_fallbacks"] = report.get("record_fallbacks", 0) + 1
    report["names"] = dict(sorted(report["names"].items()))
    return requests, records, report


def build():
    generator_sha256 = sha(Path(__file__))
    variants_sha256 = sha(Path(variants.__file__))
    programs, requests, embedded = [], [], []
    materials, containers = pcs10_shaders()
    for blob in containers:
        programs.append((0, blob))
    files = spk_files()
    for name, words, stamp, digest in materials:
        parts = name.split(".hlsl")
        # The time stamp of a cache record is the newest of its pass and factory files (sub_10C41760).
        if all((p + ".hlsl").lower() in files for p in parts[:-1]):
            expected = max(files[(p + ".hlsl").lower()][1] for p in parts[:-1])
            if expected != stamp:
                raise ValueError("cache record %s has time %d, its files %d" % (name, stamp, expected))
    compiler = Compiler()
    sources = embedded_sources()
    seen = {hashlib.sha256(b).hexdigest() for b in containers}
    for marker, entry, profile, flags in EMBEDDED:
        source = sources[marker]
        dxbc = compiler(source, entry, profile, flags)
        key = request_key(source, entry, profile, flags)
        digest = hashlib.sha256(dxbc).hexdigest()
        requests.append((key, digest))
        embedded.append({"entry": entry, "profile": profile, "flags": flags, "key": "%016x" % key,
                         "source_sha256": hashlib.sha256(source).hexdigest(), "dxbc_bytes": len(dxbc)})
        if digest not in seen:
            seen.add(digest)
            programs.append((1, dxbc))
    cached_names = {name for name, _, _, _ in materials}
    if os.environ.get("BIOSHOCK_SHADERLIB_REQUESTS", "1") != "0":
        collected, records, collected_report = collected_requests(Compiler(game=True), files, cached_names, seen, programs)
    else:   # a library of the PC alone (the shipped cache and its twins): to measure what device requests still add
        collected, records, collected_report = [], [], {"skipped": "BIOSHOCK_SHADERLIB_REQUESTS=0"}
    requests += collected
    # The shipped records decoded and proven, and their twins (tools/shaderlib10/variants.py): record keys and family
    # keys of every proven record, cache identities of the twins.  Device requests come first: their family keys keep
    # the variant a device asked for.
    twin_report = {}
    if variants.LAYOUTS.exists() and os.environ.get("BIOSHOCK_SHADERLIB_TWINS", "1") != "0":
        code_of = {hashlib.sha256(c).hexdigest(): c for c in containers}
        layouts = json.loads(variants.LAYOUTS.read_text(encoding="utf-8"))
        pool = variants.CompilePool(WORK / "compile-cache", ROOT / "tools")
        proven, twins, twin_report = variants.shipped_variants([(n, w, t, code_of[d]) for n, w, t, d in materials],
                                                               layouts, pool)
        twin_report["layouts"] = len(layouts)
        exact = {key for key, _, _ in records}
        families = []
        for name, profile, defines, stamp, code in proven:
            digest = hashlib.sha256(code).hexdigest()
            key = record_key(name, profile, defines)
            if key not in exact:
                exact.add(key)
                records.append((key, digest, stamp))
            families.append(((record_key(name, profile, defines, 1), record_key(name, profile, defines, 2)), digest, stamp))
        for name, words, stamp, dxbc, profile, defines, rule in twins:
            if stamp is None:   # a record the cache never had: the engine's time stamp of its files
                stamp = shader_time(files, layouts[name]["pass"], [tuple(x) for x in layouts[name]["pairs"]])
            digest = hashlib.sha256(dxbc).hexdigest()
            materials.append((name, words, stamp, digest))
            key = record_key(name, profile, defines)
            if key not in exact:
                exact.add(key)
                records.append((key, digest, stamp))
            families.append(((record_key(name, profile, defines, 1), record_key(name, profile, defines, 2)), digest, stamp))
            if digest not in seen:
                seen.add(digest)
                programs.append((3, dxbc))
        taken = {key for key, _, _ in records}
        for keys, digest, stamp in families:
            for key in keys:
                if key not in taken:
                    taken.add(key)
                    records.append((key, digest, stamp))
                    twin_report["family keys"] = twin_report.get("family keys", 0) + 1
    for key, text, profile in PLACEHOLDERS:
        dxbc = compiler(text.encode(), "main", profile, 0)
        digest = hashlib.sha256(dxbc).hexdigest()
        requests.append((key, digest))
        if digest not in seen:   # the device creates shaders from them
            seen.add(digest)
            programs.append((2, dxbc))
    summary, rows, messages = translate(programs)
    by_container = {(len(blob), blob[4:20].hex()): blob for _, blob in programs}   # the container's own checksum
    for r in rows:
        blob = by_container[(r["size"], r["checksum"])]
        r["sha"] = hashlib.sha256(blob).hexdigest()
        r["interface"] = interface(blob)
    key_inputs = (sha(PCS10) + sha(SPK) + sha(EXE) + sha(ROOT / "tools/shaderlib10/bs_shaderc.cpp") +
                  generator_sha256 + variants_sha256).encode()
    library, key, sizes = pack(rows, materials, records, requests, key_inputs)
    OUTPUT.parent.mkdir(parents=True, exist_ok=True)
    OUTPUT.write_bytes(library)
    manifest = {"library": OUTPUT.relative_to(ROOT).as_posix(), "sha256": hashlib.sha256(library).hexdigest(),
                "generator_sha256": generator_sha256, "variants_sha256": variants_sha256,
                "bytes": len(library), "key": "%016x" % key, "version": VERSION, "vulkan": "1.0", "pcs10_sha256": sha(PCS10),
                "spk_sha256": sha(SPK), "pcs10_records": len(materials), "unique_dxbc": len(containers),
                "tables": sizes, "embedded": embedded, "collected_requests": collected_report, "settings_twins": twin_report,
                "translator": summary,
                "spirv_bytes": sum(len(r["spirv"]) for r in rows), "glsl_es_bytes": sum(len(r["es"]) for r in rows),
                "clip_distance_shaders": sum(1 for r in rows if r["clip_distance"]),
                "push_constant_shaders": sum(1 for r in rows if r["push_constants"]),
                "max_combined_samplers": max(len(r["combined"]) for r in rows),
                "compiler_messages": [line for line in messages.splitlines() if line.strip()][:50]}
    MANIFEST.write_text(json.dumps(manifest, indent=2) + "\n")
    print(json.dumps({k: v for k, v in manifest.items() if k != "compiler_messages"}, indent=2))


def validate():
    rows = [json.loads(line) for line in (WORK / "translated.jsonl").read_text(encoding="utf-8").splitlines() if line]
    print(len(rows), "rows")


def layouts(trace):
    """tools/shaderlib10/guid_layouts.json from a shader trace of the device (BIOSHOCK_R10_SHADER_TRACE=1)."""
    learned = variants.learn_layouts(trace, parse_request)
    variants.LAYOUTS.write_bytes((json.dumps(learned, indent=1, sort_keys=True) + "\n").encode("utf-8"))
    print(len(learned), "layouts,", sum(v["samples"] for v in learned.values()), "traced variants fit")


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("command", choices=("build", "validate", "layouts"))
    parser.add_argument("trace", nargs="?", type=Path, help="layouts: <userdata>/r10-shader-trace.txt")
    args = parser.parse_args()
    if args.command == "layouts":
        if not args.trace:
            parser.error("layouts needs the trace file")
        layouts(args.trace)
    else:
        build() if args.command == "build" else validate()


if __name__ == "__main__":
    main()

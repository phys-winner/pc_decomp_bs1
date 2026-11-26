"""Every shader variant the game can ask for, built on the PC from the shipped cache (tools/build_shaderlib10.py).

Bioshock.exe keys a shader by a 128-bit permutation GUID: the values its field writers record, packed from bit 0 in
the order they run, the factory's literals and then the pass's defines (sub_10989A50 a literal and its width,
sub_10983110 a boolean, sub_10981360 a define and its width, sub_1098C5B0 a UV index). Sampler slots take the
material's width (FMaterialFactory+55), channel IDs 2 bits, booleans 1, MAX_INCIDENT_LIGHTS 3, DISTORTION_MODE 2,
NUM_SAMPLES 5, TRANSFER_TYPE 3, UV indices 3, other defines 1.

The shipped ShaderCache.pcs10 holds the variants its makers' build reached at their settings. Each shipped record is
decoded and the decode proved by compiling it with the game's d3dx10_33 to the shipped DXBC byte for byte; then the
RULES make the variants the cache lacks: fields that follow a player setting (cube map, ripples, soft particles,
realtime reflection), the patch (shadow options), the render target (blur sample counts), a light (projection mask,
cheap shadows) or a run-time state the cache build never reached (per-vertex opacity of sorted translucent objects,
light layers without the base pass), plus the lit passes of materials the cache drew unlit only and the ripple
system's materials.  Each rule cites the function of Bioshock.exe that decides it; the materials a rule may touch
come from the game's packages (materials.py).  On 21 November 2026 the result held every variant devices had asked
for (381 requests, 8342 traced identities) without a device request in the build.

The layout of each shader name (its fields in writer order) comes from guid_layouts.json, learned from a shader
trace (BIOSHOCK_R10_SHADER_TRACE=1: GUIDs beside the recorded fields) by `build_shaderlib10.py layouts TRACE`; the
names no trace reached are composed from their factory's writer (FACTORY_WRITERS) and a sibling pass.
"""
import collections
import concurrent.futures
import hashlib
import json
import os
import re
import struct
from pathlib import Path

import materials  # noqa: E402  (tools/shaderlib10, the game's materials from its packages)

HERE = Path(__file__).resolve().parent
LAYOUTS = HERE / "guid_layouts.json"
# Fields wider than one bit, as the writer calls of Bioshock.exe give them (sub_10981360 a define, sub_1098C5B0 a
# UV index; every other define, boolean and literal is one bit, channel IDs two, sampler slots the material's width).
WIDTHS = {"MAX_INCIDENT_LIGHTS": 3, "DISTORTION_MODE": 2, "NUM_SAMPLES": 5, "TRANSFER_TYPE": 3,
          "UV_0": 3, "UV_1": 3, "UV_2": 3, "UV_3": 3, "UV_MASK_0": 3, "UV_MASK_1": 3, "UV_MASK_2": 3, "UV_MASK_3": 3}
LITERAL = re.compile(r"^\s*([\w.]+) = (.+);\\$")


def width(name, kind, slot):
    if kind == "bool":
        return 1
    if name.endswith(".samplerSlot"):
        return slot
    if name.endswith(".channelID"):
        return 2
    if name.endswith(("_Idx", "_Idx1", "_Idx2")):
        return 3
    return WIDTHS.get(name, 1)


def fields(defines):
    """The recorded fields of a define list as FShaderCompiler10 builds it: [(name, kind, value)], literals in order
    (from DEFINE_LITERALS), then the defines in order (the GUID's order for the layouts this module accepts)."""
    out = []
    for line in dict(defines).get("DEFINE_LITERALS", "").split("\n"):
        m = LITERAL.match(line)
        if m:
            v = m.group(2)
            out.append((m.group(1), "bool" if v in ("true", "false") else "int",
                        1 if v == "true" else 0 if v == "false" else int(v)))
    out += [(n, "define", int(v)) for n, v in defines if n not in ("DEFINE_LITERALS", "D3DDRV10")]
    return out


def encode(values, slot):
    guid, at = 0, 0
    for name, kind, value in values:
        w = width(name, kind, slot)
        if value < 0 or value >= 1 << w:
            return None
        guid |= value << at
        at += w
    return guid


SLOT_WIDTHS = (0, 1, 2, 3, 4)   # max(1, bit length of the texture count), 0 for a material without textures


def decode(guid, layout):
    """Every reading of a GUID with a layout [(name, kind)]: (slot width, [(name, kind, value)])."""
    found = []
    for slot in SLOT_WIDTHS:
        at, values = 0, []
        for name, kind in layout:
            w = width(name, kind, slot)
            values.append((name, kind, (guid >> at) & ((1 << w) - 1)))
            at += w
        if not guid >> at:
            found.append((slot, values))
    return found


def define_list(values):
    """FShaderCompiler10's defines (Bioshock.exe sub_109B9350): the recorded defines, DEFINE_LITERALS, D3DDRV10."""
    lines = []
    for name, kind, value in values:
        if name == HIDDEN:
            continue
        if kind == "bool":
            lines.append(("%s = %s;\\\n" if "." in name else "\t%s = %s;\\\n") % (name, "true" if value else "false"))
        elif kind == "int":
            lines.append(("\t%s.%s = %d;\\\n" % (*name.split(".", 1), value)) if "." in name else "\t%s = %d;\\\n" % (name, value))
    return [(n, "%i" % v) for n, k, v in values if k == "define"] + [("DEFINE_LITERALS", "".join(lines)), ("D3DDRV10", "1")]


def learn_layouts(trace_path, parse_request):
    """{name: {"fields": [[name, kind]], "pass": file, "pairs": [[name, file]], "entry", "profile"}} for every name of a
    trace whose GUIDs pack its recorded fields in that order (all of its entries must fit)."""
    data = Path(trace_path).read_bytes()
    entries = collections.defaultdict(list)
    at = 0
    while True:
        start = data.find(b"R10TRACE ", at)
        if start < 0:
            break
        end = data.index(b"\n", start)
        if data[start + 9:end].startswith(b"unreadable"):
            at = end + 1
            continue
        block_end = data.index(b"\nend\n", end) + 5
        version, req, pairs, defines, _ = parse_request(data[end + 1:block_end])
        g = req["guid"]
        guid = sum(int(g[i:i + 8], 16) << (32 * (i // 8)) for i in range(0, 32, 8))
        entries[req["name"]].append((guid, fields(defines), req, pairs))
        at = block_end
    layouts = {}
    for name, items in sorted(entries.items()):
        groups = collections.defaultdict(list)
        for g, f, r, p in items:
            groups[tuple((n, k) for n, k, v in f)].append((g, f))
        shapes = []
        for shape, group in sorted(groups.items(), key=lambda kv: -len(kv[1])):
            orders = list(_orders(shape))
            hidden, fit = hidden_bits(group)
            best = (orders[0], hidden, fit)
            if fit < 0.9:
                # Other write orders, each with at most one unrecorded bit, then the full search on the best one.
                for order in orders[1:]:
                    reordered = [(g, [f[i] for i in order]) for g, f in group]
                    h, s = hidden_bits(reordered, pairs=False)
                    if s > best[2] + 1e-9:
                        best = (order, h, s)
                    if s == 1.0:
                        break
                if best[2] < 1.0 and best[0] != orders[0]:
                    reordered = [(g, [f[i] for i in best[0]]) for g, f in group]
                    h, s = hidden_bits(reordered)
                    if s > best[2]:
                        best = (best[0], h, s)
            order, hidden, fit = best
            if fit < 0.5:
                continue
            layout = [list(shape[i]) for i in order]
            for at in sorted(hidden, reverse=True):
                layout.insert(at, [HIDDEN, "bool"])
            shapes.append({"fields": layout, "samples": len(group), "fit": round(fit, 3)})
        if not shapes:
            continue
        g, f, req, pairs = items[0]
        layouts[name] = {"shapes": shapes, "pass": req["pass"], "pairs": [list(p) for p in pairs],
                         "entry": req["entry"], "profile": req["profile"], "samples": len(items)}
    return layouts


HIDDEN = "<unrecorded>"   # a bit the engine skips without recording a field (always 0 in the traced GUIDs)


def _orders(shape):
    """Candidate write orders of a shape (literals first, then defines, as recorded): as recorded, then the last one
    to three literals moved into the defines (a pass or vertex factory component that writes a literal among its
    defines, e.g. the particle component's UseMinDistortionClipDistance)."""
    lits = [i for i, (n, k) in enumerate(shape) if k != "define"]
    defs = [i for i, (n, k) in enumerate(shape) if k == "define"]
    yield lits + defs
    for k in (1, 2, 3):
        if k > len(lits):
            break
        head, tail = lits[:-k], lits[-k:]
        for p in range(len(defs) + 1):
            yield head + defs[:p] + tail + defs[p:]


def _fits(guid, values, hidden):
    trial = list(values)
    for at in sorted(hidden, reverse=True):
        trial.insert(at, (HIDDEN, "bool", 0))
    return any(encode(trial, slot) == guid for slot in SLOT_WIDTHS)


def hidden_bits(group, pairs=True):
    """(positions, share of the group that packs): the zero bits (none, one or two) that, inserted between the
    recorded fields, make the most traced GUIDs of one shape pack (a writer of the key that records nothing)."""
    sample = group[:200] if pairs else group[:60]

    def score(hidden):
        return sum(1 for g, f in sample if _fits(g, f, hidden)) / len(sample)

    best = ((), score(()))
    if best[1] == 1.0:
        return best
    n = len(sample[0][1])
    for at in range(n + 1):
        s = score((at,))
        if s > best[1]:
            best = ((at,), s)
    if best[1] < 1.0 and pairs:
        for i in range(n + 1):
            for j in range(i, n + 1):
                s = score((i, j))
                if s > best[1] + 1e-9:
                    best = ((i, j), s)
    return best


# ---------------------------------------------------------------- compiling in worker processes
_worker = {}


def _init(tools_dir):
    import sys
    sys.path.insert(0, tools_dir)
    import build_shaderlib10 as b
    _worker["b"] = b
    _worker["compiler"] = b.Compiler(game=True)
    _worker["identity"] = b.Identity(b.spk_files(), set())


def _compile(job):
    key, pass_file, pairs, entry, profile, defines = job
    b = _worker["b"]
    try:
        text = _worker["identity"].text(pass_file, [tuple(p) for p in pairs])
        return key, _worker["compiler"](text, entry, profile, b.MATERIAL_FLAGS, [tuple(d) for d in defines]), None
    except RuntimeError as error:
        return key, None, str(error)[:300]


class CompilePool:
    """The game's compiler in worker processes, with a disk cache of DXBC by the request's inputs."""

    def __init__(self, cache_dir, tools_dir):
        self.cache = Path(cache_dir)
        self.cache.mkdir(parents=True, exist_ok=True)
        self.tools_dir = str(tools_dir)

    @staticmethod
    def key(pass_file, pairs, entry, profile, defines):
        h = hashlib.sha256(json.dumps([pass_file.lower(), [[n.upper(), f.lower()] for n, f in pairs], entry, profile,
                                       defines]).encode())
        return h.hexdigest()[:32]

    def run(self, jobs):
        """jobs: [(pass, pairs, entry, profile, defines)] -> {key: dxbc or None}, {key: error}."""
        out, errors, todo = {}, {}, []
        for job in jobs:
            k = self.key(*job)
            path = self.cache / (k + ".dxbc")
            if path.exists():
                out[k] = path.read_bytes()
            elif k not in out:
                out[k] = None
                todo.append((k,) + tuple(job))
        if todo:
            workers = max(1, min(8, (os.cpu_count() or 4)))
            with concurrent.futures.ProcessPoolExecutor(workers, initializer=_init, initargs=(self.tools_dir,)) as pool:
                for k, dxbc, error in pool.map(_compile, todo, chunksize=8):
                    if dxbc is None:
                        errors[k] = error
                        continue
                    (self.cache / (k + ".dxbc")).write_bytes(dxbc)
                    out[k] = dxbc
        return out, errors


# The fields that follow a player setting, the scene or patch 1.1, as rules over the shipped records: (label, a
# substring of the shader name, {field: (value in the shipped record or None for any, value of the twin)}, who):
# "all" = every shipped record the rule applies to, "evidence" = materials (factory literals) the cache shows with the
# twin's values somewhere. Learned from what devices asked for (2026-11-21: 381 requests, a trace of 8342 identities)
# and the writers in Bioshock.exe; applied here to the whole cache.
RULES = [
    # USES_SPECULAR_CUBEMAP = the scene's cube map && UShader +172/+176 >= 0.01 && +208 bit 0 (sub_109B2C10, 0x10A7EB80)
    ("cube map off", "", {"USES_SPECULAR_CUBEMAP": (1, 0)}, "all"),
    ("cube map on", "", {"USES_SPECULAR_CUBEMAP": (0, 1)}, "cube"),
    # EnableRippleEffects = the material's flag 8 && the ripple system (sub_1098D0B0); the cache has ripples off.
    # With ripples a water surface also draws as distortion, and a water material whose only texture is its
    # diffuse map loses it (the ripple normal map replaces it).
    ("ripples on", "Fluid", {"EnableRippleEffects": (0, 1)}, "all"),
    ("ripples on, distortion", "Fluid", {"EnableRippleEffects": (0, 1), "DISTORTION_MODE": (0, 1)}, "all"),
    ("ripples on, no diffuse", "Fluid", {"EnableRippleEffects": (0, 1), "DiffuseBind.samplerSlot": (1, 0)}, "all"),
    ("ripples on, distortion, no diffuse", "Fluid",
     {"EnableRippleEffects": (0, 1), "DISTORTION_MODE": (0, 1), "DiffuseBind.samplerSlot": (1, 0)}, "all"),
    ("ripples off", "Fluid", {"EnableRippleEffects": (1, 0)}, "all"),
    ("realtime reflection off", "Fluid", {"UseRealtimeReflection": (1, 0)}, "all"),
    # Fluids can keep ripples with reflections or the specular cube map disabled. Rules are applied
    # to shipped records once, so the separate setting rules cannot generate these combinations.
    ("ripples on, realtime reflection off", "MaterialFactory_Fluid.hlsl",
     {"EnableRippleEffects": (0, 1), "UseRealtimeReflection": (1, 0)}, "all"),
    ("ripples on, cube map off", "MaterialFactory_Fluid.hlsl",
     {"EnableRippleEffects": (0, 1), "USES_SPECULAR_CUBEMAP": (1, 0)}, "all"),
    # Patch 1.1 writes NVIDIA_DX10_SHADOW_OPTS=1 (sub_10983EA0); the shipped shadow records have 0.
    ("shadow options of patch 1.1", "", {"NVIDIA_DX10_SHADOW_OPTS": (0, 1)}, "all"),
    # UseHighDetailSoftParticles (RenderConfig +140, on in Default.ini) makes sub_10AAC3C0 ignore an emitter's
    # BackFadeType and fade every translucent emitter against the scene (BackFadeType 1, distance 20); the others and
    # the plane fade go off.  The cache was built with the option off (only the emitters' own BackFadeType).
    ("soft particles on", "Particle.hlsl", {"USE_BACK_FADE": (None, 1), "USE_BACK_FADE_PLANE": (None, 0)}, "all"),
    ("soft particles on, emitter not faded", "Particle.hlsl",
     {"USE_BACK_FADE": (None, 0), "USE_BACK_FADE_PLANE": (None, 0)}, "all"),
    # The shadow depth test of a light (sub_10983EA0): UseShadowProjectionMask and UseCheapShadows are the light's
    # (+180, +184), NVIDIA_DX10_SHADOW_OPTS is 1 on the patched game; any light can have either bit.
    ("light without projection mask", "DepthShadows.hlsl",
     {"UseShadowProjectionMask": (None, 0), "NVIDIA_DX10_SHADOW_OPTS": (None, 1)}, "all"),
    ("light with projection mask", "DepthShadows.hlsl",
     {"UseShadowProjectionMask": (None, 1), "NVIDIA_DX10_SHADOW_OPTS": (None, 1)}, "all"),
    ("light with cheap shadows", "DepthShadows.hlsl",
     {"UseCheapShadows": (None, 1), "NVIDIA_DX10_SHADOW_OPTS": (None, 1)}, "all"),
    ("light without cheap shadows", "DepthShadows.hlsl",
     {"UseCheapShadows": (None, 0), "NVIDIA_DX10_SHADOW_OPTS": (None, 1)}, "all"),
    ("light without projection mask, cheap shadows", "DepthShadows.hlsl",
     {"UseShadowProjectionMask": (None, 0), "UseCheapShadows": (None, 1), "NVIDIA_DX10_SHADOW_OPTS": (None, 1)}, "all"),
    ("light with projection mask, cheap shadows", "DepthShadows.hlsl",
     {"UseShadowProjectionMask": (None, 1), "UseCheapShadows": (None, 1), "NVIDIA_DX10_SHADOW_OPTS": (None, 1)}, "all"),
    ("light without projection mask, full shadows", "DepthShadows.hlsl",
     {"UseShadowProjectionMask": (None, 0), "UseCheapShadows": (None, 0), "NVIDIA_DX10_SHADOW_OPTS": (None, 1)}, "all"),
    ("light with projection mask, full shadows", "DepthShadows.hlsl",
     {"UseShadowProjectionMask": (None, 1), "UseCheapShadows": (None, 0), "NVIDIA_DX10_SHADOW_OPTS": (None, 1)}, "all"),
    # A texture used as a material (FMaterialFactory_BitmapMaterial, sub_10989C00): its alpha (+164 & 3) adds the same
    # texture as the opacity map; either state for every bitmap.
    ("bitmap with alpha", "MaterialFactory_BitmapMaterial", {"OpacityBind.samplerSlot": (0, 1)}, "all"),
    ("bitmap without alpha", "MaterialFactory_BitmapMaterial", {"OpacityBind.samplerSlot": (1, 0)}, "all"),
    # A mesh section draws its light layers without the base pass when its material is unlit (no diffuse map and no
    # specular: vtable+244) or takes the specular cube map (vtable+260): sub_10BAE6A0 / sub_10BACA20.  The cache has
    # such materials only where another material with the same literals was lit; every layer count 1..6 (more than
    # three layers composite them, sub_109B2C10), as the level's precache asks.
] + [
    ("light layers without the base pass, %d layers" % n, "LayerLighting.hlslMaterialFactory_Shader",
     {"RENDER_BASE_PASS": (None, 0), "MAX_INCIDENT_LIGHTS": (None, n), "USING_COMPOSITE_LAYERS": (None, int(n > 3))},
     "base-less") for n in range(1, 7)
] + [
    # Sorted translucent objects draw with UsePerVertexOpacity 1 (sub_10BB4210: SetPostPixelShaderParams(1, 1, 1.0));
    # the cache has none.  Blended materials only (Shader OutputBlending != 0, from the packages; materials.py).
    ("per-vertex opacity of sorted translucent objects", "GlobalLitLighting",
     {"ENABLE_PER_VERTEX_OPACITY": (0, 1)}, "blended"),
] + [
    # The blur kernels' sample counts follow the size of the render target (sub_10D4F030: NUM_SAMPLES in 5 bits), so a
    # phone's resolution and render scale ask for counts the PC never used: every count for every shipped blur.
    ("blur sample counts", "ImageSpace.hlslBlur", {"NUM_SAMPLES": (None, n)}, "all") for n in range(1, 32)
]
SCENE_FIELDS = {"MAX_INCIDENT_LIGHTS", "PER_VERTEX_LUMINANCE", "USES_SPECULAR_CUBEMAP", "USING_COMPOSITE_LAYERS",
                "NVIDIA_DX10_SHADOW_OPTS", "USE_VSM_SHADOWS"}


def _material(values):
    return tuple(v for n, k, v in values if k != "define" and n != HIDDEN)


def _apply(values, slot, changes):
    """The twin's values and slot width, or None when the rule does not apply.  Dropping the only texture of a
    material leaves its slot width at 0 (sub_10989AE0: max(1, bit length of the texture count), 0 without textures)."""
    present = {n: v for n, k, v in values}
    for field, (before, after) in changes.items():
        if field not in present or (before is not None and present[field] != before):
            return None
    out = [(n, k, changes[n][1] if n in changes else v) for n, k, v in values]
    if any(n.endswith(".samplerSlot") for n in changes):
        # The indices still in use must be 1..k (the engine numbers textures by first use; a gap would renumber the
        # later ones, which is not modelled).  A slot that takes the index of another slot shares its texture.
        used = sorted({v for n, k, v in out if n.endswith(".samplerSlot") and v})
        if used != list(range(1, len(used) + 1)):
            return None
        slot = max(1, len(used).bit_length()) if used else 0
    return out, slot


CROSS_PASSES = [("GlobalLitLighting.hlsl", "global lighting of materials the cache drew unlit only"),
                ("PerPixelLighting.hlsl", "per-pixel lights of materials the cache drew unlit only")]
BLENDED_FACTORIES = ("MaterialFactory_Facing", "MaterialFactory_WindowShader", "MaterialFactory_BitmapMaterial")


def _blended(name, values, blended):
    """A record a sorted translucent object can draw with: a blended Shader (by its factory literals), a vertex shader
    (no material literals to tell), or a translucent-capable factory; never alpha tested."""
    got = {n: v for n, k, v in values}
    if got.get("ENABLE_ALPHATEST", 0):
        return False
    if name.endswith("VSMain"):
        return True
    if "MaterialFactory_Shader" in name:
        key = materials.factory_key(values)
        return key is not None and key in blended
    return any(f in name for f in BLENDED_FACTORIES)


def _binds(*names):
    out = []
    for n in names:
        mask = n.endswith("*")
        n = n.rstrip("*")
        out.append([n + ".samplerSlot", "int"])
        if mask:
            out.append([n + ".channelID", "int"])
    return out


# Factory writers of names no trace reached (their layouts cannot be learned): the factory's part in writer order,
# read from Bioshock.exe.  FMaterialFactory_FluidSurface: sub_1098F270 (DUAL_SPECULAR is a define among the literals;
# UseRealtimeReflection is the material's +144 bit 0).
FACTORY_WRITERS = {
    "MaterialFactory_FluidSurface": {
        "PSMain": _binds("FluidDiffuseBind", "FluidNormalMapBind", "CoverageMaskBind*", "FluidSpecularMaskBind*",
                         "FluidSpecularColorMapBind", "FluidGlossinessMaskBind*", "FluidReflectionMaskBind*",
                         "SurfaceDiffuseBind", "SurfaceNormalMapBind", "SurfaceSpecularMaskBind*",
                         "SurfaceSpecularColorMapBind", "SurfaceGlossinessMaskBind*")
        + [["DUAL_SPECULAR", "define"], ["WaterUseXGBRNormal", "bool"], ["SurfaceUseXGBRNormal", "bool"],
           ["UseRealtimeReflection", "bool"], ["UseSpecular", "bool"]],
        "VSMain": [["DUAL_SPECULAR", "define"]],
    },
    # The fog volume factories record nothing of their own (MaterialFactory_FogApply/FogBlend.hlsl have no literals):
    # their keys are the pass's part alone.
    "MaterialFactory_FogApply": {"PSMain": [], "VSMain": []},
    "MaterialFactory_FogBlend": {"PSMain": [], "VSMain": []},
}


def composed_layouts(layouts, names):
    """Layouts for the names of FACTORY_WRITERS that the trace never reached: the factory's part from the writer, the
    pass's part from a learned name of the same pass, vertex factory and entry with another factory (its factory part
    ends where it stops agreeing with the same factory in another pass).  The proof by compilation decides whether a
    composed layout is right."""
    out = dict(layouts)
    for name in names:
        if name in out:
            continue
        parts = name.split(".hlsl")
        if len(parts) != 4 or parts[1] not in FACTORY_WRITERS or parts[3] not in FACTORY_WRITERS[parts[1]]:
            continue
        own = FACTORY_WRITERS[parts[1]][parts[3]]
        for other, lay in sorted(layouts.items()):
            o = other.split(".hlsl")
            if len(o) != 4 or o[0] != parts[0] or o[2:] != parts[2:] or o[1] == parts[1]:
                continue
            fields = [tuple(f) for f in lay["shapes"][0]["fields"]]
            sibling = [tuple(f) for n, l in layouts.items() for f in [l["shapes"][0]["fields"]]
                       if n.split(".hlsl")[1:] == o[1:] and n.split(".hlsl")[0] != o[0]]
            if not sibling:
                continue
            common = 0
            while common < min(len(fields), len(sibling[0])) and fields[common] == tuple(sibling[0][common]):
                common += 1
            if not common:
                continue
            factory_files = [p for p in lay["pairs"] if p[0] == "MATERIALFACTORY"]
            pairs = [["MATERIALFACTORY", parts[1] + ".hlsl"] if p[0] == "MATERIALFACTORY" else p for p in lay["pairs"]]
            if not factory_files:
                continue
            out[name] = {"shapes": [{"fields": [list(f) for f in own] + [list(f) for f in fields[common:]],
                                     "samples": 0, "fit": 0.0}],
                         "pass": lay["pass"], "pairs": pairs, "entry": lay["entry"], "profile": lay["profile"],
                         "samples": 0, "composed_from": other}
            break
    return out


def shipped_variants(records, layouts, pool, material_summary=None):
    """records: [(name, GUID words, time stamp, DXBC)] of ShaderCache.pcs10.  Every record is decoded and proven
    (compiled with the game's compiler to its own DXBC byte for byte), then the RULES make its twins.  Returns
    (proven, twins, report): proven (name, profile, defines, time stamp, DXBC) of shipped records, twins (name, GUID
    words, time stamp, DXBC, profile, defines, rule)."""
    layouts = composed_layouts(layouts, {name for name, words, stamp, code in records})
    shipped = collections.defaultdict(set)
    by_name = collections.defaultdict(list)
    for name, words, stamp, code in records:
        guid = words[0] | words[1] << 32 | words[2] << 64 | words[3] << 96
        shipped[name].add(guid)
        if name in layouts:
            by_name[name].append((guid, stamp, code))
    report = collections.Counter()
    proofs, plans = [], []
    for name, items in by_name.items():
        lay = layouts[name]
        shapes = [[tuple(x) for x in s["fields"]] for s in lay["shapes"]]
        for guid, stamp, code in items:
            for slot, values in (r for layout in shapes for r in decode(guid, layout)):
                job = (lay["pass"], [tuple(p) for p in lay["pairs"]], lay["entry"], lay["profile"], define_list(values))
                proofs.append(job)
                plans.append((name, guid, stamp, code, slot, values, job))
    report["records with a layout"] = sum(len(v) for v in by_name.values())
    report["readings compiled"] = len(proofs)
    compiled, errors = pool.run(proofs)
    report["readings that do not compile"] = len(errors)
    proven = {}
    for name, guid, stamp, code, slot, values, job in plans:
        dxbc = compiled.get(pool.key(*job))
        if dxbc is not None and dxbc == code:
            proven[name, guid] = (stamp, code, slot, values, job)
    report["records decoded and proven"] = len(proven)
    if material_summary is None and any(who in ("blended", "base-less", "cube") for label, needle, changes, who in RULES):
        material_summary = materials.summary()
    tuples = (material_summary or {}).get("tuples", {})
    blended = {k for k, t in tuples.items() if t.get("blended")}
    baseless = {k for k, t in tuples.items() if t.get("unlit") or t.get("cube")}
    cube = {k for k, t in tuples.items() if t.get("cube")}
    evidence = collections.defaultdict(set)   # (name, field, value) -> materials
    for (name, guid), (stamp, code, slot, values, job) in proven.items():
        for n, k, v in values:
            evidence[name, n, v].add(_material(values))
    twin_jobs, twin_meta, planned = [], [], set()
    for (name, guid), (stamp, code, slot, values, job) in proven.items():
        lay = layouts[name]
        for label, needle, changes, who in RULES:
            if needle not in name:
                continue
            applied = _apply(values, slot, changes)
            if applied is None:
                continue
            twin, tslot = applied
            if who == "blended" and not _blended(name, twin, blended):
                continue
            if who == "base-less" and materials.factory_key(twin) not in baseless:
                continue
            if who == "cube":
                # A Shader takes the cube map by its own properties (vtable+260, from the packages); other factories
                # (water, windows) by the cache's evidence of the material with the cube map on.
                key = materials.factory_key(twin)
                if key is not None:
                    if key not in cube:
                        continue
                elif not all(_material(values) in evidence[name, f, after] for f, (before, after) in changes.items()):
                    continue
            if who == "evidence" and not all(_material(values) in evidence[name, f, after]
                                             for f, (before, after) in changes.items()):
                continue
            tguid = encode(twin, tslot)
            if tguid is None or tguid in shipped[name]:
                report["twins already shipped"] += 1
                continue
            if (name, tguid) in planned:
                continue
            planned.add((name, tguid))
            tjob = (lay["pass"], [tuple(p) for p in lay["pairs"]], lay["entry"], lay["profile"], define_list(twin))
            twin_jobs.append(tjob)
            twin_meta.append((name, tguid, stamp, code, tjob, label))

    def plan(name, values, slot, stamp, code, label):
        lay = layouts[name]
        tguid = encode(values, slot)
        if tguid is None or tguid in shipped[name] or (name, tguid) in planned:
            return
        planned.add((name, tguid))
        tjob = (lay["pass"], [tuple(p) for p in lay["pairs"]], lay["entry"], lay["profile"], define_list(values))
        twin_jobs.append(tjob)
        twin_meta.append((name, tguid, stamp, code, tjob, label))

    # Lit passes of materials the cache only drew unlit: any lit primitive takes the global lighting and per-pixel
    # light passes, whatever material it wears.  The material's part of the key (the fields both names share in front)
    # goes with every pass part the lit name has.
    by_pass = collections.defaultdict(list)
    for (name, guid), item in proven.items():
        by_pass[name].append(item)
    for source in [n for n in by_pass if n.startswith("Base.hlsl")]:
        for target_pass, label in CROSS_PASSES:
            target = target_pass + source[len("Base.hlsl"):]
            if target not in by_pass or target not in layouts:
                continue
            tfields = [tuple(f) for f in layouts[target]["shapes"][0]["fields"]]
            sfields = [tuple(f) for f in layouts[source]["shapes"][0]["fields"]]
            common = 0
            while common < min(len(tfields), len(sfields)) and tfields[common] == sfields[common]:
                common += 1
            if not common:
                continue
            def shaped(items, fields_):
                return [it for it in items if [(n, k) for n, k, v in it[3]] == list(fields_)]
            seen_materials = {tuple(v for n, k, v in values[:common]) for stamp, code, slot, values, job in by_pass[target]}
            parts = {tuple(values[common:]) for stamp, code, slot, values, job in shaped(by_pass[target], tfields)}
            tstamp = by_pass[target][0][0]
            for stamp, code, slot, values, job in shaped(by_pass[source], sfields):
                head = tuple(values[:common])
                if tuple(v for n, k, v in head) in seen_materials:
                    continue
                for part in sorted(parts):
                    plan(target, list(head) + list(part), slot, tstamp, None, label)
    # The ripple system's materials (Engine.U), absent from the cache: the ripple pass draws them with a fixed state.
    if material_summary is None and any(n.startswith("Ripples.hlslMaterialFactory_RippleShader") for n in layouts):
        material_summary = materials.summary()
    for ripple in (material_summary or {}).get("ripples", []):
        for name in [n for n in layouts if n.startswith("Ripples.hlslMaterialFactory_RippleShader")]:
            fields_ = [tuple(f) for f in layouts[name]["shapes"][0]["fields"]]
            values = [(n, k, ripple.get(n, 1 if n == "USE_MATERIAL_FACTORY" else 0)) for n, k in fields_]
            plan(name, values, ripple["slot width"], None, None, "ripple system materials")
    compiled, errors = pool.run(twin_jobs)
    report["twins that do not compile"] = len(errors)
    twins = []
    for name, tguid, stamp, code, tjob, label in twin_meta:
        dxbc = compiled.get(pool.key(*tjob))
        if dxbc is None:
            continue
        words = tuple((tguid >> (32 * i)) & 0xFFFFFFFF for i in range(4))
        twins.append((name, words, stamp, dxbc, tjob[3], tjob[4], label))
        report["twins: " + label] += 1
        report["twins with the base's code" if code is not None and dxbc == code else "twins with code of their own"] += 1
    shipped_out = [(name, job[3], job[4], stamp, code) for (name, guid), (stamp, code, slot, values, job) in proven.items()]
    return shipped_out, twins, dict(report)

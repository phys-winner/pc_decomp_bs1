"""The game's materials read from its packages, as Bioshock.exe turns them into shader keys.

FMaterialFactory_Shader (UShader, MaterialType 9): the init sub_10989EB0 gives each texture slot a sampler index in
the order below (sub_10989AE0: a reference resolved through the material's vtable+228 takes a new index when it is a
bitmap material, MaterialType 0x19..0x20, and keeps the index of an earlier slot with the same reference; the slot
width is max(1, bit length of the count), 0 without textures), channels through sub_10D58210 (0->3, 1->0, 2->1,
3->2); the literal writer sub_1098A190 adds UseXGBRNormal (the normal map's format is 12, DXT5 with X in G:
sub_1099C2E0) and UseSpecular (the pass allows it and Glossiness, SpecularBrightness >= 0.01). The script layout of
Shader in Engine.U puts every property at the offset the C++ reads (+120 Diffuse ... +288 ClipMask).

Checked on 21 November 2026 against the shipped cache and devices: the 9211 Shaders of the maps and script packages
give 1164 literal tuples, which hold all 625 tuples a 15-level device trace asked for and 1162 of the 1176 tuples of
ShaderCache.pcs10 (the other 14 are materials the cache was built from before they changed).

    python tools/shaderlib10/materials.py      # summary
"""
import collections
import hashlib
import itertools
import json
import os
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import packages  # noqa: E402

GAME = Path(os.environ.get("BIOSHOCK_GAME", r"C:\Games\BioShock"))
TEXTURE_CLASSES = {"Texture": 29, "Cubemap": 30, "BitmapMaterial": 26}   # MaterialType from the class defaults
SLOTS = [("DiffuseBind", "Diffuse", False), ("OpacityBind", "Opacity", True), ("SubsurfaceBind", "Subsurface", False),
         ("SubsurfaceMaskBind", "SubsurfaceMask", True), ("NormalMapBind", "NormalMap", False),
         ("SelfIlluminationBind", "Emissive", False), ("SelfIlluminationMaskBind", "EmissiveMask", True),
         ("EnvironmentMapBind", "ReflectionCubemap", False), ("EnvironmentMapMaskBind", "ReflectionMask", True),
         ("SpecularMaskBind", "SpecularMask", True), ("SpecularColorMapBind", "SpecularColorMap", False),
         ("GlossinessMaskBind", "GlossinessMask", True), ("ClipMaskBind", "ClipMask", True),
         ("HeightMapBind", "HeightMap", True)]
CHANNEL = {0: 3, 1: 0, 2: 1, 3: 2}
ANIMATORS = ("DiffuseTextureAnimator", "DiffuseColorAnimator", "OpacityTextureAnimator", "SelfIllumTextureAnimator",
             "SelfIllumColorAnimator")
FACTORY_FIELDS = [b + ".samplerSlot" if i == 0 else b + ".channelID"
                  for b, prop, mask in SLOTS for i in ((0, 1) if mask else (0,))] + ["UseXGBRNormal", "UseSpecular"]


def package_paths():
    return sorted((GAME / "Content/Maps").glob("*.bsm")) + sorted((GAME / "Builds/Release").glob("*.U"))


class World:
    """Every export of every package; a reference (class, name) resolves in its own package first."""

    def __init__(self, paths=None):
        self.packages = []
        self.by_name = collections.defaultdict(list)
        for path in paths or package_paths():
            p = packages.load(path)
            k = len(self.packages)
            self.packages.append((Path(path), p))
            for i, e in enumerate(p["exports"], 1):
                if e["size"]:
                    self.by_name[(e["class"], e["name"].lower())].append((k, i))
        self._props = {}

    def export(self, ref):
        return self.packages[ref[0]][1]["exports"][ref[1] - 1]

    def props(self, ref):
        if ref not in self._props:
            self._props[ref] = packages.properties(self.packages[ref[0]][1], self.export(ref))
        return self._props[ref]

    def objects(self, cls):
        for k, (path, p) in enumerate(self.packages):
            for i, e in enumerate(p["exports"], 1):
                if e["class"] == cls and e["size"]:
                    yield (k, i)

    def resolve(self, ref, value):
        if value is None:
            return None
        cands = self.by_name.get((value[0], value[1].lower()), [])
        for c in cands:
            if c[0] == ref[0]:
                return c
        return cands[0] if cands else ("missing",) + tuple(value)


def _resolutions(world, ref, depth=0):
    """What a slot reference can resolve to at some time: a bitmap material, or None (any other material resolves to
    itself and is not one).  MaterialSequence: any frame; MaterialSwitch: any of its materials."""
    if ref is None:
        return {None}
    if ref[0] == "missing":
        return {ref} if ref[1] in TEXTURE_CLASSES else {None}
    cls = world.export(ref)["class"]
    if cls in TEXTURE_CLASSES:
        return {ref}
    if depth > 4:
        return {None}
    out = set()
    if cls == "MaterialSequence":
        for item in world.props(ref).get("SequenceItems", []):
            out |= _resolutions(world, world.resolve(ref, item.get("Material")), depth + 1)
    elif cls == "MaterialSwitch":
        mats = world.props(ref).get("Materials", [])
        for m in mats if isinstance(mats, list) else []:
            out |= _resolutions(world, world.resolve(ref, m), depth + 1)
    return out or {None}


def shader_tuples(world, ref):
    """{(slot width, factory values in FACTORY_FIELDS order)} a Shader can key, over its dynamic slot resolutions and
    both UseSpecular states (the pass decides whether specular is allowed)."""
    d = world.props(ref)
    slots = []
    for bind, prop, mask in SLOTS:
        v = d.get(prop)
        if mask:
            target = world.resolve(ref, v.get("Material")) if isinstance(v, dict) else None
            channel = CHANNEL.get(v.get("Channel", 0) if isinstance(v, dict) else 0, 0)
        else:
            target = world.resolve(ref, v)
            channel = None
        slots.append((bind, target, channel))
    refs = sorted({t for b, t, c in slots if t is not None}, key=repr)
    out = set()
    specular = d.get("Glossiness", 0.0) >= 0.01 and d.get("SpecularBrightness", 0.0) >= 0.01
    for combo in itertools.product(*[sorted(_resolutions(world, t), key=repr) for t in refs]):
        chosen = dict(zip(refs, combo))
        index, values, normal = {}, [], None
        for bind, target, channel in slots:
            slot = 0
            if target is not None and chosen[target] is not None:
                slot = index.setdefault(target, len(index) + 1)
                if bind == "NormalMapBind":
                    normal = chosen[target]
            values.append(slot)
            if channel is not None:
                values.append(channel)
        width = max(1, len(index).bit_length()) if index else 0
        fmt = world.props(normal).get("Format", 0) if normal is not None and normal[0] != "missing" else None
        for spec in {0, 1 if specular else 0}:
            out.add((width, tuple(values) + (1 if fmt == 12 else 0, spec)))
    return out


def summary(world=None):
    """{"tuples": {factory values: {"blended": bool, "count": n}}} of all Shaders, cached in build/render10/shaderlib
    by the packages' sizes and times."""
    root = HERE.parents[1]
    key = hashlib.sha256(json.dumps([(str(p), p.stat().st_size, p.stat().st_mtime_ns) for p in package_paths()]
                                    + [hashlib.sha256(Path(__file__).read_bytes()).hexdigest()]).encode()).hexdigest()
    cache = root / "build/render10/shaderlib" / ("materials-%s.json" % key[:16])
    if cache.exists():
        return json.loads(cache.read_text(encoding="utf-8"))
    world = world or World()
    tuples = {}
    for ref in world.objects("Shader"):
        d = world.props(ref)
        blended = d.get("OutputBlending", 0) != 0                                   # vtable+240 (sub_10A7BBB0)
        specular = d.get("SpecularBrightness", 0.0) >= 0.01 and d.get("Glossiness", 0.0) >= 0.01
        unlit = d.get("Diffuse") is None and not specular                         # !vtable+244 (sub_10A7EAD0)
        cube = specular and bool(d.get("UseSpecularCubemaps"))                     # vtable+260 (sub_10A7EB80)
        for width, values in shader_tuples(world, ref):
            k = ",".join(map(str, values))
            t = tuples.setdefault(k, {"blended": False, "unlit": False, "cube": False, "count": 0})
            t["blended"] |= blended
            t["unlit"] |= unlit
            t["cube"] |= cube
            t["count"] += 1
    data = {"fields": FACTORY_FIELDS, "tuples": tuples, "ripples": ripple_materials(world)}
    cache.parent.mkdir(parents=True, exist_ok=True)
    cache.write_bytes(json.dumps(data, sort_keys=True).encode("utf-8"))
    return data


def ripple_materials(world):
    """The literals of the ripple system's materials (RippleShader, Engine.U: RadialRippleMaterial and
    DirectionalRippleMaterial) for MaterialFactory_RippleShader: the normal, offset, diffuse and diffuse mask maps take
    sampler indices 1..4 in that order (as the devices' requests show), channels as for Shader, the two XGBR flags
    from the normal and offset maps' formats, FlipNormalUVCoordinates from NormalMapFlipUV."""
    out = []
    for ref in world.objects("RippleShader"):
        d = world.props(ref)
        index, values = {}, {}
        for bind, prop, mask in (("NormalMapBind", "NormalMap", False), ("OffsetMapBind", "OffsetMap", False),
                                 ("DiffuseMapBind", "DiffuseMap", True), ("DiffuseMaskBind", "DiffuseMask", True)):
            v = d.get(prop)
            target = world.resolve(ref, v.get("Material") if mask and isinstance(v, dict) else v)
            values[bind + ".samplerSlot"] = index.setdefault(target, len(index) + 1) if target is not None else 0
            if mask:
                values[bind + ".channelID"] = CHANNEL.get(v.get("Channel", 0) if isinstance(v, dict) else 0, 0)

        def xgbr(prop):
            target = world.resolve(ref, d.get(prop))
            return 1 if target is not None and target[0] != "missing" and world.props(target).get("Format") == 12 else 0
        values["UseXGBRNormal"] = xgbr("NormalMap")
        values["UseOffsetXGBRNormal"] = xgbr("OffsetMap")
        values["FlipNormalUVCoordinates"] = 1 if d.get("NormalMapFlipUV") else 0
        values["slot width"] = max(1, len(index).bit_length()) if index else 0
        values["material"] = world.export(ref)["name"]
        out.append(values)
    return out


def factory_key(values):
    """The FACTORY_FIELDS key of a record's recorded fields [(name, kind, value)], or None for another factory."""
    got = {n: v for n, k, v in values}
    if any(f not in got for f in FACTORY_FIELDS):
        return None
    return ",".join(str(got[f]) for f in FACTORY_FIELDS)


if __name__ == "__main__":
    s = summary()
    print(len(s["tuples"]), "Shader literal tuples,", sum(1 for t in s["tuples"].values() if t["blended"]), "blended")

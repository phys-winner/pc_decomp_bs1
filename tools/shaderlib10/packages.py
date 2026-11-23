"""Tagged properties of the exports of BioShock PC packages (maps *.bsm, scripts *.U), read only.

The package container (names, imports, exports, compressed blocks) is tools/bioshock_textures.py package(); this adds
the property stream of an export: tagged properties up to None, with object references resolved to (class, name).
Two quirks of the format: a struct value that is itself tagged declares a size without the size bytes of its own
members (it is parsed in place, never skipped by size), and the same holds for arrays of tagged structs (only the
arrays named in STRUCT_ARRAYS are read as such; other arrays keep their raw bytes).
"""
import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import bioshock_textures as bt  # noqa: E402

KINDS = {1: "byte", 2: "int", 3: "bool", 4: "float", 5: "object", 6: "name", 7: "delegate", 8: "class", 9: "array",
         10: "struct", 11: "vector", 12: "rotator", 13: "str", 14: "map", 15: "fixedarray"}
STRUCT_ARRAYS = {"SequenceItems"}
BINARY_STRUCTS = {"Vector", "Rotator", "Color", "Plane", "Quat", "Coords", "Box", "Sphere", "Matrix", "Scale",
                  "IntPoint", "FloatPoint", "Guid"}


def load(path):
    """The package with its import table ({name, cls, outer} per import)."""
    p, sha = bt.package(Path(path))
    p["imports"] = _imports(p)
    p["sha256"] = sha
    return p


def _imports(p):
    data, names = p["data"], p["names"]
    nc, no, ec, eo, ic, io = struct.unpack_from("<6I", data, 12)
    r = bt.Reader(data, io)

    def fname():
        index, number = r.compact(), r.u32()
        return names[index] + (str(number - 1) if number else "")
    out = []
    for _ in range(ic):
        class_package, cls = fname(), fname()
        outer = struct.unpack("<i", r.take(4))[0]
        out.append(dict(name=fname(), cls=cls, outer=outer))
    return out


def objref(p, index):
    """(class, name) of an object reference: negative = import, positive = export, 0 = None."""
    if index == 0:
        return None
    if index < 0:
        imp = p["imports"][-index - 1]
        return (imp["cls"], imp["name"])
    exp = p["exports"][index - 1]
    return (exp["class"], exp["name"])


class Array(list):
    """An array of tagged structs (each element a property list)."""


def tagged(p, r, limit=4096):
    """[(name, element, kind, struct name, value)] from a reader up to None."""
    out = []
    for _ in range(limit):
        index, number = r.compact(), r.u32()
        bt.require(0 <= index < len(p["names"]), "invalid property name")
        name = p["names"][index] + (str(number - 1) if number else "")
        if name == "None":
            return out
        info = r.take(1)[0]
        kind, code = info & 15, info >> 4 & 7
        sname = None
        if kind == 10:
            si, sn = r.compact(), r.u32()
            bt.require(0 <= si < len(p["names"]), "invalid struct name")
            sname = p["names"][si]
        size = [1, 2, 4, 12, 16][code] if code < 5 else r.take(1)[0] if code == 5 else \
            struct.unpack("<H", r.take(2))[0] if code == 6 else r.u32()
        element = r.compact() if info & 128 and kind != 3 else 0
        if kind == 3:
            value = bool(info & 128)
        elif kind == 10 and sname not in BINARY_STRUCTS:
            value = tagged(p, r)
        elif kind == 9 and name in STRUCT_ARRAYS:
            value = _struct_array(p, r, size)
        else:
            value = r.take(size)
        out.append((name, element, KINDS.get(kind, kind), sname, value))
    raise ValueError("property count exceeds bound")


def _struct_array(p, r, size):
    start = r.position
    try:
        count = r.compact()
        if 0 < count <= 4096:
            items = Array(tagged(p, r, limit=256) for _ in range(count))
            if all(items):
                return items
    except (ValueError, IndexError):
        pass
    r.position = start
    return r.take(size)


def properties(p, export):
    """{name: value} of an export: numbers, bools, (class, name) references, nested dicts for tagged structs, lists
    for arrays of structs or references (raw hex otherwise); element > 0 of a static array as "name[i]"."""
    if not export["size"]:
        return {}
    raw = p["data"][export["offset"]:export["offset"] + export["size"]]
    r = bt.Reader(raw)
    r.u32(), r.u32()   # serialization versions (4, 3), as texture() checks
    return _to_dict(p, tagged(p, r))


def _to_dict(p, out):
    d = {}
    for name, el, kind, sname, raw in out:
        key = name if not el else "%s[%d]" % (name, el)
        if isinstance(raw, Array):
            d[key] = [_to_dict(p, item) for item in raw]
        elif isinstance(raw, list):
            d[key] = _to_dict(p, raw)
        elif kind == "array":
            d[key] = _references(p, raw)
        else:
            d[key] = _value(p, kind, raw)
    return d


def _references(p, raw):
    try:
        r = bt.Reader(raw)
        count = r.compact()
        refs = [objref(p, r.compact()) for _ in range(count)]
        if r.position == len(raw):
            return refs
    except (ValueError, IndexError):
        pass
    return raw.hex()


def _value(p, kind, raw):
    if kind == "bool":
        return raw
    if kind == "byte":
        return raw[0] if len(raw) == 1 else raw.hex()
    if kind == "int":
        return struct.unpack("<i", raw)[0] if len(raw) == 4 else raw.hex()
    if kind == "float":
        return struct.unpack("<f", raw)[0] if len(raw) == 4 else raw.hex()
    if kind in ("object", "class"):
        return objref(p, bt.Reader(raw).compact())
    if kind == "name":
        rr = bt.Reader(raw)
        return p["names"][rr.compact()]
    return raw.hex()

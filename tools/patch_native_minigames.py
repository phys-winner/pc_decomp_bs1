"""Give original localized mini-game widgets larger invisible finger targets."""
import copy
import hashlib
import json
import math
from pathlib import Path
import struct
import subprocess
import tempfile
import xml.etree.ElementTree as ET
import zlib

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "work/localization/text/Content/FlashMovies/ComboLockPC.swf"
SOURCE_SHA256 = "0853a49648906f7ae4a490e0c718d7448eca1fa336c61fb5005246b3aa3028f8"
ASSET = "native-combolock.swf"
TARGETS = (7, 20, 21, *range(31, 39))
PLASMINOW_SOURCE = ROOT / "work/localization/text/Content/FlashMovies/PlasmiNowPC.swf"
PLASMINOW_SOURCE_SHA256 = "176e2cc224bac393f9944b1141673580722f4ce2b824f244fdf46ee2e3bd9a02"
PLASMINOW_ASSET = "native-plasminow.swf"


def digest(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


class Bits:
    def __init__(self, data, offset=0):
        self.data, self.bit = data, offset * 8

    def u(self, count):
        if self.bit + count > len(self.data) * 8:
            raise ValueError("Truncated SWF bit field")
        result = 0
        for _ in range(count):
            result = result * 2 + ((self.data[self.bit // 8] >> (7 - self.bit % 8)) & 1)
            self.bit += 1
        return result

    def s(self, count):
        value = self.u(count)
        return value - (1 << count) if count and value & (1 << (count - 1)) else value

    def end(self):
        return (self.bit + 7) // 8


def rect(data, offset):
    bits = Bits(data, offset)
    size = bits.u(5)
    values = tuple(bits.s(size) for _ in range(4))
    return values, bits.end()


def matrix(data, offset):
    bits = Bits(data, offset)
    sx = sy = 65536
    if bits.u(1):
        size = bits.u(5); sx, sy = bits.s(size), bits.s(size)
    rx = ry = 0
    if bits.u(1):
        size = bits.u(5); rx, ry = bits.s(size), bits.s(size)
    size = bits.u(5)
    tx, ty = bits.s(size), bits.s(size)
    return (sx / 65536, sy / 65536, rx / 65536, ry / 65536, tx, ty), bits.end()


def color_end(data, offset):
    bits = Bits(data, offset)
    add, mult, size = bits.u(1), bits.u(1), bits.u(4)
    for _ in range(4 * (add + mult)):
        bits.s(size)
    return bits.end()


def buttons(payload):
    if len(payload) < 6:
        raise ValueError("Truncated DefineButton2")
    action = struct.unpack_from("<H", payload, 3)[0]
    position, records = 5, []
    while payload[position]:
        start = position; flags = payload[position]; position += 1
        if flags & 0xF0:
            raise ValueError("Pinned button unexpectedly uses filters/blending")
        character, depth = struct.unpack_from("<HH", payload, position); position += 4
        transform, end = matrix(payload, position)
        matrix_start, matrix_end = position, end
        position = color_end(payload, end)
        records.append(dict(flags=flags, character=character, depth=depth,
                            matrix=transform, raw=payload[start:position],
                            matrix_relative=(matrix_start - start, matrix_end - start)))
    position += 1
    if action and position != 3 + action:
        raise ValueError("Unexpected DefineButton2 ActionOffset")
    return records, payload[position:]


def parse(data):
    if data[:3] == b"CWS":
        data = b"FWS" + data[3:8] + zlib.decompress(data[8:])
    if data[:3] != b"FWS" or struct.unpack_from("<I", data, 4)[0] != len(data):
        raise ValueError("Invalid SWF header/length")
    bounds, position = rect(data, 8)
    position += 4
    header, tags = data[:position], []
    while position < len(data):
        start = position
        value = struct.unpack_from("<H", data, position)[0]; position += 2
        code, length = value >> 6, value & 63
        long = length == 63
        if long:
            length = struct.unpack_from("<I", data, position)[0]; position += 4
        end = position + length
        if end > len(data):
            raise ValueError("Truncated SWF tag")
        tags.append(dict(code=code, payload=data[position:end], raw=data[start:end], long=long))
        position = end
    if not tags or tags[-1]["code"] != 0:
        raise ValueError("Missing SWF End tag")
    return header, bounds, tags


def tag_bytes(code, payload, long=False):
    if long or len(payload) >= 63:
        return struct.pack("<HI", code * 64 + 63, len(payload)) + payload
    return struct.pack("<H", code * 64 + len(payload)) + payload


def placed(tag):
    payload = tag["payload"]
    if tag["code"] != 26 or not payload[0] & 2:
        return None
    depth, character = struct.unpack_from("<HH", payload, 1)
    if not payload[0] & 4:
        raise ValueError("Pinned root placement lacks matrix")
    transform, end = matrix(payload, 5)
    return dict(character=character, depth=depth, matrix=transform, end=end)


def transformed(bounds, *transforms):
    points = [(bounds[x], bounds[y]) for x in (0, 1) for y in (2, 3)]
    for sx, sy, rx, ry, tx, ty in transforms:
        points = [(x * sx + y * rx + tx, x * ry + y * sy + ty) for x, y in points]
    return [min(x for x, y in points) / 20, max(x for x, y in points) / 20,
            min(y for x, y in points) / 20, max(y for x, y in points) / 20]


def geometry(data, require_touch=False, targets=TARGETS, invisible_only=True):
    _, stage, tags = parse(data)
    shapes = {struct.unpack_from("<H", t["payload"])[0]: t for t in tags if t["code"] in (2, 22, 32, 83)}
    definitions = {struct.unpack_from("<H", t["payload"])[0]: t for t in tags if t["code"] == 34}
    placements = {}
    for tag in tags:
        if tag["code"] != 26:
            continue
        if tag["payload"][0] & 64:
            raise ValueError("Pinned movie unexpectedly has a root clipping mask")
        place = placed(tag)
        if place and place["character"] in targets:
            if place["character"] in placements:
                raise ValueError("Ambiguous root button placement")
            placements[place["character"]] = place
    result = []
    for ident in targets:
        records, _ = buttons(definitions[ident]["payload"])
        hits = [r for r in records if r["flags"] & 8]
        if len(hits) != 1 or (invisible_only and hits[0]["flags"] != 8):
            raise ValueError("Expected one invisible-only HIT record")
        hit = hits[0]; shape, _ = rect(shapes[hit["character"]]["payload"], 2)
        bounds = transformed(shape, hit["matrix"], placements[ident]["matrix"])
        device = [bounds[0] * 2780 / 1280, bounds[1] * 2780 / 1280,
                  bounds[2] * 1264 / 720, bounds[3] * 1264 / 720]
        width, height = device[1] - device[0], device[3] - device[2]
        if require_touch:
            if not (0 <= bounds[0] < bounds[1] <= 1280 and 0 <= bounds[2] < bounds[3] <= 720):
                raise ValueError("Touch target extends outside the stage")
            if min(width, height) < 132 - 1e-5:
                raise ValueError("Touch target is smaller than132 native pixels")
        result.append(dict(button=ident, hit_shape=hit["character"], movie_bounds=bounds,
                           native_bounds=device, native_size=[width, height]))
    minimum = math.inf
    for index, a in enumerate(result):
        for b in result[index + 1:]:
            x, y = a["native_bounds"], b["native_bounds"]
            dx = max(x[0] - y[1], y[0] - x[1], 0)
            dy = max(x[2] - y[3], y[2] - x[3], 0)
            gap = math.hypot(dx, dy)
            if require_touch and gap <= 0:
                raise ValueError("Touch targets overlap")
            minimum = min(minimum, gap)
    return dict(stage_twips=stage, physical_viewport=[2780, 1264], controls=result,
                minimum_pairwise_clearance_native=minimum,
                transform_scope="Full original1280x720 stage to native2780x1264; wide SetRes root translation0")


def rectangle(template, ident, bounds):
    node = copy.deepcopy(template); node.set("shapeId", str(ident))
    x0, x1, y0, y1 = bounds
    bound = node.find("shapeBounds")
    for key, value in zip(("Xmin", "Xmax", "Ymin", "Ymax"), bounds):
        bound.set(key, str(value))
    bound.set("nbits", str(max(abs(v).bit_length() + 1 for v in bounds)))
    records = node.find("shapes/shapeRecords"); records.clear()
    ET.SubElement(records, "item", dict(type="StyleChangeRecord", fillStyle1="1",
        moveBits=str(max(abs(x1).bit_length(), abs(y1).bit_length()) + 1),
        moveDeltaX=str(x1), moveDeltaY=str(y1), stateFillStyle0="false", stateFillStyle1="true",
        stateLineStyle="false", stateMoveTo="true", stateNewStyles="false"))
    for value, vertical in ((x0 - x1, False), (y0 - y1, True), (x1 - x0, False), (y1 - y0, True)):
        attrs = dict(type="StraightEdgeRecord", generalLineFlag="false", vertLineFlag=str(vertical).lower(),
                     numBits=str(abs(value).bit_length() + 1 - 2))
        attrs["deltaY" if vertical else "deltaX"] = str(value)
        ET.SubElement(records, "item", attrs)
    ET.SubElement(records, "item", dict(type="EndShapeRecord", endOfShape="0"))
    return node


def patch_xml(tree):
    tags = tree.getroot().find("tags")
    defs = {node.get("buttonId", node.get("shapeId", "")): node for node in tags}
    ids = [int(v) for node in tree.getroot().iter() for k, v in node.attrib.items()
           if k in ("shapeId", "buttonId", "spriteId", "fontId", "fontID", "characterId", "characterID") and v.isdecimal()]
    start = max(ids) + 1
    if start + 2 >= 65535:
        raise ValueError("SWF character ID overflow")
    new = [rectangle(defs["5"], start, (-620, 620, -752, 752)),
           rectangle(defs["5"], start + 1, (-2248, 2392, -779, 725)),
           rectangle(defs["5"], start + 2, (-1609, 1771, -1268, 1352))]
    insert = next(i for i, node in enumerate(tags) if node.get("type") == "DefineButton2Tag")
    for index, node in enumerate(new):
        tags.insert(insert + index, node)
    for ident in TARGETS:
        record = [r for r in defs[str(ident)].find("characters") if r.get("buttonStateHitTest") == "true"]
        if len(record) != 1 or any(record[0].get(k) == "true" for k in ("buttonStateUp", "buttonStateOver", "buttonStateDown")):
            raise ValueError("Pinned HIT record is not invisible-only")
        record = record[0]
        record.set("characterId", str(start + (2 if ident == 7 else 1 if ident in (20, 21) else 0)))
        old = record.find("placeMatrix"); record.remove(old)
        record.insert(0, ET.Element("placeMatrix", dict(type="MATRIX", hasRotate="false", hasScale="false",
            nRotateBits="0", nScaleBits="0", nTranslateBits="0", translateX="0", translateY="0")))
    for ident, depth, previous in ((21, 56, 15528), (28, 66, 15341)):
        node = [n for n in tags if n.get("type") == "PlaceObject2Tag" and n.get("characterId") == str(ident) and n.get("depth") == str(depth)]
        if len(node) != 1 or node[0].find("matrix").get("translateY") != str(previous):
            raise ValueError("Pinned Cancel placement changed")
        node[0].find("matrix").set("translateY", str(previous - 2835))
    return tuple(range(start, start + 3))


def assemble(original, candidate, new_ids, targets=TARGETS, relocations=None):
    if relocations is None:
        relocations = {(21, 56): (0, -2835), (28, 66): (0, -2835)}
    header, _, source = parse(original); _, _, staged = parse(candidate)
    staged_buttons = {struct.unpack_from("<H", t["payload"])[0]: t for t in staged if t["code"] == 34}
    staged_places = {(p["character"], p["depth"]): (t, p) for t in staged if t["code"] == 26 and (p := placed(t))}
    shapes = [t["raw"] for t in staged if t["code"] == 2 and struct.unpack_from("<H", t["payload"])[0] in new_ids]
    if len(shapes) != len(new_ids):
        raise ValueError("Missing new invisible hit rectangles")
    output, inserted = [], False
    for tag in source:
        payload = tag["payload"]
        if tag["code"] == 34 and not inserted:
            output.extend(shapes); inserted = True
        if tag["code"] == 34 and (ident := struct.unpack_from("<H", payload)[0]) in targets:
            old_records, actions = buttons(payload)
            new_records, new_actions = buttons(staged_buttons[ident]["payload"])
            if len(old_records) != len(new_records) or actions != new_actions:
                raise ValueError("Button action bytecode changed")
            records = []
            for old, new in zip(old_records, new_records):
                if old["flags"] & 8:
                    if old["flags"] != new["flags"] or old["depth"] != new["depth"]:
                        raise ValueError("Button HIT state/depth changed")
                    records.append(new["raw"])
                else:
                    records.append(old["raw"])
            body = b"".join(records) + b"\0"
            updated = payload[:3] + struct.pack("<H", len(body) + 2 if actions else 0) + body + actions
            output.append(tag_bytes(34, updated, tag["long"]))
        elif tag["code"] == 26 and (place := placed(tag)) and (place["character"], place["depth"]) in relocations:
            other, new_place = staged_places[(place["character"], place["depth"])]
            dx, dy = relocations[(place["character"], place["depth"])]
            expected = (*place["matrix"][:4], place["matrix"][4] + dx, place["matrix"][5] + dy)
            if payload[:5] != other["payload"][:5] or expected != new_place["matrix"]:
                raise ValueError("Cancel changed beyond its approved placement")
            updated = payload[:5] + other["payload"][5:new_place["end"]] + payload[place["end"]:]
            output.append(tag_bytes(26, updated, tag["long"]))
        else:
            output.append(tag["raw"])
    data = header + b"".join(output)
    return data[:4] + struct.pack("<I", len(data)) + data[8:]


def build_plasminow(output):
    """Enlarge the two original targets and move Cancel, preserving its hide scripts."""
    output = Path(output)
    if digest(PLASMINOW_SOURCE) != PLASMINOW_SOURCE_SHA256:
        raise ValueError("Native mini-games require the pinned localized PlasmiNowPC.swf")
    machine = json.loads((Path.home() / ".agents/skills/adapt-arm32-game/machine_config.json").read_text())
    java = Path(machine["java_exe"])
    ffdec = Path(machine["analyzers"]["ffdec_cli"]).with_suffix(".jar")
    identity = dict(source_sha256=PLASMINOW_SOURCE_SHA256, generator_sha256=digest(Path(__file__)), ffdec_sha256=digest(ffdec))
    receipt = output.with_suffix(".json")
    if output.is_file() and receipt.is_file():
        old = json.loads(receipt.read_text())
        if all(old.get(k) == v for k, v in identity.items()) and old.get("sha256") == digest(output):
            return old
    output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="native-plasminow-", dir=output.parent) as temporary:
        stage = Path(temporary)

        def run(*args):
            result = subprocess.run([str(java), "-Xmx512m", "-jar", str(ffdec), "-onerror", "abort", *map(str, args)],
                capture_output=True, text=True, encoding="utf-8", errors="replace", timeout=60)
            if result.returncode:
                raise RuntimeError(result.stdout + result.stderr)

        run("-swf2xml", PLASMINOW_SOURCE, stage / "original.xml")
        tree = ET.parse(stage / "original.xml")
        tags = tree.getroot().find("tags")
        defs = {n.get("buttonId", n.get("shapeId", "")): n for n in tags}
        ids = [int(v) for n in tree.getroot().iter() for k, v in n.attrib.items()
               if k in ("shapeId", "buttonId", "spriteId", "fontId", "fontID", "characterId", "characterID") and v.isdecimal()]
        new_ids = (max(ids) + 1, max(ids) + 2)
        if new_ids[-1] >= 65535:
            raise ValueError("SWF character ID overflow")
        for ident, depth, previous in ((41, 4, (7021, 12609)), (134, 423, (8421, 12636))):
            nodes = [n for n in tags if n.get("type") == "PlaceObject2Tag" and n.get("characterId") == str(ident) and n.get("depth") == str(depth)]
            if len(nodes) != 1:
                raise ValueError("Pinned PlasmiNow Cancel placement changed")
            m = nodes[0].find("matrix")
            if (int(m.get("translateX")), int(m.get("translateY"))) != previous:
                raise ValueError("Pinned PlasmiNow Cancel placement changed")
            m.set("translateX", str(previous[0] + 13200)); m.set("translateY", str(previous[1] + 640))
        insert = next(i for i, n in enumerate(tags) if n.get("type") == "DefineButton2Tag")
        for index, (ident, desired) in enumerate(((40, (200, 445, 630, 705.2)), (41, (860, 1110, 630, 705.2)))):
            place = next(n for n in tags if n.get("type") == "PlaceObject2Tag" and n.get("characterId") == str(ident))
            m = place.find("matrix")
            sx, sy = float(m.get("scaleX")), float(m.get("scaleY"))
            tx, ty = int(m.get("translateX")), int(m.get("translateY"))
            # Integral local twips round outwards so encoded finger targets cannot shrink.
            bounds = (math.floor((desired[0] * 20 - tx) / sx), math.ceil((desired[1] * 20 - tx) / sx),
                      math.floor((desired[2] * 20 - ty) / sy), math.ceil((desired[3] * 20 - ty) / sy))
            tags.insert(insert + index, rectangle(defs["165"], new_ids[index], bounds))
            hit = [h for h in defs[str(ident)].find("characters") if h.get("buttonStateHitTest") == "true"]
            if len(hit) != 1 or any(hit[0].get(k) == "true" for k in ("buttonStateUp", "buttonStateOver", "buttonStateDown")):
                raise ValueError("Pinned PlasmiNow HIT is not invisible-only")
            hit[0].set("characterId", str(new_ids[index]))
        tree.write(stage / "patched.xml", encoding="utf-8", xml_declaration=True)
        run("-xml2swf", stage / "patched.xml", stage / "encoded.swf")
        data = assemble(PLASMINOW_SOURCE.read_bytes(), (stage / "encoded.swf").read_bytes(), new_ids,
                        targets=(40, 41), relocations={(41, 4): (13200, 640), (134, 423): (13200, 640)})
        (stage / "final.swf").write_bytes(data)
        after = geometry(data, True, targets=(40, 41))
        all_controls = geometry(data, targets=(40, 41, 42, 161, 162, 163, 164, 166, 167, 168, 169, 170, 171, 173, 174, 175), invisible_only=False)
        for target in after["controls"]:
            a = target["movie_bounds"]
            if not 160 <= a[0] < a[1] <= 1120:
                raise ValueError("PlasmiNow target extends outside the original 4:3 crop")
            for other in all_controls["controls"]:
                b = other["movie_bounds"]
                if target["button"] != other["button"] and min(a[1], b[1]) > max(a[0], b[0]) and min(a[3], b[3]) > max(a[2], b[2]):
                    raise ValueError("PlasmiNow touch target overlaps another original control")
        exports = []
        for name, source in (("before", PLASMINOW_SOURCE), ("after", stage / "final.swf")):
            run("-export", "script", stage / name, source)
            exports.append({p.relative_to(stage / name).as_posix(): p.read_bytes() for p in (stage / name).rglob("*.as")})
        if len(exports[0]) != 110 or exports[0] != exports[1]:
            raise ValueError("Original PlasmiNow ActionScript changed")
        result = {**identity, "source": PLASMINOW_SOURCE.relative_to(ROOT).as_posix(), "language": "RUS",
            "sha256": hashlib.sha256(data).hexdigest(), "bytes": len(data), "asset": PLASMINOW_ASSET,
            "overlay_path": "Content/FlashMovies/PlasmiNowPC.swf", "action_scripts": 110,
            "action_script_hashes": {name: hashlib.sha256(body).hexdigest() for name, body in exports[0].items()},
            "new_hit_shapes": list(new_ids), "before": geometry(PLASMINOW_SOURCE.read_bytes(), targets=(40, 41)),
            "after": after, "all_local_controls": all_controls, "cancel_translation_movie": [660, 32],
            "preserved": "All original non-HIT button records, action bytes, art/font/text tags and other raw tags; only Cancel41+caption134 root matrices move"}
        output.write_bytes(data)
        receipt.write_text(json.dumps(result, indent=2) + "\n")
        return result


def build(output):
    output = Path(output)
    if digest(SOURCE) != SOURCE_SHA256:
        raise ValueError("Native mini-games require the pinned localized ComboLockPC.swf")
    machine = json.loads((Path.home() / ".agents/skills/adapt-arm32-game/machine_config.json").read_text())
    java = Path(machine["java_exe"])
    ffdec = Path(machine["analyzers"]["ffdec_cli"]).with_suffix(".jar")
    identity = dict(source_sha256=SOURCE_SHA256, generator_sha256=digest(Path(__file__)), ffdec_sha256=digest(ffdec))
    receipt = output.with_suffix(".json")
    if output.is_file() and receipt.is_file():
        old = json.loads(receipt.read_text())
        if all(old.get(k) == v for k, v in identity.items()) and old.get("sha256") == digest(output):
            return old
    output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="native-minigames-", dir=output.parent) as temporary:
        stage = Path(temporary)

        def run(*args):
            result = subprocess.run([str(java), "-Xmx512m", "-jar", str(ffdec), "-onerror", "abort", *map(str, args)],
                capture_output=True, text=True, encoding="utf-8", errors="replace", timeout=60)
            if result.returncode:
                raise RuntimeError(result.stdout + result.stderr)

        run("-swf2xml", SOURCE, stage / "original.xml")
        tree = ET.parse(stage / "original.xml")
        new_ids = patch_xml(tree)
        tree.write(stage / "patched.xml", encoding="utf-8", xml_declaration=True)
        run("-xml2swf", stage / "patched.xml", stage / "encoded.swf")
        data = assemble(SOURCE.read_bytes(), (stage / "encoded.swf").read_bytes(), new_ids)
        (stage / "final.swf").write_bytes(data)
        run("-export", "script", stage / "before", SOURCE)
        run("-export", "script", stage / "after", stage / "final.swf")
        before = {p.relative_to(stage / "before").as_posix(): p.read_bytes() for p in (stage / "before").rglob("*.as")}
        after = {p.relative_to(stage / "after").as_posix(): p.read_bytes() for p in (stage / "after").rglob("*.as")}
        if len(before) != 43 or before != after:
            raise ValueError("Original mini-game ActionScript changed")
        result = {**identity, "source": SOURCE.relative_to(ROOT).as_posix(), "language": "RUS",
            "sha256": hashlib.sha256(data).hexdigest(), "bytes": len(data), "asset": ASSET,
            "overlay_path": "Content/FlashMovies/ComboLockPC.swf", "action_scripts": 43,
            "action_script_hashes": {name: hashlib.sha256(body).hexdigest() for name, body in before.items()},
            "new_hit_shapes": list(new_ids), "before": geometry(SOURCE.read_bytes()), "after": geometry(data, True),
            "cancel_translation_movie": [0, -141.75],
            "preserved": "All original non-HIT button records, action bytes, art/font/text tags and other raw tags; only Cancel21+caption28 vertical placements move"}
        output.write_bytes(data)
        receipt.write_text(json.dumps(result, indent=2) + "\n")
        return result


if __name__ == "__main__":
    print(json.dumps(build(ROOT / "build/native-minigames/ComboLockPC.swf"), indent=2))

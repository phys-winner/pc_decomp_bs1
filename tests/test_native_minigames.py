"""Check the actual localized SWF: invisible geometry, original art and actions."""
import hashlib
import json
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch
import xml.etree.ElementTree as ET

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
import patch_native_minigames as native
import build_apk

PAYLOAD_SHA256 = "773980e69adc828e4571cadbc1276b5482d5df18f346d78b4e716219d0ca6828"


def xml_nodes(root, kind, key):
    return {int(n.get(key)): n for n in root.find("tags") if n.get("type") == kind}


def polygon(shape):
    x = y = 0
    points = []
    for record in shape.findall("shapes/shapeRecords/item"):
        if record.get("stateMoveTo") == "true":
            x, y = int(record.get("moveDeltaX")), int(record.get("moveDeltaY"))
            points.append((x, y))
        elif record.get("type") == "StraightEdgeRecord":
            x += int(record.get("deltaX", 0)); y += int(record.get("deltaY", 0))
            points.append((x, y))
    if points[-1] == points[0]:
        points.pop()
    return points


def transform(points, matrix):
    sx, sy = float(matrix.get("scaleX", 1)), float(matrix.get("scaleY", 1))
    rx, ry = float(matrix.get("rotateSkew0", 0)), float(matrix.get("rotateSkew1", 0))
    tx, ty = int(matrix.get("translateX", 0)), int(matrix.get("translateY", 0))
    return [(sx * x + rx * y + tx, ry * x + sy * y + ty) for x, y in points]


def box(points):
    return min(x for x, y in points), max(x for x, y in points), min(y for x, y in points), max(y for x, y in points)


def in_convex(point, points):
    crosses = []
    for a, b in zip(points, points[1:] + points[:1]):
        crosses.append((b[0] - a[0]) * (point[1] - a[1]) - (b[1] - a[1]) * (point[0] - a[0]))
    return all(c >= -1e-6 for c in crosses) or all(c <= 1e-6 for c in crosses)


class NativeMinigamesTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.folder = tempfile.TemporaryDirectory(prefix="native-mini-test-", dir=ROOT / "build")
        cls.stage = Path(cls.folder.name)
        cls.output = ROOT / "build/native-minigames/ComboLockPC.swf"
        cls.receipt = native.build(cls.output)
        cls.before = native.SOURCE.read_bytes(); cls.after = cls.output.read_bytes()
        machine = json.loads((Path.home() / ".agents/skills/adapt-arm32-game/machine_config.json").read_text())
        cls.java = machine["java_exe"]
        cls.ffdec = str(Path(machine["analyzers"]["ffdec_cli"]).with_suffix(".jar"))
        cls.xml = []
        for index, source in enumerate((native.SOURCE, cls.output)):
            target = cls.stage / (str(index) + ".xml")
            cls.run_ffdec("-swf2xml", source, target)
            cls.xml.append(ET.parse(target).getroot())

    @classmethod
    def tearDownClass(cls):
        cls.folder.cleanup()

    @classmethod
    def run_ffdec(cls, *args):
        result = subprocess.run([cls.java, "-Xmx512m", "-jar", cls.ffdec, "-onerror", "abort", *map(str, args)],
            capture_output=True, text=True, encoding="utf-8", errors="replace", timeout=60)
        if result.returncode:
            raise AssertionError(result.stdout + result.stderr)

    def test_payload_and_source_pins_and_reject_changed_original(self):
        self.assertEqual(hashlib.sha256(self.after).hexdigest(), PAYLOAD_SHA256)
        self.assertEqual(len(self.after), 213296)
        self.assertEqual(hashlib.sha256(self.before).hexdigest(), native.SOURCE_SHA256)
        damaged = self.stage / "changed-original.swf"
        damaged.write_bytes(self.before[:-1] + bytes([self.before[-1] ^ 1]))
        with patch.object(native, "SOURCE", damaged):
            with self.assertRaisesRegex(ValueError, "pinned localized"):
                native.build(self.stage / "must-not-exist.swf")
        self.assertFalse((self.stage / "must-not-exist.swf").exists())

    def test_actual_encoded_polygons_cover_old_hit_and_triangle_missed_corners(self):
        original, final = self.xml
        old_shapes = xml_nodes(original, "DefineShapeTag", "shapeId")
        new_shapes = xml_nodes(final, "DefineShapeTag", "shapeId")
        old_buttons = xml_nodes(original, "DefineButton2Tag", "buttonId")
        new_buttons = xml_nodes(final, "DefineButton2Tag", "buttonId")
        self.assertEqual(set(new_shapes) - set(old_shapes), {52, 53, 54})
        for ident in native.TARGETS:
            old = next(n for n in old_buttons[ident].find("characters") if n.get("buttonStateHitTest") == "true")
            new = next(n for n in new_buttons[ident].find("characters") if n.get("buttonStateHitTest") == "true")
            points = transform(polygon(old_shapes[int(old.get("characterId"))]), old.find("placeMatrix"))
            enlarged = transform(polygon(new_shapes[int(new.get("characterId"))]), new.find("placeMatrix"))
            self.assertEqual(len(enlarged), 4)
            self.assertEqual(len(set(x for x, y in enlarged)), 2)
            self.assertEqual(len(set(y for x, y in enlarged)), 2)
            for point in points:
                self.assertTrue(in_convex(point, enlarged), (ident, point))
            if ident in range(31, 39):
                self.assertEqual(len(points), 3)
                # Near a former bounding-box corner, the original triangular HIT misses.
                old_box = box(points)
                corner = (old_box[1] - 20, old_box[3] - 20)
                self.assertFalse(in_convex(corner, points))
                self.assertTrue(in_convex(corner, enlarged))
        for ident, expected in ((52, (1240, 1504)), (53, (4640, 1504)), (54, (3380, 2620))):
            bound = box(polygon(new_shapes[ident]))
            self.assertEqual((bound[1] - bound[0], bound[3] - bound[2]), expected)

    def test_independent_xml_root_geometry_all_eleven_fit_without_overlap(self):
        root = self.xml[1]
        shapes = xml_nodes(root, "DefineShapeTag", "shapeId")
        buttons = xml_nodes(root, "DefineButton2Tag", "buttonId")
        places = xml_nodes(root, "PlaceObject2Tag", "characterId")
        rectangles = []
        for ident in native.TARGETS:
            hit = next(n for n in buttons[ident].find("characters") if n.get("buttonStateHitTest") == "true")
            points = transform(polygon(shapes[int(hit.get("characterId"))]), hit.find("placeMatrix"))
            points = transform(points, places[ident].find("matrix"))
            bound = box([(x / 20 * 2780 / 1280, y / 20 * 1264 / 720) for x, y in points])
            self.assertGreaterEqual(bound[0], 0); self.assertLessEqual(bound[1], 2780)
            self.assertGreaterEqual(bound[2], 0); self.assertLessEqual(bound[3], 1264)
            self.assertGreaterEqual(min(bound[1] - bound[0], bound[3] - bound[2]), 132)
            rectangles.append((ident, bound))
        self.assertEqual(len(rectangles), 11)
        for index, (ident, a) in enumerate(rectangles):
            for other, b in rectangles[index + 1:]:
                self.assertTrue(a[1] < b[0] or b[1] < a[0] or a[3] < b[2] or b[3] < a[2], (ident, other))
        # The relocated original localized caption stays inside the Cancel target.
        caption = xml_nodes(root, "DefineEditTextTag", "characterID")[28]
        self.assertEqual(caption.get("variableName"), "ComboLock_Cancel")
        bounds = caption.find("bounds")
        corners = [(int(bounds.get(x)), int(bounds.get(y))) for x in ("Xmin", "Xmax") for y in ("Ymin", "Ymax")]
        label = box([(x / 20 * 2780 / 1280, y / 20 * 1264 / 720)
                     for x, y in transform(corners, places[28].find("matrix"))])
        cancel = dict(rectangles)[21]
        self.assertLessEqual(cancel[0], label[0]); self.assertGreaterEqual(cancel[1], label[1])
        self.assertLessEqual(cancel[2], label[2]); self.assertGreaterEqual(cancel[3], label[3])

    def test_only_hit_records_cancel_placements_and_three_new_shapes_change(self):
        old_head, old_stage, old_tags = native.parse(self.before)
        new_head, new_stage, new_tags = native.parse(self.after)
        self.assertEqual(old_head[:4] + old_head[8:], new_head[:4] + new_head[8:])
        self.assertEqual(old_stage, new_stage)
        new_tags = [t for t in new_tags if not (t["code"] == 2 and struct.unpack_from("<H", t["payload"])[0] in (52, 53, 54))]
        self.assertEqual(len(old_tags), len(new_tags))
        changed = []
        for old, new in zip(old_tags, new_tags):
            self.assertEqual(old["code"], new["code"])
            if old["raw"] == new["raw"]:
                continue
            if old["code"] == 34:
                ident = struct.unpack_from("<H", old["payload"])[0]
                self.assertIn(ident, native.TARGETS)
                a, actions = native.buttons(old["payload"]); b, new_actions = native.buttons(new["payload"])
                self.assertEqual(actions, new_actions)
                self.assertEqual(len(a), len(b))
                for before, after in zip(a, b):
                    if before["flags"] != 8:
                        self.assertEqual(before["raw"], after["raw"])
                changed.append((34, ident))
            else:
                self.assertEqual(old["code"], 26)
                a, b = native.placed(old), native.placed(new)
                self.assertIn((a["character"], a["depth"]), ((21, 56), (28, 66)))
                self.assertEqual(a["matrix"][:5], b["matrix"][:5])
                self.assertEqual(a["matrix"][5] - 2835, b["matrix"][5])
                self.assertEqual(old["payload"][:5], new["payload"][:5])
                self.assertEqual(old["payload"][a["end"]:], new["payload"][b["end"]:])
                changed.append((26, a["character"]))
        self.assertEqual(set(changed), {(34, i) for i in native.TARGETS} | {(26, 21), (26, 28)})

    def test_all_original_actionscript_exports_are_identical(self):
        exports = []
        for name, source in (("before", native.SOURCE), ("after", self.output)):
            target = self.stage / name
            self.run_ffdec("-export", "script", target, source)
            exports.append({p.relative_to(target).as_posix(): p.read_bytes() for p in target.rglob("*.as")})
        self.assertEqual(len(exports[0]), 43)
        self.assertEqual(exports[0], exports[1])
        self.assertEqual(self.receipt["action_script_hashes"], {k: hashlib.sha256(v).hexdigest() for k, v in exports[0].items()})

    def test_production_apk_asset_stager_emits_exact_payload_and_receipt(self):
        assets = self.stage / "apk-assets"; assets.mkdir()
        result = build_apk.stage_native_minigames(assets)
        serialized = json.loads(json.dumps(result))
        self.assertEqual((assets / native.ASSET).read_bytes(), self.after)
        self.assertEqual(result["sha256"], PAYLOAD_SHA256)
        self.assertEqual(json.loads((assets / native.ASSET).with_suffix(".json").read_text()), serialized)
        # Staging is deterministic and an intact receipt lets a repeated call reuse output.
        with patch.object(native.subprocess, "run", side_effect=AssertionError("Unexpected regeneration")):
            self.assertEqual(build_apk.stage_native_minigames(assets), serialized)


if __name__ == "__main__":
    unittest.main()

"""Decode the production PlasmiNow payload independently and preserve native policy."""
import hashlib
import json
from pathlib import Path
import struct
import tempfile
import unittest
from unittest.mock import patch
import xml.etree.ElementTree as ET

from tests import test_native_minigames as combo
from tests.test_native_minigames import native, xml_nodes, polygon, transform, box

ROOT = Path(__file__).resolve().parents[1]
PAYLOAD_SHA256 = "9c58c42090d6a74b75f3eb4e55bcac4dcf2bdaadffc0191105a8c2315fbcf22a"


def corners(node):
    return [(int(node.get(x)), int(node.get(y))) for x in ("Xmin", "Xmax") for y in ("Ymin", "Ymax")]


class NativePlasmiNowTest(unittest.TestCase):
    run_ffdec = combo.NativeMinigamesTest.__dict__["run_ffdec"]

    @classmethod
    def setUpClass(cls):
        cls.folder = tempfile.TemporaryDirectory(prefix="native-plasmi-test-", dir=ROOT / "build")
        cls.stage = Path(cls.folder.name)
        cls.output = ROOT / "build/native-minigames/PlasmiNowPC.swf"
        cls.receipt = native.build_plasminow(cls.output)
        cls.before = native.PLASMINOW_SOURCE.read_bytes(); cls.after = cls.output.read_bytes()
        machine = json.loads((Path.home() / ".agents/skills/adapt-arm32-game/machine_config.json").read_text())
        cls.java = machine["java_exe"]
        cls.ffdec = str(Path(machine["analyzers"]["ffdec_cli"]).with_suffix(".jar"))
        cls.xml = []
        for index, source in enumerate((native.PLASMINOW_SOURCE, cls.output)):
            target = cls.stage / (str(index) + ".xml")
            cls.run_ffdec("-swf2xml", source, target)
            cls.xml.append(ET.parse(target).getroot())

    @classmethod
    def tearDownClass(cls):
        cls.folder.cleanup()

    def test_source_payload_pin_reject_changed_input_and_cached_reuse(self):
        self.assertEqual(hashlib.sha256(self.before).hexdigest(), native.PLASMINOW_SOURCE_SHA256)
        self.assertEqual(hashlib.sha256(self.after).hexdigest(), PAYLOAD_SHA256)
        self.assertEqual(len(self.after), 147776)
        changed = self.stage / "changed.swf"
        changed.write_bytes(self.before[:-1] + bytes([self.before[-1] ^ 1]))
        with patch.object(native, "PLASMINOW_SOURCE", changed):
            with self.assertRaisesRegex(ValueError, "pinned localized PlasmiNow"):
                native.build_plasminow(self.stage / "missing.swf")
        self.assertFalse((self.stage / "missing.swf").exists())
        with patch.object(native.subprocess, "run", side_effect=AssertionError("Unexpected regeneration")):
            self.assertEqual(native.build_plasminow(self.output), json.loads(json.dumps(self.receipt)))

    def test_decoded_hit_polygons_other_controls_and_original_captions(self):
        original, final = self.xml
        shapes = {int(n.get("shapeId")): n for n in final.find("tags") if n.get("shapeId")}
        buttons = xml_nodes(final, "DefineButton2Tag", "buttonId")
        places = xml_nodes(final, "PlaceObject2Tag", "characterId")
        self.assertEqual(len(buttons), 16)
        bounds = {}
        for ident, button in buttons.items():
            hit = next(n for n in button.find("characters") if n.get("buttonStateHitTest") == "true")
            shape = shapes[int(hit.get("characterId"))]
            points = polygon(shape) if ident in (40, 41) else corners(shape.find("shapeBounds"))
            if ident in (40, 41):
                self.assertEqual(len(points), 4)
                self.assertEqual(len(set(x for x, y in points)), 2)
                self.assertEqual(len(set(y for x, y in points)), 2)
                self.assertEqual(shape.find("shapes/fillStyles/fillStyles/item").get("fillStyleType"), "0")
            points = transform(transform(points, hit.find("placeMatrix")), places[ident].find("matrix"))
            bounds[ident] = tuple(v / 20 for v in box(points))
        for ident, expected in ((40, (200, 445, 630, 705.2)), (41, (860, 1110, 630, 705.2))):
            a = bounds[ident]
            for actual, desired in zip(a, expected):
                self.assertAlmostEqual(actual, desired, delta=0.08)
            self.assertGreaterEqual(a[0], 160); self.assertLessEqual(a[1], 1120)
            self.assertGreaterEqual(a[2], 0); self.assertLessEqual(a[3], 720)
            self.assertGreaterEqual(min((a[1] - a[0]) * 2780 / 1280, (a[3] - a[2]) * 1264 / 720), 132)
            for other, b in bounds.items():
                if other != ident:
                    self.assertTrue(a[1] < b[0] or b[1] < a[0] or a[3] < b[2] or b[3] < a[2], (ident, other))
            # New solid polygons contain the original bitmap and solid HIT outlines.
            old_button = xml_nodes(original, "DefineButton2Tag", "buttonId")[ident]
            old_hit = next(n for n in old_button.find("characters") if n.get("buttonStateHitTest") == "true")
            old_shape = next(n for n in original.find("tags") if n.get("shapeId") == old_hit.get("characterId"))
            points = transform(transform(corners(old_shape.find("shapeBounds")), old_hit.find("placeMatrix")), places[ident].find("matrix"))
            for x, y in points:
                self.assertTrue(a[0] <= x / 20 <= a[1] and a[2] <= y / 20 <= a[3])
            for record in buttons[ident].find("characters"):
                if record.get("buttonStateHitTest") != "true":
                    art = shapes[int(record.get("characterId"))]
                    points = transform(transform(corners(art.find("shapeBounds")), record.find("placeMatrix")), places[ident].find("matrix"))
                    for x, y in points:
                        self.assertTrue(a[0] <= x / 20 <= a[1] and a[2] <= y / 20 <= a[3], (ident, "visible state"))
        texts = xml_nodes(final, "DefineEditTextTag", "characterID")
        labels = []
        for ident in (125, 127):
            self.assertEqual(texts[ident].get("variableName"), "PlasmiNowConfirm")
            labels.append((40, transform(corners(texts[ident].find("bounds")), places[ident].find("matrix"))))
        sprite = xml_nodes(final, "DefineSpriteTag", "spriteId")[134]
        for child in sprite.find("subTags"):
            if child.get("characterId") in ("132", "133"):
                text = texts[int(child.get("characterId"))]
                self.assertEqual(text.get("variableName"), "PlasmiNowCancel")
                points = transform(corners(text.find("bounds")), child.find("matrix"))
                labels.append((41, transform(points, places[134].find("matrix"))))
        self.assertEqual(len(labels), 4)
        for ident, points in labels:
            a = bounds[ident]
            for x, y in points:
                self.assertTrue(a[0] <= x / 20 <= a[1] and a[2] <= y / 20 <= a[3], ident)
        description = xml_nodes(final, "DefineSpriteTag", "spriteId")[156]
        for child in description.find("subTags"):
            if child.get("characterId") in ("152", "153", "154", "155"):
                points = transform(corners(texts[int(child.get("characterId"))].find("bounds")), child.find("matrix"))
                b = tuple(v / 20 for v in box(transform(points, places[156].find("matrix"))))
                for ident in (40, 41):
                    a = bounds[ident]
                    self.assertTrue(a[1] < b[0] or b[1] < a[0] or a[3] < b[2] or b[3] < a[2], (ident, "description caption"))

    def test_only_two_hits_two_root_matrices_and_two_new_shapes_change(self):
        old_head, old_stage, old_tags = native.parse(self.before)
        new_head, new_stage, new_tags = native.parse(self.after)
        self.assertEqual(old_head[:4] + old_head[8:], new_head[:4] + new_head[8:])
        self.assertEqual(old_stage, new_stage)
        added = set(self.receipt["new_hit_shapes"])
        self.assertEqual(added, {196, 197})
        new_tags = [t for t in new_tags if not (t["code"] == 2 and struct.unpack_from("<H", t["payload"])[0] in added)]
        self.assertEqual(len(old_tags), len(new_tags))
        changed = []
        for old, new in zip(old_tags, new_tags):
            self.assertEqual(old["code"], new["code"])
            if old["raw"] == new["raw"]:
                continue
            if old["code"] == 34:
                ident = struct.unpack_from("<H", old["payload"])[0]
                self.assertIn(ident, (40, 41))
                a, actions = native.buttons(old["payload"]); b, new_actions = native.buttons(new["payload"])
                self.assertEqual(actions, new_actions); self.assertEqual(len(a), len(b))
                self.assertEqual(old["payload"][:3], new["payload"][:3])
                for before, after in zip(a, b):
                    if before["flags"] != 8:
                        self.assertEqual(before["raw"], after["raw"])
                    else:
                        self.assertEqual(before["flags"], after["flags"])
                        self.assertEqual(before["depth"], after["depth"])
                        self.assertEqual(before["matrix"], after["matrix"])
                changed.append((34, ident))
            else:
                self.assertEqual(old["code"], 26)
                a, b = native.placed(old), native.placed(new)
                self.assertIn((a["character"], a["depth"]), ((41, 4), (134, 423)))
                self.assertEqual((*a["matrix"][:4], a["matrix"][4] + 13200, a["matrix"][5] + 640), b["matrix"])
                self.assertEqual(old["payload"][:5], new["payload"][:5])
                self.assertEqual(old["payload"][a["end"]:], new["payload"][b["end"]:])
                changed.append((26, a["character"]))
        self.assertEqual(set(changed), {(34, 40), (34, 41), (26, 41), (26, 134)})

    def test_all_110_script_exports_and_cancel_hide_policy_remain_identical(self):
        exports = []
        for name, source in (("before", native.PLASMINOW_SOURCE), ("after", self.output)):
            target = self.stage / name
            self.run_ffdec("-export", "script", target, source)
            exports.append({p.relative_to(target).as_posix(): p.read_bytes() for p in target.rglob("*.as")})
        self.assertEqual(len(exports[0]), 110)
        self.assertEqual(exports[0], exports[1])
        self.assertEqual(self.receipt["action_script_hashes"], {k: hashlib.sha256(v).hexdigest() for k, v in exports[0].items()})
        scripts = b"\n".join(exports[1].values())
        self.assertEqual(scripts.count(b"_root.CancelButtonBG._y = 10000;"), 6)
        self.assertIn(b"function SetAsSlotMode()", scripts)
        self.assertIn(b"function MandatoryEquip()", scripts)
        self.assertIn(b"_root.CancelButton.gotoAndStop(2);", scripts)
        self.assertIn(b"_root.bDisableCancel = true;", scripts)


if __name__ == "__main__":
    unittest.main()

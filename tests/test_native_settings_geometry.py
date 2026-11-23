"""Check original PausePC input geometry without rebuilding or changing the movie."""
import copy
import json
from pathlib import Path
import shutil
import subprocess
import unittest
import xml.etree.ElementTree as ET

from tools import patch_native_settings as native

ROOT = Path(__file__).resolve().parents[1]


class NativeSettingsGeometryTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        source = ROOT / "build/localization/Content/FlashMovies/pausePC.swf"
        if native.digest(source) != native.SOURCE_SHA256:
            raise AssertionError("Original Russian PausePC source changed")
        cls.tags = ET.parse(ROOT / "build/native-settings/review-original.xml").getroot().find("tags")
        cls.characters = {node.get("spriteId", node.get("buttonId", node.get("characterID", node.get("shapeId")))): node
                          for node in cls.tags if node.get("type", "").startswith("Define")}
        cls.bounds = native.slider_bounds(cls.tags, cls.characters, ROOT / "build/native-settings/review-script/scripts")

    def test_original_slider_hit_rectangles_cover_visual_knobs(self):
        self.assertEqual({frame: set(rows) for frame, rows in self.bounds.items() if rows},
                         {7: {8, 9, 10, 11}, 8: {3}, 9: {2, 3, 4, 5}, 10: {1}})
        expected = {
            (7, 8): (781.5248768, 364.899881425, 1061.7751232, 448.901166625),
            (7, 9): (781.5248768, 464.899881425, 1061.7751232, 548.901166625),
            (7, 10): (780.2748768, 564.899881425, 1060.5251232, 648.901166625),
            (7, 11): (500.0248768, 184.849881425, 780.2751232, 268.851166625),
            (8, 3): (498.2748768, 188.949881425, 778.5251232, 272.951166625),
        }
        expected.update({(9, 2): expected[7, 8], (9, 3): expected[7, 9], (9, 4): expected[7, 10],
                         (9, 5): expected[8, 3], (10, 1): expected[8, 3]})
        for (frame, row), rectangle in expected.items():
            for actual, wanted in zip(self.bounds[frame][row], rectangle):
                self.assertAlmostEqual(actual, wanted, places=7)
        # Reported audio failure: slidersize_mc mouse Y=-36.2 at the visible knob.
        # Its measuring clip starts 853 twips (42.65 px) below DoubleTab2.
        knob_x, knob_y = 15539 / 20 + 150, 8191 / 20 + 853 / 20 - 36.2
        left, top, right, bottom = self.bounds[9][2]
        self.assertTrue(left <= knob_x <= right and top <= knob_y <= bottom)
        self.assertLess(-36.2, 0)

    def test_hit_shape_excludes_other_button_states(self):
        characters = copy.deepcopy(self.characters)
        button = characters["278"]
        art = copy.deepcopy(button.find("characters")[0])
        art.set("buttonStateHitTest", "false")
        art.set("buttonStateUp", "true")
        art.find("placeMatrix").set("scaleX", "100")
        button.find("characters").append(art)
        hit = native.character_bounds("278", characters, hit_test=True)
        for actual, wanted in zip(hit, (-2678.502464, -155, 2926.502464, 1525)):
            self.assertAlmostEqual(actual, wanted, places=6)
        self.assertLess(native.character_bounds("278", characters)[0], hit[0])

    def test_slider_press_selects_own_row_and_preserves_original_guards(self):
        exported = ROOT / "build/native-settings/review-script/scripts"
        cases = []
        for path in exported.glob("DefineButton2_*/BUTTONCONDACTION on(press).as"):
            original = path.read_text(encoding="utf-8")
            if '_root.guiVal = "SLIDER";' not in original:
                continue
            rollover = (path.parent / "BUTTONCONDACTION on(rollOver).as").read_text(encoding="utf-8")
            row = int(native.re.findall(r"_root\.selectorPos = (\d+);", rollover)[0])
            adjusted = native.responsive_slider_press(original, row)
            self.assertTrue(adjusted.index(f"_root.selectorPos = {row};") < adjusted.index('_root.guiVal = "SLIDER";'))
            cases.append([row, original[original.index("{") + 1:original.rindex("}")],
                          adjusted[adjusted.index("{") + 1:adjusted.rindex("}")]])
        self.assertEqual(len(cases), 10)
        script = r'''
const cases=JSON.parse(process.argv[1]);
for(const [row,original,adjusted] of cases)for(const mainMenu of [false,true]){
    function root(){const r={selectorPos:0,guiVal:'NULL',bInMainMenu:mainMenu,ScrollBar0:{_y:-1},SlotHolder:{}};
        for(let i=0;i<16;i++)r.SlotHolder['Slot'+i]={_y:i*100};
        for(const key of ['NumGraphicsOptions','NumActorDetailOptions','NumTextureOptions','NumResolutionOptions',
            'NumMouseSensitivities','NumVolumes','NumSpeakerModes','NumDifficultyModes'])r[key]=11;
        return r;}
    const before=root(),after=root();new Function('_root',original)(before);new Function('_root',adjusted)(after);
    if(before.guiVal==='SLIDER'){
        if(after.selectorPos!==row||after.ScrollBar0._y!==row*100||after.guiVal!=='SLIDER')throw Error('stale hover');
        if(after.numSliderSpots!==before.numSliderSpots)throw Error('changed original slider range');
    }else if(JSON.stringify(before)!==JSON.stringify(after))throw Error('bypassed original guard');
}
'''
        subprocess.run([shutil.which("node"), "-e", script, json.dumps(cases)], check=True, timeout=10)

    def test_flat_metadata_is_numeric_and_frame_names_prevent_stale_rows(self):
        node = shutil.which("node")
        self.assertIsNotNone(node)
        metadata = native.slider_bounds_script(self.bounds)
        self.assertNotIn("new Object", metadata)
        self.assertEqual(metadata.count("_root.PortSlider"), 40)
        script = ("const _root={};\n" + metadata +
                  "if(_root.PortSlider9_2_left!==781.5248768)throw Error('audio bounds');\n" +
                  "if(_root.PortSlider8_2_left!==undefined)throw Error('stale audio row');\n" +
                  "if(typeof _root.PortSlider8_3_top!=='number')throw Error('not numeric');\n" +
                  "if(_root.PortSlider6_2_left!==undefined)throw Error('stale options bounds');\n" +
                  "if(Object.values(_root).some(v=>typeof v!=='number'))throw Error('nested nonclip target');\n")
        subprocess.run([node, "-e", script], check=True, timeout=10)

    def test_modal_button_name_preserves_original_depth_geometry_and_events(self):
        characters = copy.deepcopy(self.characters)
        original = copy.deepcopy(characters["81"])
        native.name_confirm_buttons(characters)
        placement = next(node for node in characters["81"].find("subTags") if node.get("characterId") == "73")
        self.assertEqual(placement.get("name"), "PortButtons")
        self.assertEqual(placement.get("placeFlagHasName"), "true")
        placement.attrib.pop("name")
        placement.set("placeFlagHasName", "false")
        self.assertEqual(ET.tostring(characters["81"]), ET.tostring(original))
        for ident in ("69", "71", "73"):
            self.assertEqual(ET.tostring(characters[ident]), ET.tostring(self.characters[ident]))

    def test_port_depths_are_free_on_every_original_frame_and_below_modal(self):
        native.validate_port_depths(self.tags)
        self.assertEqual(native.PORT_DEPTHS, (420, 422, 423, 424, 425))
        self.assertIn(421, {int(node.get("depth")) for node in self.tags if node.get("depth")})
        for depth in (native.PORT_DEPTHS[0], 437):
            tags = copy.deepcopy(self.tags)
            ET.SubElement(tags, "item", {"type": "PlaceObject2Tag", "depth": str(depth)})
            if depth == 437:
                next(node for node in tags if node.get("name") == "ConfirmBox").set("depth", "425")
            with self.assertRaises(ValueError):
                native.validate_port_depths(tags)

    def _bottom_rows(self, tags):
        """Per frame, the placed objects whose bottom is below 640 px (the bottom button row)."""
        frame, live, rows = 1, {}, {}
        for tag in tags:
            typ = tag.get("type", "")
            if typ == "ShowFrameTag":
                for depth, node in live.items():
                    if node.get("characterId") in ("225", "168", "308", "324", "292", "81"):
                        continue   # backgrounds and full-screen art
                    try:
                        box = native.placed_bounds(node, self.characters)
                    except (KeyError, ValueError, TypeError):
                        continue   # text with a font reference or a rotated clip: not a button row
                    if box[3] > 640 * 20 and box[1] > 600 * 20:
                        rows.setdefault(frame, {})[depth] = box
                frame += 1
            elif typ.startswith("RemoveObject"):
                live.pop(tag.get("depth"), None)
            elif typ.startswith("PlaceObject"):
                previous, node = live.get(tag.get("depth")), copy.deepcopy(tag)
                if not node.get("characterId") and previous is not None:
                    node.set("characterId", previous.get("characterId"))
                if node.find("matrix") is None and previous is not None:
                    node.append(copy.deepcopy(previous.find("matrix")))
                if node.get("characterId") and node.find("matrix") is not None:
                    live[tag.get("depth")] = node
        return rows

    def test_graphics_bottom_row_fits_the_stage_like_other_screens(self):
        """D6: APPLY/DISCARD/ADJUST BRIGHTNESS were cut by the bottom edge of a 2400x1080 screen."""
        original = self._bottom_rows(self.tags)
        row = set(native.GRAPHICS_BOTTOM_ROW)
        self.assertAlmostEqual(max(original[7][d][3] for d in row) / 20, 724.1, places=1)   # beyond 720
        tags = copy.deepcopy(self.tags)
        native.lift_graphics_bottom_row(tags)
        lifted = self._bottom_rows(tags)
        for depth in row:
            before, after = original[7][depth], lifted[7][depth]
            self.assertEqual((before[0], before[2]), (after[0], after[2]))      # columns unchanged
            self.assertAlmostEqual(before[1] - after[1], 200)
        # Same top and bottom as the bottom row of the Input, Audio and Load screens, inside the stage.
        self.assertAlmostEqual(max(lifted[7][d][3] for d in row) / 20, 714.1, places=1)
        self.assertAlmostEqual(min(lifted[7][d][1] for d in ("5", "143", "146")) / 20, 681.1, places=1)
        for frame in (4, 8, 9, 10, 11):
            self.assertAlmostEqual(max(box[3] for box in original[frame].values() if box[1] > 680 * 20) / 20, 714.1, places=1)
        # Only frame 7 moves; a changed preimage is refused.
        for frame in lifted:
            if frame != 7:
                self.assertEqual(lifted[frame], original[frame])
        with self.assertRaises(ValueError):
            native.lift_graphics_bottom_row(tags)


if __name__ == "__main__":
    unittest.main()

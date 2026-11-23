"""Run the production loading animation and preserve the original movie art."""
import json
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest
import xml.etree.ElementTree as ET

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
import patch_loading_indicator as loading


class LoadingIndicatorTest(unittest.TestCase):
    def test_busy_motion_real_progress_and_bounds(self):
        script = (ROOT / "android/flash/loading_indicator.as").read_text(encoding="utf-8")
        probe = r'''
const assert = require('assert');
var now = 0, getTimer = () => now;
var _root = {ProgressBarFiller: {}, ProgressBarNew: {}, ProgressBarFillerWidth: undefined};
_root.PortLoadingFrame = PortLoadingFrame;
/* ACTUAL */
_root.PortLoadingFrame = PortLoadingFrame;
assert(!_root.PortLoadingReady);
// The frame's actions are before its PlaceObject tags. Initialize only after placement.
_root.ProgressBarFiller={_x:400,_y:-33.95,_width:640};
_root.ProgressBarNew={_x:400,_y:106.05,_visible:true};
_root.onEnterFrame();
assert(_root.PortLoadingReady);
assert.strictEqual(_root.ProgressBarFillerWidth,640);
assert.strictEqual(_root.ProgressBarFiller._y,106.05);
assert.strictEqual(_root.ProgressBarNew._visible,false);
for (let fraction of [0, undefined, NaN, -1]) {
  FillProgressBar(fraction);
  const positions = new Set();
  for (now = 0; now < 3600; now += 30) {
    _root.onEnterFrame();
    const bar = _root.ProgressBarFiller;
    assert.strictEqual(bar._width, 640 * .18);
    assert(bar._x - bar._width/2 >= 79.99999 && bar._x + bar._width/2 <= 720.00001);
    positions.add(bar._x);
  }
  assert(positions.size > 20);
}
for (let fraction of [.01, .25, .5, 1, 2]) {
  FillProgressBar(fraction);
  assert.strictEqual(_root.ProgressBarFiller._x, 400);
  assert.strictEqual(_root.ProgressBarFiller._width, 640 * Math.min(1, fraction));
  now += 600; _root.onEnterFrame();
  assert.strictEqual(_root.ProgressBarFiller._width, 640 * Math.min(1, fraction));
}
console.log('PASS loading: bounded moving busy segment, real fractions, reset, NaN and clamp');
'''
        result = subprocess.run([shutil.which("node"), "-e", probe.replace("/* ACTUAL */", script)],
                                cwd=ROOT, capture_output=True, text=True, timeout=20)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_compiled_movie_preserves_art_and_other_scripts(self):
        with tempfile.TemporaryDirectory(prefix="loading-test-", dir=ROOT / "build") as temporary:
            stage = Path(temporary)
            output = stage / "native-loading.swf"
            receipt = loading.build(output)
            self.assertEqual(receipt["source_sha256"], loading.SOURCE_SHA256)
            self.assertEqual(loading.build(output), receipt)
            java, ffdec = loading.tools()
            movies = []
            for index, source in enumerate((loading.SOURCE, output)):
                xml = stage / (str(index) + ".xml")
                result = subprocess.run([str(java), "-Xmx512m", "-jar", str(ffdec), "-swf2xml", str(source), str(xml)],
                                        capture_output=True, text=True, timeout=60)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                root = ET.parse(xml).getroot()
                movies.append(root)
            before, after = (list(root.find("tags")) for root in movies)
            bar = next(n for n in before if n.get("name") == "ProgressBarFiller")
            sprite = next(n for n in before if n.get("spriteId") == bar.get("characterId"))
            placement = sprite.find("subTags")[0]
            shape = next(n for n in before if n.get("shapeId") == placement.get("characterId"))
            bounds = shape.find("shapeBounds")
            self.assertLessEqual(abs(int(bounds.get("Xmin")) + int(bounds.get("Xmax"))), 1)
            decoration = next(n for n in before if n.get("name") == "ProgressBarNew")
            self.assertLess(int(bar.find("matrix").get("translateY")), 0)
            self.assertGreater(int(decoration.find("matrix").get("translateY")), 0)
            self.assertEqual(bar.find("matrix").get("translateX"), decoration.find("matrix").get("translateX"))
            self.assertEqual(len(before), len(after))
            changed = [(a, b) for a, b in zip(before, after) if ET.tostring(a) != ET.tostring(b)]
            self.assertEqual(len(changed), 1)
            self.assertEqual(changed[0][0].get("type"), "DoActionTag")
            self.assertEqual(changed[0][1].get("type"), "DoActionTag")


if __name__ == "__main__":
    unittest.main()

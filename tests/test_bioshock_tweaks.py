"""Execute the production game tweaks of android/host/bioshock_tweaks.c."""
from pathlib import Path
import subprocess
import tempfile
import unittest

from tests.host_environment import CLANG, environment

ROOT = Path(__file__).resolve().parents[1]


class BioshockTweaksTest(unittest.TestCase):
    def test_tweaks(self):
        env, linker = environment("x64")
        with tempfile.TemporaryDirectory(prefix="bioshock-tweaks-") as tmp:
            out = Path(tmp)
            subprocess.run([str(CLANG), "/nologo", "/O2", "/std:c11", "/D_CRT_SECURE_NO_WARNINGS",
                            "/clang:--target=x86_64-pc-windows-msvc", "/Itests", "/c",
                            str(ROOT / "tests/test_bioshock_tweaks.c"), "/Fo" + str(out / "test.obj")],
                           cwd=ROOT, env=env, check=True)
            subprocess.run([str(linker), "/nologo", str(out / "test.obj"), "/OUT:" + str(out / "test.exe")], env=env, check=True)
            subprocess.run([str(out / "test.exe")], env=env, check=True, timeout=30)

    def test_seam_and_subtitle_preimages(self):
        import tools.build_android as build
        import tools.patch_hud_layout as hud
        seams = {seam[0]: (seam[1], seam[2], seam[3] if len(seam) > 3 else 1) for seam in build.SEAMS}
        before, after, copies = seams[0x10CB85C0]
        source = (ROOT / "generated/bioshock/functions_0159.c").read_text(errors="replace")
        self.assertEqual(source.count(before), 1)
        self.assertIn("void_bioshock_reroll_blocked", after)
        self.assertEqual(copies, 1)
        # UObject::ProcessEvent's GScriptCycles clock reads: the body and the copy the generator inlined into
        # sub_109EF150 (android/host/bioshock_script_clock.c).
        for address in (0x1096792E, 0x10967A04):
            before, after, copies = seams[address]
            self.assertEqual(copies, 2)
            self.assertIn("void_bioshock_script_clock()", after)
            self.assertIn(before.split(";")[0] + ";", after)   # the original call stays for BIOSHOCK_SCRIPT_CYCLES=1
            for name in ("functions_0027.c", "functions_0052.c"):
                text = (ROOT / "generated/bioshock" / name).read_text(errors="replace")
                self.assertEqual(text.count(before), 1, (hex(address), name))
        for address, source_file, fallback in (
            (0x10983F04, "functions_0032.c", "goto L_10983F16;"),
            (0x109A120A, "functions_0035.c", "D3D10_FILTER_MIN_MAG_MIP_POINT"),
            (0x10AAC3FA, "functions_0086.c", "goto L_10AAC4D4;"),
        ):
            before, after, copies = seams[address]
            source = (ROOT / "generated/bioshock" / source_file).read_text(errors="replace")
            self.assertEqual(source.count(before), copies)
            self.assertIn(fallback, after)
        original = "x " + hud.RADIAL_SUBTITLE_BEFORE + " y"
        self.assertIn("_currentframe >= 25", hud.fix_radial_subtitles(original))
        with self.assertRaises(ValueError):
            hud.fix_radial_subtitles("no preimage")


if __name__ == "__main__":
    unittest.main()

"""The launcher's Space card for BioShock (android/port.json "cleanup") and the start-up pruning helpers
(android/render10/r10_prune.h).

Globs follow launcher/android/src/port/launcher/Glob.java (case-insensitive, "*" and "?" inside a name, "**" across
folders, a trailing "/" means the whole folder). The listing fixtures hold the GOG BioShock 1.1 folder
(tests/fixtures/bioshock-gog-1.1-files.txt) and the names the port writes into <game>/userdata
(tests/fixtures/bioshock-userdata-files.txt)."""
import json
from pathlib import Path
import re
import subprocess
import unittest

from tests.host_environment import CLANG, environment

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / "build/tests/r10-prune"
FIXTURES = ROOT / "tests/fixtures"


def compile_glob(source):
    text = source.replace("\\", "/").lstrip("/")
    if text.endswith("/"):
        text += "**"
    regex, i = "", 0
    while i < len(text):
        c = text[i]
        if c == "*" and text[i + 1:i + 2] == "*":
            slash = text[i + 2:i + 3] == "/"
            regex += "(?:.*/)?" if slash else ".*"
            i += 2 if slash else 1
        elif c == "*":
            regex += "[^/]*"
        elif c == "?":
            regex += "[^/]"
        else:
            regex += re.escape(c)
        i += 1
    return re.compile(regex, re.IGNORECASE)


def lines(name):
    return [l for l in (FIXTURES / name).read_text(encoding="utf-8").splitlines() if l]


def categories():
    manifest = json.loads((ROOT / "android/port.json").read_text(encoding="utf-8"))
    return manifest["cleanup"]["categories"]


PROTECTED_SUFFIXES = (".ini", ".int", ".lbf", ".ibf", ".swf", ".bik", ".fsb", ".blk", ".bsm", ".bdc", ".u", ".dll", ".exe",
                      ".lst", ".deu", ".esp", ".fra", ".ita", ".pcs", ".pcs10", ".spk", ".bsg", ".bmp")


class CleanupManifestTest(unittest.TestCase):
    def test_categories_parse(self):
        ids = set()
        for c in categories():
            self.assertNotIn(c["id"], ids)
            ids.add(c["id"])
            self.assertEqual(c["part"], "game")
            for language in ("en", "ru"):
                self.assertTrue(c["title"][language].strip())
            self.assertTrue(c["globs"])
            for g in c["globs"]:
                self.assertIsInstance(g, str)
                self.assertNotIn("..", g)
                self.assertFalse(g.startswith("/") or ":" in g)
                self.assertNotIn("**", g, "no catch-all patterns")

    def test_every_glob_matches_a_listed_entry(self):
        listing = lines("bioshock-gog-1.1-files.txt") + lines("bioshock-userdata-files.txt")
        for c in categories():
            for g in c["globs"]:
                pattern = compile_glob(g)
                self.assertTrue(any(pattern.fullmatch(p) for p in listing), f"{c['id']}: {g} matches nothing")

    def test_no_glob_matches_a_protected_path(self):
        inputs = json.loads((ROOT / "config/inputs.json").read_text(encoding="utf-8"))
        modules = {m["path"].lower() for m in inputs["modules"]}
        self.assertEqual(len(modules), 7)
        modules_json = json.loads((ROOT / "config/modules.json").read_text(encoding="utf-8"))
        modules |= {m["path"].lower() for m in modules_json["modules"] if m["path"].startswith("Builds/")}
        listing = lines("bioshock-gog-1.1-files.txt") + lines("bioshock-userdata-files.txt")
        for p in modules:
            self.assertIn(p, [x.lower() for x in listing])

        def protected(path):
            low = path.lower()
            if low in modules or low.startswith("content/"):
                return True
            if low.startswith("userdata/documents/") or low.startswith("userdata/appdata/"):
                return True
            if low in ("userdata/bioshock.log",):
                return True
            if low.startswith("builds/") and low.endswith(PROTECTED_SUFFIXES):
                return True
            if low.startswith("builds/release/") and low.rsplit("/", 1)[1] in ("dfe",):
                return True
            return low.endswith((".ini", ".int", ".lbf", ".ibf")) and not low.startswith(("goglog", "unins", "userdata/"))

        for c in categories():
            for g in c["globs"]:
                pattern = compile_glob(g)
                for path in listing:
                    if pattern.fullmatch(path):
                        self.assertFalse(protected(path), f"{c['id']}: {g} matches protected {path}")
        # the GOG installer ini files are not game inputs; everything else called .ini stays
        hit = {p for c in categories() for g in c["globs"] for p in listing if compile_glob(g).fullmatch(p)}
        self.assertNotIn("Content/contentmode.ini", hit)
        self.assertNotIn("Builds/Release/Default.ini", hit)

    def test_userdata_saves_and_settings_stay(self):
        hit = {p for c in categories() for g in c["globs"] for p in lines("bioshock-userdata-files.txt")
               if compile_glob(g).fullmatch(p)}
        for kept in ("userdata/bioshock.log", "userdata/Documents/Bioshock/SaveGames/Save0.bsg",
                     "userdata/Documents/Bioshock/BioshockUser.ini", "userdata/AppData/Local/Bioshock/preferences.bin"):
            self.assertNotIn(kept, hit)
        for gone in ("userdata/ShaderCache/r10-es-0123456789abcdef-0123456789abcdef-0123456789abcdef-0123456789abcdef.bin",
                     "userdata/bioshock.1.log", "userdata/bioshock.2.log", "userdata/r10-pipeline-cache-turnip.bin",
                     "userdata/bink-voice/0123abcd-Intro.bik", "userdata/Temp/.mapping-AbCdEf"):
            self.assertIn(gone, hit)


HARNESS = r'''
#include <assert.h>
#include <stdint.h>
#include <string.h>
#include "r10_prune.h"

int main(void)
{
    const uint64_t current = 0x0123456789abcdefull;
    uint64_t key = 0;
    assert(r10_prune_es_name("r10-es-0123456789abcdef-00000000000000aa-00000000000000bb-00000000000000cc.bin", &key) && key == current);
    assert(!r10_prune_es_stale("r10-es-0123456789abcdef-00000000000000aa-00000000000000bb-00000000000000cc.bin", current));
    assert(r10_prune_es_stale("r10-es-fedcba9876543210-00000000000000aa-00000000000000bb-00000000000000cc.bin", current));
    /* anything not exactly the r10_gles.c naming is never touched */
    assert(!r10_prune_es_stale("r10-es-fedcba9876543210-00000000000000aa-00000000000000bb-00000000000000cc.bin.tmp", current));
    assert(!r10_prune_es_stale("r10-es-fedcba9876543210-00000000000000aa-00000000000000bb-00000000000000cc.bi", current));
    assert(!r10_prune_es_stale("r10-es-FEDCBA9876543210-00000000000000aa-00000000000000bb-00000000000000cc.bin", current));
    assert(!r10_prune_es_stale("r10-es-fedcba9876543210_00000000000000aa-00000000000000bb-00000000000000cc.bin", current));
    assert(!r10_prune_es_stale("r10-es-fedcba987654321-00000000000000aa-00000000000000bb-00000000000000cc.bin", current));
    assert(!r10_prune_es_stale("r10-vk-fedcba9876543210-00000000000000aa-00000000000000bb-00000000000000cc.bin", current));
    assert(!r10_prune_es_stale("ShaderCache.pcs", current));
    assert(!r10_prune_es_stale("", current));

    assert(r10_prune_miss_name("r10-shader-miss-0123456789abcdef.dxbc"));
    assert(!r10_prune_miss_name("r10-shader-miss-0123456789abcde.dxbc"));
    assert(!r10_prune_miss_name("r10-shader-miss-0123456789abcdef.dxbc.bak"));
    assert(!r10_prune_miss_name("r10-shader-miss-0123456789abcdeg.dxbc"));
    assert(!r10_prune_miss_name("r10-pipeline-cache-system.bin"));

    assert(!r10_prune_miss_delete(64, 10, 100));   /* not more than 64: all stay */
    assert(r10_prune_miss_delete(65, 10, 100));    /* older than the session */
    assert(!r10_prune_miss_delete(65, 100, 100));  /* this session's */
    assert(!r10_prune_miss_delete(65, 101, 100));
    return 0;
}
'''


class PruneHelpersTest(unittest.TestCase):
    def test_names_and_decisions(self):
        OUT.mkdir(parents=True, exist_ok=True)
        source, exe = OUT / "prune.c", OUT / "prune.exe"
        source.write_text(HARNESS, encoding="utf-8")
        env, linker = environment("x64")
        obj = OUT / "prune.obj"
        subprocess.run([str(CLANG), "/nologo", "/O1", "/std:c11", "/clang:--target=x86_64-pc-windows-msvc", "/c",
                        "/I" + str(ROOT / "android/render10"), str(source), "/Fo" + str(obj)], check=True, env=env, cwd=OUT)
        subprocess.run([str(linker), "/nologo", str(obj), "/OUT:" + str(exe)], check=True, env=env, cwd=OUT)
        subprocess.run([str(exe)], check=True, env=env)

    def test_wired_into_the_renderer(self):
        gles = (ROOT / "android/render10/r10_gles.c").read_text(encoding="utf-8")
        device = (ROOT / "android/render10/r10_device.c").read_text(encoding="utf-8")
        self.assertIn("r10_prune_program_binaries(program_cache_dir, program_driver_key)", gles)
        self.assertIn("r10_prune_shader_misses(void_data_dir()", device)


if __name__ == "__main__":
    unittest.main()

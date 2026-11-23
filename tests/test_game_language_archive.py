"""GameLanguage finds a language pack inside a stored ZIP the launcher plays in place.

The launcher's game folder for "Play from archive" is the sidecar folder (files/launcher/mount-bioshock-game),
which holds only the archive's few compressed files; the archive's index (ZMI1, StoredArchive.java) lies beside
it as mount-bioshock-game.zmi. Before 21 November 2026 GameLanguage looked at the sidecar alone, so a ZIP with the
Russian files still refused to start ("Не найдены файлы языка"). The production GameLanguage.java runs on the
desktop JVM against such an index.
"""
from pathlib import Path
import shutil
import struct
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "android/app/src/org/bioshock/port/GameLanguage.java"
LEVELS = ["0-lighthouse", "1-welcome", "1-medical", "2-fisheries", "2-subbay", "3-arcadia", "3-market", "4-recreation",
          "5-hephaestus", "5-ryan", "6-resi", "6-slums", "7-bossfight", "7-gauntlet", "7-science"]
BANKS = ["0", "1", "2", "3", "4", "5", "6", "7", "99", "aivo"]

HARNESS = r"""
package org.bioshock.port;

import java.io.File;

public final class LanguageProbe {
    public static void main(String[] args) {
        System.out.println(args[1] + " " + GameLanguage.missing(new File(args[0]), args[1], args[1]));
    }
}
"""


def russian_files():
    files = ["Content/Localizedrus.lbf"]
    files += [f"Content/Maps/{level}_rus.bsm" for level in LEVELS]
    files += [f"Content/Sounds_Windows/streams_{bank}_audio.rus_fsb" for bank in BANKS]
    return files


def write_index(path, names):
    """ZMI1 as StoredArchive.writeIndex writes it: header, then one record per file (little endian)."""
    with path.open("wb") as out:
        out.write(struct.pack("<4sIQ", b"ZMI1", len(names), 123456789) + bytes(32))
        for i, name in enumerate(names):
            encoded = name.encode("utf-8")
            out.write(struct.pack("<QQIIQH", 4096 * (i + 1), 1000 + i, 0x1234, 0, 0, len(encoded)) + encoded)


@unittest.skipUnless(shutil.which("javac") and shutil.which("java"), "needs a JDK")
class GameLanguageArchiveTest(unittest.TestCase):
    def run_probe(self, classes, folder, language):
        result = subprocess.run(["java", "-cp", str(classes), "org.bioshock.port.LanguageProbe", str(folder), language],
                                capture_output=True, text=True, timeout=60)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        return result.stdout.strip()

    def test_language_pack_in_archive(self):
        with tempfile.TemporaryDirectory(prefix="bioshock-language-", dir=ROOT / "build") as temporary:
            work = Path(temporary)
            harness = work / "LanguageProbe.java"
            harness.write_text(HARNESS, encoding="utf-8")
            classes = work / "classes"
            classes.mkdir()
            compiled = subprocess.run(["javac", "-encoding", "UTF-8", "-d", str(classes), str(SOURCE), str(harness)],
                                      capture_output=True, text=True, timeout=120)
            self.assertEqual(compiled.returncode, 0, compiled.stdout + compiled.stderr)
            sidecar = work / "mount-bioshock-game"
            (sidecar / "Content/System").mkdir(parents=True)
            (sidecar / "Content/contentmode.ini").write_text("[ContentMode]\n", encoding="utf-8")
            files = russian_files()
            # A plain folder without the pack: every Russian file is missing; English needs nothing.
            self.assertTrue(self.run_probe(classes, sidecar, "rus").startswith("rus [Content/Localizedrus.lbf, "))
            self.assertEqual(self.run_probe(classes, sidecar, "int"), "int []")
            # The archive carries the pack (any case, '\\' or '/' in its names).
            names = [name.upper() if i % 3 == 0 else name.replace("/", "\\") if i % 3 == 1 else name for i, name in enumerate(files)]
            write_index(work / "mount-bioshock-game.zmi", ["Builds/Release/Bioshock.exe"] + names)
            self.assertEqual(self.run_probe(classes, sidecar, "rus"), "rus []")
            # One file absent from both the archive and the sidecar is reported.
            write_index(work / "mount-bioshock-game.zmi", ["Builds/Release/Bioshock.exe"] + names[1:])
            self.assertEqual(self.run_probe(classes, sidecar, "rus"), "rus [Content/Localizedrus.lbf]")
            # ... and found when the launcher unpacked it into the sidecar instead.
            (sidecar / "Content/Localizedrus.lbf").write_bytes(b"lbf")
            self.assertEqual(self.run_probe(classes, sidecar, "rus"), "rus []")
            # A compact bank is available with its corresponding English bank.
            compact = [name.removesuffix(".rus_fsb") + ".rus_fsd" for name in files if name.endswith(".rus_fsb")]
            bases = [name.removesuffix(".rus_fsd") + ".fsb" for name in compact]
            maps = [name for name in files if not name.endswith(".rus_fsb")]
            write_index(work / "mount-bioshock-game.zmi", maps + compact + bases)
            self.assertEqual(self.run_probe(classes, sidecar, "rus"), "rus []")
            write_index(work / "mount-bioshock-game.zmi", maps + compact + bases[1:])
            self.assertEqual(self.run_probe(classes, sidecar, "rus"), f"rus [{files[-len(BANKS)]}]")
            base = sidecar / bases[0]
            base.parent.mkdir(parents=True)
            base.write_bytes(b"fsb")
            self.assertEqual(self.run_probe(classes, sidecar, "rus"), "rus []")
            # A damaged index is no archive: the sidecar decides.
            (work / "mount-bioshock-game.zmi").write_bytes(b"ZMI0" + bytes(60))
            self.assertIn("Content/Maps/0-lighthouse_rus.bsm", self.run_probe(classes, sidecar, "rus"))


if __name__ == "__main__":
    unittest.main()

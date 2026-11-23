"""Exercise direct offset reads and provenance on a small complete OBB fixture."""
import json
from pathlib import Path
import shutil
import tempfile
import zipfile
import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from tools import build_data_obb

ROOT = Path(__file__).resolve().parents[1]


def main():
    with tempfile.TemporaryDirectory(prefix="bioshock-obb-test-", dir=ROOT / "build") as temporary:
        root = Path(temporary)
        original = root / "original"
        localized = root / "localized"
        (original / "Builds/Release").mkdir(parents=True)
        (original / "Content/BulkContent").mkdir(parents=True)
        (original / "Content/Maps").mkdir(parents=True)
        (localized / "Builds/Release").mkdir(parents=True)
        (localized / "Content/Sounds_Windows").mkdir(parents=True)
        (localized / "Content/Maps").mkdir(parents=True)
        (original / "Content/BinkMovies").mkdir(parents=True)
        (localized / "Content/BinkMovies").mkdir(parents=True)
        for spec in json.loads((ROOT / "config/modules.json").read_text())["modules"]:
            if "source" not in spec:  # pinned game modules (exe, binkw32, d3dx9_33); system DLLs come from "source"
                shutil.copy2(Path("C:/Games/BioShock") / spec["path"], original / spec["path"])
        (original / "Builds/Release/Default.ini").write_text("[Engine.Engine]\nLanguage=int\n")
        (original / "Builds/Release/analysis.id0").write_bytes(b"not a game input")
        (original / "Content/BulkContent/test.blk").write_bytes(bytes(range(256)) * 32)
        (original / "Content/Maps/test.bsm").write_bytes(b"MAP" * 1000)
        (localized / "Builds/Release/Default.ini").write_text("[Engine.Engine]\nLanguage=rus\n")
        (localized / "Content/Sounds_Windows/voice.rus_fsb").write_bytes(b"FSB3" + bytes(500))
        (localized / "Content/Maps/test_rus.bsm").write_bytes(b"\xc1\x83\x2a\x9e" + b"RUS" * 300)
        (original / "Content/BinkMovies/PlaneSequence.bik").write_bytes(b"BIKi" + b"ENGLISH" * 100)
        (original / "Content/BinkMovies/PlaneSequence.bik").write_bytes(b"BIKi" + b"RUSSIAN" * 100)
        output = root / "obb"
        manifest = build_data_obb.build_parts(original, localized, output)
        build_data_obb.verify_parts(output)
        records = {entry["path"]: entry for entry in manifest["files"]}
        assert records["game/Builds/Release/Default.ini"]["origin"] == "original"
        assert "game/Content/Maps/test_rus.bsm" not in records
        assert records["game/Content/BinkMovies/PlaneSequence.bik"]["origin"] == "original"
        assert "game/Builds/Release/analysis.id0" not in records
        modules = json.loads((ROOT / "config/modules.json").read_text())["modules"]
        for spec in modules:
            report = json.loads((ROOT / "generated" / spec["name"] / "report.json").read_text())
            assert records["images/" + spec["file"]]["source_sha256"] == report["image_sha256"]
        for relative, source in [
            ("game/Content/BulkContent/test.blk", original / "Content/BulkContent/test.blk"),
            ("game/Content/Maps/test.bsm", original / "Content/Maps/test.bsm"),
            ("game/Builds/Release/Default.ini", original / "Builds/Release/Default.ini"),
            ("game/Content/BinkMovies/PlaneSequence.bik", original / "Content/BinkMovies/PlaneSequence.bik")]:
            entry = records[relative]
            with zipfile.ZipFile(output / entry["part"]) as archive:
                assert entry["zip_offset"] % 16384 == 0
                with archive.open(relative) as stream:
                    assert stream.read() == source.read_bytes()
                with path_open(archive, relative) as raw:
                    assert raw.read(entry["bytes"]) == source.read_bytes()
        print(f"PASS OBB direct offsets, CRC, optional Russian data excluded, sidecar exclusion and {len(records)} input records")


def path_open(archive, name):
    info = archive.getinfo(name)
    class Raw:
        def __enter__(self):
            archive.fp.seek(build_data_obb.data_offset(info, archive))
            return archive.fp
        def __exit__(self, *args):
            return False
    return Raw()


if __name__ == "__main__":
    main()

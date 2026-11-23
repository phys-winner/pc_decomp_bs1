"""Package original BioShock data and staged Russian files as direct-read OBBs."""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import zipfile

ROOT = Path(__file__).resolve().parents[1]
EXE_SHA256 = "7f2ee821ad60eaf08322b4bc6ebbf839d7fe010b4d0255ec12b5be4a2966f7f6"
SIDECARS = {".id0", ".id1", ".id2", ".nam", ".til"}


def sha(path):
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def source_files(original, localization):
    executable = original / "Builds/Release/Bioshock.exe"
    if not executable.is_file() or sha(executable) != EXE_SHA256:
        raise ValueError(f"Original executable identity mismatch: {executable}")
    entries = {}
    for top in ("Builds", "Content"):
        for path in sorted((original / top).rglob("*")):
            if path.is_file() and path.suffix.lower() not in SIDECARS:
                relative = path.relative_to(original)
                entries[str(relative).casefold()] = (path, "game/" + relative.as_posix(), "original")
    for path in sorted(localization.rglob("*")):
        # Shared Flash fonts/widgets belong to the port; Russian text/voice/movies are a separate optional ZIP.
        if path.is_file() and path.suffix.lower() == ".swf":
            relative = path.relative_to(localization)
            entries[str(relative).casefold()] = (path, "game/" + relative.as_posix(), "localization")
    modules = json.loads((ROOT / "config/modules.json").read_text())["modules"]
    for spec in modules:
        original_source = Path(spec["source"]) if "source" in spec else original / spec["path"]
        if not original_source.is_file() or sha(original_source) != spec["sha256"]:
            raise ValueError(f"Pinned module mismatch: {original_source}")
        report = json.loads((ROOT / "generated" / spec["name"] / "report.json").read_text())
        source = ROOT / "generated/input" / spec["file"]
        if (report.get("status") != "GENERATED" or report.get("original_sha256") != spec["sha256"] or
                not source.is_file() or sha(source) != report.get("image_sha256")):
            raise ValueError(f"Pre-based AOT image mismatch: {source}")
        entries[spec["name"]] = (source, "images/" + spec["file"], "aot-module")
    return list(entries.values())


def data_offset(info, archive):
    import struct
    position = archive.fp.tell()
    archive.fp.seek(info.header_offset + 26)
    name_bytes, extra_bytes = struct.unpack("<HH", archive.fp.read(4))
    archive.fp.seek(position)
    return info.header_offset + 30 + name_bytes + extra_bytes


def build_parts(original, localization, output):
    output.mkdir(parents=True, exist_ok=True)
    inputs = source_files(original, localization)
    records = []
    parts = []
    for part, name in enumerate(("main.1.org.bioshock.port.obb", "patch.1.org.bioshock.port.obb")):
        selected = [entry for entry in inputs if (entry[1].startswith("game/Builds/") or entry[1].startswith("game/Content/BulkContent/")) == (part == 0)]
        path = output / name
        with zipfile.ZipFile(path, "w", compression=zipfile.ZIP_STORED, allowZip64=True) as archive:
            for source, relative, origin in selected:
                info = zipfile.ZipInfo(relative, (2007, 8, 21, 0, 0, 0))
                info.compress_type = zipfile.ZIP_STORED
                info.create_system = 0
                info.external_attr = 0o100444 << 16
                info.extra = bytes((-archive.start_dir - 30 - len(relative.encode("utf-8"))) % 16384)
                with source.open("rb") as src, archive.open(info, "w") as dst:
                    shutil.copyfileobj(src, dst, 8 * 1024 * 1024)
                records.append({"path": relative, "part": name, "origin": origin,
                                "zip_offset": data_offset(info, archive), "bytes": source.stat().st_size,
                                "source_sha256": sha(source)})
        if path.stat().st_size >= 4 * 1024 ** 3:
            raise ValueError(f"OBB exceeds the 4-GiB per-container limit: {path}")
        parts.append({"path": name, "bytes": path.stat().st_size, "sha256": sha(path)})
    manifest = {"package": "org.bioshock.port", "version": 1, "compression": "STORED",
                "alignment": 16384, "original_exe_sha256": EXE_SHA256,
                "cache_prepared": True, "cache_runtime_verified": False,
                "parts": parts, "files": records}
    (output / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
    return manifest


def verify_parts(output):
    manifest = json.loads((output / "manifest.json").read_text())
    expected = {entry["path"]: entry for entry in manifest["files"]}
    for part in manifest["parts"]:
        path = output / part["path"]
        if sha(path) != part["sha256"]:
            raise ValueError(f"OBB hash mismatch: {path}")
        with zipfile.ZipFile(path) as archive:
            if archive.testzip() is not None:
                raise ValueError(f"OBB CRC mismatch: {path}")
            for info in archive.infolist():
                entry = expected[info.filename]
                if (info.compress_type != zipfile.ZIP_STORED or info.file_size != entry["bytes"] or
                        data_offset(info, archive) != entry["zip_offset"] or data_offset(info, archive) % 16384):
                    raise ValueError(f"Direct-read layout mismatch: {info.filename}")
    if len(expected) != len({key.casefold() for key in expected}):
        raise ValueError("Case-insensitive duplicate OBB paths")
    return manifest


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, default=Path("C:/Games/BioShock"))
    parser.add_argument("--localization", type=Path, default=ROOT / "build/localization")
    parser.add_argument("--output", type=Path, default=ROOT / "build/obb")
    parser.add_argument("--check", action="store_true")
    args = parser.parse_args()
    if args.check:
        manifest = verify_parts(args.output)
    else:
        build_parts(args.source, args.localization, args.output)
        manifest = verify_parts(args.output)
    print(json.dumps({"status": "CACHE_PREPARED", "parts": manifest["parts"],
                      "files": len(manifest["files"]), "runtime_verified": manifest["cache_runtime_verified"]}, indent=2))


if __name__ == "__main__":
    main()

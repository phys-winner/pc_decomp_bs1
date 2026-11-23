"""Validate and stage BioShock 2007 Russian data without touching the installation."""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import struct
import tempfile
import zipfile

ROOT = Path(__file__).resolve().parents[1]
EXPECTED_EXE = "7f2ee821ad60eaf08322b4bc6ebbf839d7fe010b4d0255ec12b5be4a2966f7f6"


def sha(path):
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def parse_lbf(path):
    raw = path.read_bytes()
    if not raw:
        raise ValueError(f"Empty LBF: {path}")

    def read_header(offset):
        if offset + 7 > len(raw):
            raise ValueError(f"Truncated LBF header: {path}")
        count = raw[offset]
        header = offset + 1 + count * 2 + 4
        if count < 2 or count > 127 or header > len(raw):
            raise ValueError(f"Invalid LBF name/header: {path}")
        name = raw[offset + 1:offset + 1 + count * 2].decode("utf-16-le")
        if (not name.isascii() or not name.endswith("\0") or not name[:-1].replace("_", "").replace("-", "").replace(".", "").isalnum()
                or "/" in name or "\\" in name or not name[:-1].lower().endswith((".int", ".rus"))):
            raise ValueError(f"Invalid LBF internal filename: {name!r}")
        metadata, = struct.unpack_from("<I", raw, offset + 1 + count * 2)
        return name, metadata, header

    def is_ini(payload):
        if payload.startswith(b"\xff\xfe"):
            return len(payload) >= 4 and payload[2] == ord("[")
        return payload.startswith(b"[")

    first_name, first_metadata, payload_offset = read_header(0)
    records = []
    position = 0
    while position < len(raw):
        name, metadata, start = read_header(position)
        if start >= len(raw) or not is_ini(raw[start:start + 4]):
            raise ValueError(f"LBF has no INI payload: {path}")
        end = len(raw)
        if name.lower().endswith(".rus\0"):
            for candidate in range(start + 2, len(raw) - 7):
                try:
                    _, _, next_start = read_header(candidate)
                except (ValueError, UnicodeDecodeError):
                    continue
                if is_ini(raw[next_start:next_start + 4]):
                    end = candidate
                    break
        records.append({"internal_name": name[:-1], "metadata": metadata,
                        "payload_offset": start, "payload_bytes": end - start})
        position = end
    return {"bytes": len(raw), "internal_name": first_name[:-1], "version": first_metadata,
            "payload_bytes": sum(record["payload_bytes"] for record in records),
            "records": records, "sha256": sha(path)}


def validate_fsb(path):
    with path.open("rb") as stream:
        header = stream.read(48)
    if len(header) < 48 or header[:4] != b"FSB3":
        raise ValueError(f"Not an FSB3 sound bank: {path}")
    samples, directory_bytes = struct.unpack_from("<II", header, 4)
    if not samples or directory_bytes < 48 or directory_bytes > path.stat().st_size:
        raise ValueError(f"Invalid FSB3 directory: {path}")
    return {"samples": samples, "directory_bytes": directory_bytes,
            "bytes": path.stat().st_size, "sha256": sha(path)}


def validate_package(path, original):
    """A localized level package (<level>_rus.bsm) must be an Unreal package of the same engine build as the
    original <level>_int.bsm. The game loads <level>_<language>.bsm with no fallback to _int, so without these
    packages every in-memory voice bank of the level (for example Ryan's bathysphere speech) stays silent."""
    if not path.name.lower().endswith("_rus.bsm"):
        raise ValueError(f"Not a Russian level package: {path}")
    reference = original / "Content/Maps" / (path.name[:-len("_rus.bsm")] + "_int.bsm")
    matches = [candidate for candidate in reference.parent.glob("*.bsm") if candidate.name.lower() == reference.name.lower()]
    if len(matches) != 1:
        raise ValueError(f"No original companion package for {path.name}")
    with path.open("rb") as stream:
        header = stream.read(12)
    with matches[0].open("rb") as stream:
        expected = stream.read(12)
    if len(header) < 12 or header[:4] != b"\xc1\x83\x2a\x9e" or header[4:8] != expected[4:8]:
        raise ValueError(f"Package version differs from {matches[0].name}: {path}")
    version, licensee = struct.unpack_from("<HH", header, 4)
    return {"package_version": version, "licensee_version": licensee, "companion": matches[0].name,
            "bytes": path.stat().st_size, "sha256": sha(path)}


def validate_bink(path, original):
    reference = original / "Content/BinkMovies" / path.name
    if not reference.is_file():
        raise ValueError(f"No original companion movie for {path.name}")
    with path.open("rb") as stream:
        header = stream.read(44)
    with reference.open("rb") as stream:
        expected = stream.read(44)
    if len(header) != 44 or len(expected) != 44:
        raise ValueError(f"Truncated Bink header: {path}")
    signature, size, frames, largest, repeated, width, height, rate, divisor, flags, tracks = struct.unpack("<4s10I", header)
    if (signature != expected[:4] or signature[:3] != b"BIK" or size + 8 != path.stat().st_size or
            not frames or repeated != frames or not largest or not width or not height or not rate or not divisor or
            not 0 < tracks <= 256 or 44 + tracks * 12 + (frames + 1) * 4 > size + 8 or
            header[20:28] != expected[20:28]):
        raise ValueError(f"Invalid or incompatible localized Bink movie: {path}")
    return {"signature": signature.decode("ascii"), "frames": frames, "width": width, "height": height,
            "frame_rate": [rate, divisor], "audio_tracks": tracks, "companion": reference.name,
            "bytes": path.stat().st_size, "sha256": sha(path)}


def edit_default(path):
    raw = path.read_bytes()
    encoding = "utf-16" if raw.startswith((b"\xff\xfe", b"\xfe\xff")) else "utf-8-sig" if raw.startswith(b"\xef\xbb\xbf") else "utf-8"
    text = raw.decode(encoding)
    lines = text.splitlines(keepends=True)
    sections = {}
    for index, line in enumerate(lines):
        clean = line.strip()
        if clean.startswith("[") and clean.endswith("]"):
            sections[clean[1:-1].lower()] = index
    language_index = sections.get("languages")
    engine_index = sections.get("engine.engine")
    if language_index is None or engine_index is None:
        raise ValueError("Default.ini lacks [Languages] or [Engine.Engine]")
    end = next((i for i in range(language_index + 1, len(lines))
                if lines[i].lstrip().startswith("[")), len(lines))
    engine_end = next((i for i in range(engine_index + 1, len(lines))
                       if lines[i].lstrip().startswith("[")), len(lines))
    if not any(line.strip().lower() == "language=rus" for line in lines[language_index:end]):
        insert = next((i for i in range(end - 1, language_index, -1)
                       if lines[i].strip().lower().startswith("language=")), end - 1)
        newline = "\r\n" if "\r\n" in lines[insert] else "\n"
        lines.insert(insert + 1, "language=rus" + newline)
    matches = [i for i in range(engine_index + 1, engine_end)
               if lines[i].strip().lower().startswith("language=")]
    if len(matches) != 1:
        raise ValueError("Default.ini must have exactly one [Engine.Engine] Language value")
    newline = "\r\n" if "\r\n" in lines[matches[0]] else "\n"
    lines[matches[0]] = "Language=rus" + newline
    path.write_bytes("".join(lines).encode(encoding))


def stage(source, original, out, provenance, include_voice=True, voice_source=None):
    exe = original / "Builds/Release/Bioshock.exe"
    if not exe.is_file() or sha(exe) != EXPECTED_EXE:
        raise ValueError("Pinned BioShock 1.1 executable is missing or has the wrong hash")
    lbf = source / "Content/Localizedrus.lbf"
    if not lbf.is_file():
        raise ValueError(f"Missing official Russian text: {lbf}")
    lbf_info = parse_lbf(lbf)
    voice_root = voice_source or source
    banks = sorted((voice_root / "Content/Sounds_Windows").glob("*_audio.rus_fsb"))
    if include_voice and not banks:
        raise ValueError("No Russian *_audio.rus_fsb voice banks found")
    packages = sorted((voice_root / "Content/Maps").glob("*_rus.bsm")) if include_voice else []
    movies = sorted((voice_root / "Content/BinkMovies").glob("*.bik")) if include_voice else []
    if include_voice and not packages:
        raise ValueError("No Russian *_rus.bsm level packages found (in-memory voice banks)")
    flash = sorted((source / "Content/FlashMovies").glob("*.swf"))
    records = []
    out.mkdir(parents=True, exist_ok=True)
    voice_files = {*banks, *packages, *movies}
    for path in [lbf, *flash, *banks, *packages, *movies]:
        relative = path.relative_to(voice_root if path in voice_files else source)
        if relative.parts[0] not in ("Content", "Builds") or ".." in relative.parts:
            raise ValueError(f"Unsafe localization path: {relative}")
        if path == lbf:
            info = lbf_info
        elif path.suffix.lower() == ".bsm":
            info = validate_package(path, original)
        elif path.suffix.lower() == ".bik":
            info = validate_bink(path, original)
        elif path.suffix == ".swf":
            with path.open("rb") as stream:
                header = stream.read(8)
            declared = struct.unpack_from("<I", header, 4)[0] if len(header) == 8 else 0
            if len(header) != 8 or header[:3] not in (b"FWS", b"CWS", b"ZWS") or declared != path.stat().st_size:
                raise ValueError(f"Invalid Flash resource: {path}")
            info = {"signature": header[:3].decode("ascii"), "bytes": path.stat().st_size, "sha256": sha(path)}
        else:
            info = validate_fsb(path)
        destination = out / relative
        destination.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(path, destination)
        records.append({"path": relative.as_posix(), **info})
    optional = [source / "Builds/Release/DefUser_rus.ini", source / "Content/System/bindings.rus"]
    for path in optional:
        if path.is_file():
            relative = path.relative_to(source)
            destination = out / relative
            destination.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(path, destination)
            records.append({"path": relative.as_posix(), "bytes": path.stat().st_size, "sha256": sha(path)})
    default = out / "Builds/Release/Default.ini"
    default.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(original / "Builds/Release/Default.ini", default)
    edit_default(default)
    text = default.read_text(encoding="utf-8-sig")
    assert "language=rus" in text and "Language=rus" in text
    manifest = {"game": "BioShock 2007 PC 1.1", "language": "rus", "provenance": provenance,
                "content_scope": "text-and-voice" if include_voice else "text-only",
                "original_exe_sha256": EXPECTED_EXE, "default_ini_sha256": sha(default),
                "files": records, "runtime_verified": False}
    (out / "manifest.json").write_text(json.dumps(manifest, indent=2, ensure_ascii=False) + "\n")
    return manifest


def self_test(original):
    reference_lbf = original / "Content/Localizedint.lbf"
    reference_fsb = original / "Content/Sounds_Windows/streams_1_audio.fsb"
    reference_package = original / "Content/Maps/0-Lighthouse_int.bsm"
    assert parse_lbf(reference_lbf)["internal_name"] == "Bioshock.int"
    assert validate_fsb(reference_fsb)["samples"] > 0
    with tempfile.TemporaryDirectory(prefix="bioshock-locale-test-", dir=ROOT / "build") as temporary:
        source = Path(temporary)
        (source / "Content/Sounds_Windows").mkdir(parents=True)
        (source / "Content/Maps").mkdir(parents=True)
        (source / "Content/BinkMovies").mkdir(parents=True)
        shutil.copy2(reference_lbf, source / "Content/Localizedrus.lbf")
        shutil.copy2(reference_fsb, source / "Content/Sounds_Windows/streams_1_audio.rus_fsb")
        shutil.copy2(reference_package, source / "Content/Maps/0-lighthouse_rus.bsm")
        reference_movie = original / "Content/BinkMovies/PlaneSequence.bik"
        shutil.copy2(reference_movie, source / "Content/BinkMovies/PlaneSequence.bik")
        (source / "Content/Maps/1-welcome_rus.bsm").write_bytes(b"\xc1\x83\x2a\x9e\x8c\x00\x38\x00" + bytes(64))
        try:
            stage(source, original, source / "rejected", {"source": "self-test-only"})
        except ValueError as error:
            assert "Package version differs" in str(error), error
        else:
            raise AssertionError("a package from another engine build was staged")
        (source / "Content/Maps/1-welcome_rus.bsm").unlink()
        manifest = stage(source, original, source / "staged", {"source": "self-test-only"})
        assert len(manifest["files"]) == 4 and not manifest["runtime_verified"]
        package = next(entry for entry in manifest["files"] if entry["path"] == "Content/Maps/0-lighthouse_rus.bsm")
        assert package["companion"] == "0-Lighthouse_int.bsm" and package["package_version"] == 141
        assert (source / "staged/Content/Maps/0-lighthouse_rus.bsm").read_bytes() == reference_package.read_bytes()
        movie = next(entry for entry in manifest["files"] if entry["path"] == "Content/BinkMovies/PlaneSequence.bik")
        assert movie["companion"] == "PlaneSequence.bik" and movie["audio_tracks"] > 0
        assert (source / "staged/Content/BinkMovies/PlaneSequence.bik").read_bytes() == reference_movie.read_bytes()
        (source / "Content/BinkMovies/PlaneSequence.bik").write_bytes(b"BIKi" + bytes(40))
        try:
            validate_bink(source / "Content/BinkMovies/PlaneSequence.bik", original)
        except ValueError:
            pass
        else:
            raise AssertionError("a corrupt Bink movie was accepted")
        default = source / "staged/Builds/Release/Default.ini"
        assert default.is_file()
        assert default.read_bytes().startswith(b"[Startup]")
        assert b"PersistentPackages=ShockGame" in default.read_bytes()
    print("bioshock_locale: synthetic staging PASS (text, stream bank, level package, Bink movie, corrupt/foreign data rejected); "
          "original files unchanged; real Russian data not verified")


def build_optional_pack(staged, output):
    """A data-only ZIP to merge into the game folder, without replacing English movies or INIs."""
    manifest = json.loads((staged / "manifest.json").read_text(encoding="utf-8"))
    records = []
    output.parent.mkdir(parents=True, exist_ok=True)
    with zipfile.ZipFile(output, "w", zipfile.ZIP_STORED, allowZip64=True) as archive:
        for entry in manifest["files"]:
            relative = Path(entry["path"])
            if relative.is_absolute() or ".." in relative.parts: raise ValueError("Unsafe localization path")
            if relative.suffix.lower() not in {".lbf", ".rus_fsb", ".bsm", ".bik"}: continue
            source = staged / relative
            if sha(source) != entry["sha256"]: raise ValueError(f"Localization changed: {source}")
            target = relative.with_name(relative.stem + "_rus.bik") if relative.suffix.lower() == ".bik" else relative
            archive.write(source, target.as_posix())
            records.append({"path": target.as_posix(), "bytes": source.stat().st_size, "sha256": entry["sha256"]})
    if not records: raise ValueError("No language data to package")
    with zipfile.ZipFile(output) as archive:
        if archive.testzip() is not None: raise ValueError("Language pack CRC failure")
    receipt = {"language": "rus", "sha256": sha(output), "files": records,
               "install": "Extract into the BioShock game folder, then select Русский in the launcher."}
    output.with_suffix(".json").write_text(json.dumps(receipt, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    return receipt


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, help="Extracted data-only Russian localization directory")
    parser.add_argument("--original", type=Path, default=Path("C:/Games/BioShock"))
    parser.add_argument("--output", type=Path, default=ROOT / "build/localization")
    parser.add_argument("--provenance", default="user-supplied data; URL not recorded")
    parser.add_argument("--self-test", action="store_true")
    parser.add_argument("--text-only", action="store_true", help="Stage text/Flash without requiring Russian voice banks")
    parser.add_argument("--voice-source", type=Path, help="Separate extracted directory containing Russian FSB banks")
    parser.add_argument("--pack", type=Path, help="Package --output (already staged) as an optional Russian ZIP")
    args = parser.parse_args()
    if args.self_test:
        self_test(args.original)
        return
    if args.pack:
        manifest = build_optional_pack(args.output.resolve(), args.pack.resolve())
        print(json.dumps({"path": str(args.pack), "sha256": manifest["sha256"], "files": len(manifest["files"])}))
        return
    if not args.source:
        parser.error("--source is required unless --self-test is used")
    manifest = stage(args.source.resolve(), args.original.resolve(), args.output.resolve(), args.provenance,
                     include_voice=not args.text_only, voice_source=args.voice_source.resolve() if args.voice_source else None)
    print(json.dumps(manifest, indent=2, ensure_ascii=False))


if __name__ == "__main__":
    main()

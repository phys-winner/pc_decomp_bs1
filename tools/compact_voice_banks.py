"""Replace Russian FSB3 banks in a BioShock ZIP with seekable deltas from the English banks."""

import argparse
from concurrent.futures import ThreadPoolExecutor
import hashlib
import json
from pathlib import Path
import re
import shutil
import struct
import subprocess
import tempfile
import zipfile
import zlib


PREFIX = "Content/Sounds_Windows/"
BANKS = [f"{PREFIX}streams_{bank}_audio" for bank in (*range(8), 99, "aivo")]
HEADER = struct.Struct("<4sIIII")  # Magic, target size, base size, base prefix CRC, extent count
EXTENT = struct.Struct("<II")       # Exclusive target end, source offset (high bit selects English)
ENGLISH = 1 << 31
MAX_BANK = 256 << 20
MAX_EXTENTS = 65536
INSTRUCTION = rb"(RUN|ADD|CPY_\d)\s+(\d+)(?:\s+([ST])@(\d+))?"
ROW = re.compile(rb"\s+(\d+)\s+\d+\s+" + INSTRUCTION + rb"(?:\s+" + INSTRUCTION + rb")?\s*")


def english_ranges(xdelta, base, target, diff):
    # Use the whole English bank as the source window, including shifted samples.
    window = max(1 << 20, 1 << (base.stat().st_size - 1).bit_length())
    subprocess.run([xdelta, "-e", "-D", "-N", "-3", "-S", "none", "-B", str(window),
                    "-s", base, target, diff], check=True)
    position = 0
    ranges = []
    with subprocess.Popen([xdelta, "printdelta", diff], stdout=subprocess.PIPE) as process:
        for line in process.stdout:
            if not line.lstrip()[:1].isdigit():
                continue
            match = ROW.fullmatch(line)
            if not match or int(match[1]) != position:
                raise ValueError(f"Unexpected xdelta instruction in {target.name}: {line!r}")
            for opcode, size, space, address in (match.groups()[1:5], match.groups()[5:9]):
                if opcode is None:
                    break
                length = int(size)
                if space == b"S" and length >= 64:
                    source = int(address)
                    if ranges and ranges[-1][1] == position and ranges[-1][2] + position - ranges[-1][0] == source:
                        start, _, source = ranges.pop()
                    else:
                        start = position
                    ranges.append((start, position + length, source))
                position += length
        if process.wait() or position != target.stat().st_size or not ranges:
            raise ValueError(f"Could not match complete source ranges for {target.name}")
    return ranges


def encode(base, target, patch, ranges):
    original = memoryview(base.read_bytes())
    localized = memoryview(target.read_bytes())
    if not all(4 < len(bank) <= MAX_BANK and bank[:4] == b"FSB3" for bank in (original, localized)):
        raise ValueError(f"Unexpected FSB3 input: {target.name}")
    extents = []
    literals = []
    position = 0
    for start, end, source in ranges:
        if not position <= start < end <= len(localized) or not 0 <= source <= len(original) - (end - start):
            raise ValueError(f"Invalid copy range in {target.name}")
        if start > position:
            extents.append((start, False, len(literals)))
            literals.append(localized[position:start])
        extents.append((end, True, source))
        position = end
    if position < len(localized):
        extents.append((len(localized), False, len(literals)))
        literals.append(localized[position:])
    if not 0 < len(extents) <= MAX_EXTENTS:
        raise ValueError(f"Invalid extent count for {target.name}")

    table_end = HEADER.size + EXTENT.size * len(extents)
    position = 0
    literal_offset = table_end
    with patch.open("wb") as out:
        out.write(HEADER.pack(b"FSD1", len(localized), len(original), zlib.crc32(original[:65536]), len(extents)))
        for end, english, offset in extents:
            out.write(EXTENT.pack(end, ENGLISH | offset if english else literal_offset))
            if not english:
                literal_offset += len(literals[offset])
        for literal in literals:
            out.write(literal)
    # Decode the written file and compare the whole bank, independently of the match list.
    expected = hashlib.sha256(localized).hexdigest()
    recovered = hashlib.sha256()
    with patch.open("rb") as inp:
        inp.seek(HEADER.size)
        records = list(EXTENT.iter_unpack(inp.read(table_end - HEADER.size)))
        for end, source in records:
            size = end - position
            if source & ENGLISH:
                source ^= ENGLISH
                recovered.update(original[source:source + size])
            else:
                inp.seek(source)
                recovered.update(inp.read(size))
            position = end
    compact = patch.stat().st_size
    if recovered.hexdigest() != expected or compact >= len(localized):
        raise ValueError(f"Invalid or oversized delta for {target.name}")
    return {"bank": target.name, "original": len(localized), "compact": compact,
            "sha256": expected, "base_sha256": hashlib.sha256(original).hexdigest(), "extents": len(extents)}


def build(source, destination, xdelta, seven_zip):
    if destination.exists():
        raise ValueError("Choose a new output ZIP")
    with tempfile.TemporaryDirectory(prefix="bioshock-banks-", dir=destination.resolve().parent) as tmp:
        stage = Path(tmp)
        (stage / PREFIX).mkdir(parents=True)
        with zipfile.ZipFile(source) as archive:
            original = {item.filename: (item.file_size, item.CRC) for item in archive.infolist() if not item.is_dir()}
            for bank in BANKS:
                for extension in (".fsb", ".rus_fsb"):
                    archive.extract(bank + extension, stage)

        def compact(bank):
            path = stage / bank
            base, target = path.with_suffix(".fsb"), path.with_suffix(".rus_fsb")
            ranges = english_ranges(xdelta, base, target, path.with_suffix(".vcdiff"))
            report = encode(base, target, path.with_suffix(".rus_fsd"), ranges)
            print(json.dumps(report), flush=True)
            return report

        with ThreadPoolExecutor(max_workers=4) as pool:
            reports = list(pool.map(compact, BANKS))
        draft = stage / "archive.zip"
        shutil.copyfile(source, draft)
        removed = {bank + ".rus_fsb" for bank in BANKS}
        added = [bank + ".rus_fsd" for bank in BANKS]
        subprocess.run([seven_zip, "d", draft, *sorted(removed), "-bsp0"], check=True)
        subprocess.run([seven_zip, "a", draft, *added, "-tzip", "-mm=Deflate", "-mx=6", "-bsp0"], cwd=stage, check=True)
        with zipfile.ZipFile(draft) as archive:
            entries = {item.filename: (item.file_size, item.CRC) for item in archive.infolist() if not item.is_dir()}
            if entries.keys() != (original.keys() - removed) | set(added) or any(
                    entries[name] != meta for name, meta in original.items() if name not in removed):
                raise ValueError("Unrelated ZIP entries changed")
            for item in added:
                if archive.read(item) != (stage / item).read_bytes():
                    raise ValueError(f"ZIP bytes differ: {item}")
        if draft.stat().st_size >= source.stat().st_size:
            raise ValueError("Compact ZIP did not get smaller")
        saved = source.stat().st_size - draft.stat().st_size
        draft.rename(destination.resolve())
        return {"archive_bytes_saved": saved, "installed_bytes_saved": sum(
            bank["original"] - bank["compact"] for bank in reports), "banks": reports}


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", type=Path)
    parser.add_argument("destination", type=Path)
    parser.add_argument("--xdelta", default="xdelta3")
    parser.add_argument("--seven-zip", default="7z")
    args = parser.parse_args()
    print(json.dumps(build(args.source, args.destination, args.xdelta, args.seven_zip), indent=2))

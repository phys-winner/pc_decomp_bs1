"""Inventory the original BioShock 2007 installation without modifying it."""
import argparse
from collections import Counter
import hashlib
import json
from pathlib import Path
import re

import pefile

PROJECT = Path(__file__).resolve().parents[1]
DEFAULT_ROOT = Path(r"C:\Games\BioShock")


def digest(path):
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def text(value):
    if value is None:
        return None
    return value.decode("utf-8", errors="replace") if isinstance(value, bytes) else value


def describe(path, root):
    pe = pefile.PE(str(path))
    base = pe.OPTIONAL_HEADER.ImageBase
    result = {
        "path": path.relative_to(root).as_posix(),
        "size": path.stat().st_size,
        "sha256": digest(path),
        "machine": f"0x{pe.FILE_HEADER.Machine:04x}",
        "optional_magic": f"0x{pe.OPTIONAL_HEADER.Magic:x}",
        "image_base": f"0x{base:08x}",
        "entry_point": f"0x{base + pe.OPTIONAL_HEADER.AddressOfEntryPoint:08x}",
        "image_size": pe.OPTIONAL_HEADER.SizeOfImage,
        "timestamp": pe.FILE_HEADER.TimeDateStamp,
        "sections": [
            {
                "name": text(section.Name.rstrip(b"\0")),
                "rva": f"0x{section.VirtualAddress:x}",
                "virtual_size": section.Misc_VirtualSize,
                "raw_size": section.SizeOfRawData,
                "characteristics": f"0x{section.Characteristics:08x}",
                "entropy": round(section.get_entropy(), 3),
            }
            for section in pe.sections
        ],
        "imports": [
            {
                "module": text(module.dll).lower(),
                "symbols": [
                    {
                        "name": text(symbol.name),
                        "ordinal": symbol.ordinal,
                        "iat_va": f"0x{symbol.address:08x}",
                    }
                    for symbol in module.imports
                ],
            }
            for module in getattr(pe, "DIRECTORY_ENTRY_IMPORT", [])
        ],
        "delay_imports": [text(module.dll).lower() for module in getattr(pe, "DIRECTORY_ENTRY_DELAY_IMPORT", [])],
        "exports": [
            {
                "name": text(symbol.name),
                "ordinal": symbol.ordinal,
                "address": f"0x{base + symbol.address:08x}",
                "forwarder": text(symbol.forwarder),
            }
            for symbol in getattr(getattr(pe, "DIRECTORY_ENTRY_EXPORT", None), "symbols", [])
        ],
        "relocation_entries": sum(len(block.entries) for block in getattr(pe, "DIRECTORY_ENTRY_BASERELOC", [])),
        "warnings": pe.get_warnings(),
    }
    raw = path.read_bytes()
    result["dll_strings"] = sorted({text(value).lower() for value in re.findall(rb"[A-Za-z0-9_.-]{2,100}\.dll", raw)})
    result["pdb_paths"] = [
        text(debug.entry.PdbFileName.rstrip(b"\0"))
        for debug in getattr(pe, "DIRECTORY_ENTRY_DEBUG", [])
        if hasattr(debug, "entry") and hasattr(debug.entry, "PdbFileName")
    ]
    pe.close()
    return result


def verify(root, manifest):
    failures = []
    for module in manifest["modules"]:
        path = root / module["path"]
        if not path.is_file():
            failures.append(f"Missing input: {path}")
        elif path.stat().st_size != module["size"] or digest(path) != module["sha256"]:
            failures.append(f"Changed input: {path}")
    if failures:
        raise SystemExit("\n".join(failures))
    print(f"Verified {len(manifest['modules'])} pinned input files.")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, default=DEFAULT_ROOT)
    parser.add_argument("--verify", action="store_true")
    args = parser.parse_args()
    root = args.root.resolve(strict=True)
    manifest_path = PROJECT / "config/inputs.json"
    if args.verify:
        verify(root, json.loads(manifest_path.read_text(encoding="utf-8")))
        return
    release = root / "Builds/Release"
    paths = sorted(
        (path for path in release.iterdir() if path.is_file() and path.suffix.lower() in {".exe", ".dll"}),
        key=lambda path: str(path).lower(),
    )
    modules = [describe(path, root) for path in paths]
    game = next(module for module in modules if module["path"].lower() == "builds/release/bioshock.exe")
    if game["machine"] != "0x014c" or game["optional_magic"] != "0x10b":
        raise SystemExit("Bioshock.exe is not a PE32 i386 image")
    manifest = {
        "schema_version": 1,
        "edition": "GOG / BioShock PC patch 1.1 / build 127339",
        "source_root": str(root),
        "main_module": "Builds/Release/Bioshock.exe",
        "modules": [
            {key: module[key] for key in ("path", "size", "sha256", "machine", "image_base", "entry_point", "image_size")}
            for module in modules
        ],
    }
    assets = {}
    for directory in ("Builds", "Content"):
        files = [path for path in (root / directory).rglob("*") if path.is_file()]
        by_extension = Counter(path.suffix.lower() or "(none)" for path in files)
        assets[directory] = {
            "files": len(files),
            "bytes": sum(path.stat().st_size for path in files),
            "extensions": dict(sorted(by_extension.items())),
        }
    report = {"modules": modules, "assets": assets}
    for path, content in ((manifest_path, manifest), (PROJECT / "research/binary-analysis.json", report)):
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(json.dumps(content, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    verify(root, manifest)
    print(
        f"BioShock: {game['machine']}, base {game['image_base']}, entry {game['entry_point']}, "
        f"{len(game['imports'])} static and {len(game['delay_imports'])} delay import modules"
    )


if __name__ == "__main__":
    main()

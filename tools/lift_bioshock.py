"""Lift the pinned BioShock 1.1 PE images to C; originals stay read-only."""
import argparse
import hashlib
import json
from pathlib import Path
import struct
import sys

import pefile

from lift_game import collect_functions, emit_dispatch, emit_functions
from pe_rebase import rebase_image

ROOT = Path(__file__).resolve().parents[1]


def digest(raw):
    return hashlib.sha256(raw).hexdigest()


def write_json(path, value):
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_suffix(path.suffix + ".tmp")
    temporary.write_text(json.dumps(value, indent=2) + "\n", encoding="utf-8")
    temporary.replace(path)


def code_address(pe, address):
    section = pe.get_section_by_rva(address - pe.OPTIONAL_HEADER.ImageBase)
    return section is not None and bool(section.Characteristics & 0x20000000)


def module_jump_tables(spec):
    tables = {}
    for row in spec.get("jump_tables", []):
        address = int(row["address"], 0)
        if address in tables:
            raise ValueError(f"Duplicate declared jump table {address:#x}")
        tables[address] = (row["first_index"], row["count"])
    return tables


def metadata_roots(pe):
    base = pe.OPTIONAL_HEADER.ImageBase
    roots = {
        base + symbol.address
        for symbol in getattr(getattr(pe, "DIRECTORY_ENTRY_EXPORT", None), "symbols", [])
        if not symbol.forwarder and code_address(pe, base + symbol.address)
    }
    for block in getattr(pe, "DIRECTORY_ENTRY_BASERELOC", []):
        for relocation in block.entries:
            if relocation.type != 3:
                continue
            section = pe.get_section_by_rva(relocation.rva)
            if section is None or section.Characteristics & 0x20000000:
                continue
            raw = pe.get_data(relocation.rva, 4)
            if len(raw) != 4:
                raise ValueError("Truncated relocation pointer")
            pointer = struct.unpack("<I", raw)[0]
            if code_address(pe, pointer):
                roots.add(pointer)
    load_config = getattr(pe, "DIRECTORY_ENTRY_LOAD_CONFIG", None)
    if load_config:
        table = getattr(load_config.struct, "SEHandlerTable", 0)
        count = getattr(load_config.struct, "SEHandlerCount", 0)
        if count > 1000000:
            raise ValueError("Unreasonable SafeSEH table length")
        if count:
            raw = pe.get_data(table - base, count * 4)
            if len(raw) != count * 4:
                raise ValueError("Truncated SafeSEH table")
            for (rva,) in struct.iter_unpack("<I", raw):
                if not code_address(pe, base + rva):
                    raise ValueError(f"SafeSEH handler is not executable: {base + rva:#x}")
                roots.add(base + rva)
    tls = getattr(pe, "DIRECTORY_ENTRY_TLS", None)
    if tls and tls.struct.AddressOfCallBacks:
        table = tls.struct.AddressOfCallBacks - base
        for index in range(1024):
            raw = pe.get_data(table + index * 4, 4)
            if len(raw) != 4:
                raise ValueError("Truncated TLS callback table")
            address = struct.unpack("<I", raw)[0]
            if not address:
                break
            if not code_address(pe, address):
                raise ValueError("TLS callback outside code")
            roots.add(address)
        else:
            raise ValueError("Unterminated TLS callback table")
    return roots


def prepare(spec, source_root):
    source = Path(spec["source"]) if "source" in spec else source_root / spec["path"]
    original = source.read_bytes()
    if digest(original) != spec["sha256"]:
        raise ValueError(f"Original SHA-256 mismatch: {source}")
    pe = pefile.PE(data=original, fast_load=True)
    if pe.FILE_HEADER.Machine != 0x14C or pe.OPTIONAL_HEADER.Magic != 0x10B:
        raise ValueError(f"Expected PE32 i386: {source}")
    base = int(spec["base"], 0)
    old_base = pe.OPTIONAL_HEADER.ImageBase
    raw = original
    if old_base != base:
        raw, _ = rebase_image(original, base)
        if rebase_image(raw, old_base)[0] != original:
            raise ValueError(f"Reverse relocation failed: {source}")
    if spec.get("jump_tables"):
        rebased = pefile.PE(data=raw, fast_load=True)
        try:
            for row in spec["jump_tables"]:
                rva = int(row["address"], 0) - base + 4 * row["first_index"]
                size = 4 * row["count"]
                section = rebased.get_section_by_rva(rva)
                if size <= 0 or section is None or rva + size > section.VirtualAddress + section.SizeOfRawData:
                    raise ValueError("Declared jump table is outside file-backed PE data")
                if digest(rebased.get_data(rva, size)) != row["sha256"]:
                    raise ValueError(f"Declared jump table SHA-256 mismatch: {row['address']}")
        finally:
            rebased.close()
    target = ROOT / "generated/input" / spec["file"]
    target.parent.mkdir(parents=True, exist_ok=True)
    target.write_bytes(raw)
    return target, digest(raw)


def merge_dispatch(specs):
    entries, headers, module_lines = [], [], []
    for spec in specs:
        directory = ROOT / "generated" / spec["name"]
        report_path = directory / "report.json"
        if not report_path.exists():
            continue
        report = json.loads(report_path.read_text())
        if report.get("status") != "GENERATED":
            continue
        if digest((ROOT / "generated/input" / spec["file"]).read_bytes()) != report["image_sha256"]:
            raise ValueError(f"Stale generated image: {spec['file']}")
        entries.extend(int(address, 0) for address in report["dispatch_entries"])
        headers.append(f'{spec["name"]}/functions.h')
        headers.extend(f'{spec["name"]}/{header}' for header in report.get("supplemental_headers", []))
        module_lines.append('{"%s", "%s", %su, %d, 0},' % (
            spec["file"], spec["file"], spec["base"], int(spec.get("exe", False))))
    if not entries:
        raise ValueError("No generated modules")
    emit_dispatch(ROOT / "generated/dispatch.c", entries, headers)
    (ROOT / "generated/modules.inc").write_text("\n".join(module_lines) + "\n")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--module", action="append", help="Module name from config/modules.json")
    args = parser.parse_args()
    inputs = json.loads((ROOT / "config/inputs.json").read_text())
    specs = json.loads((ROOT / "config/modules.json").read_text())["modules"]
    known = {module["name"] for module in specs}
    if args.module and set(args.module) - known:
        parser.error("Unknown module: " + ", ".join(sorted(set(args.module) - known)))
    for spec in specs:
        if args.module and spec["name"] not in args.module:
            continue
        directory = ROOT / "generated" / spec["name"]
        directory.mkdir(parents=True, exist_ok=True)
        report_path = directory / "report.json"
        previous = json.loads(report_path.read_text()) if report_path.exists() else {}
        write_json(report_path, {"status": "GENERATING"})
        binary, image_hash = prepare(spec, Path(inputs["source_root"]))
        pe = pefile.PE(str(binary))
        catalog = ROOT / "config" / f'{spec["name"]}.functions.json'
        document = json.loads(catalog.read_text()) if catalog.exists() else {}
        rows = document.get("functions", [])
        excluded = set(document.get("data_entries", []))
        if rows:
            if document.get("module") != spec["file"] or document.get("original_sha256") != spec["sha256"]:
                raise ValueError(f"IDA catalog does not match the pinned module: {spec['file']}")
            if "image_sha256" in document and document["image_sha256"] != image_hash:
                raise ValueError(f"IDA catalog does not match the prepared image: {spec['file']}")
            if excluded & {row["address"] for row in rows}:
                raise ValueError(f"IDA function and data catalogs overlap: {spec['file']}")
        roots = (metadata_roots(pe) - excluded) | {int(address, 0) for address in previous.get("supplemental_entries", [])}
        print(f'{spec["name"]}: {len(roots)} metadata roots, {len(rows)} catalog entries, '
              f'{len(excluded)} data entries', flush=True)
        # BioShock.exe has no .reloc. Its data sections still contain vtables and
        # callback pointers, so retain the checked pointer scan until the IDA
        # catalog replaces it. Other modules use their narrower metadata roots.
        pointer_entries = None if spec.get("exe") and not rows else ()
        info, imports, decoder, functions, rejected = collect_functions(
            binary,
            rows,
            roots,
            label=spec["file"],
            compact=True,
            pointer_entries=pointer_entries,
            excluded_entries=excluded,
            jump_tables=module_jump_tables(spec),
        )
        for module in getattr(pe, "DIRECTORY_ENTRY_DELAY_IMPORT", []):
            for symbol in module.imports:
                imports[symbol.address] = (
                    module.dll.decode(),
                    symbol.name.decode() if symbol.name else f"#{symbol.ordinal}",
                )
        report = emit_functions(
            directory,
            imports,
            decoder,
            functions,
            label=spec["file"],
            header_name="functions.h",
            chunk_prefix="functions",
        )
        report.update(
            status="GENERATED",
            module=spec["file"],
            original_sha256=spec["sha256"],
            image_sha256=image_hash,
            metadata_roots=len(roots),
            rejected_entries=rejected,
            dispatch_entries=[hex(address) for address in sorted(functions)],
        )
        supplemental = previous.get("supplemental_entries", [])
        if supplemental:
            report["supplemental_generation"] = previous.get("supplemental_generation", 0)
            report["supplemental_headers"] = previous.get("supplemental_headers", [])
            report["supplemental_entries"] = supplemental
            report["dispatch_entries"] = sorted(set(report["dispatch_entries"]) | set(supplemental))
            report["sources"].extend(name for name in previous.get("sources", []) if name.startswith("supplemental_"))
        report["functions"] = len(report["dispatch_entries"])
        write_json(report_path, report)
        pe.close()
    merge_dispatch(specs)


if __name__ == "__main__":
    main()

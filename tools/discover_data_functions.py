"""Read-only audit of code pointers in BioShock's non-executable PE sections."""
import json
from pathlib import Path
import struct

import pefile

from lift_game import analyze_pe, build_iat_map, BoundedDisassembler
from lift_bioshock import ROOT, digest, module_jump_tables


def main():
    specs = json.loads((ROOT / "config/modules.json").read_text())["modules"]
    spec = next(module for module in specs if module.get("exe"))
    report = json.loads((ROOT / "generated" / spec["name"] / "report.json").read_text())
    known = {int(address, 0) for address in report["dispatch_entries"]}
    binary = ROOT / "generated/input" / spec["file"]
    image_hash = digest(binary.read_bytes())
    if image_hash != report["image_sha256"]:
        raise ValueError("Prepared PE differs from the generated module")
    info = analyze_pe(str(binary))
    imports = build_iat_map(info)
    pe = pefile.PE(str(binary))
    for module in getattr(pe, "DIRECTORY_ENTRY_DELAY_IMPORT", []):
        for symbol in module.imports:
            imports[symbol.address] = (module.dll.decode(), symbol.name.decode() if symbol.name else f"#{symbol.ordinal}")
    decoder = BoundedDisassembler(binary.read_bytes(), info.image_base, info.sections,
                                  jump_tables=module_jump_tables(spec))
    candidates = set()
    for section in info.sections:
        if section.is_code:
            continue
        data = binary.read_bytes()[section.raw_offset:section.raw_offset + section.raw_size]
        for offset in range(0, len(data) - 3, 4):
            pointer = struct.unpack_from("<I", data, offset)[0]
            if pointer not in known and decoder.is_code_address(pointer):
                candidates.add(pointer)
    valid, rejected = [], []
    for index, address in enumerate(sorted(candidates), 1):
        function = decoder.disassemble_function(address, imports)
        (valid if function is not None and function.blocks else rejected).append(address)
        if index % 100 == 0:
            print(f"Checked {index}/{len(candidates)} candidates", flush=True)
    result = {
        "module": spec["file"],
        "image_sha256": image_hash,
        "candidates": len(candidates),
        "valid": [hex(address) for address in valid],
        "rejected": [hex(address) for address in rejected],
    }
    output = ROOT / "research/data-function-candidates.json"
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    print(f"Candidates {len(candidates)}; decodable {len(valid)}; rejected {len(rejected)}; report {output}")
    pe.close()


if __name__ == "__main__":
    main()

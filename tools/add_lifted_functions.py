"""Add independently verified function entries without regenerating the whole game."""
import argparse
import json
from pathlib import Path

import pefile

from lift_game import analyze_pe, build_iat_map, BoundedDisassembler, emit_functions
from lift_bioshock import ROOT, merge_dispatch, module_jump_tables, prepare, write_json


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("addresses", nargs="+", type=lambda value: int(value, 0))
    parser.add_argument("--module", default="bioshock")
    args = parser.parse_args()
    specs = json.loads((ROOT / "config/modules.json").read_text())["modules"]
    spec = next(module for module in specs if module["name"] == args.module)
    inputs = json.loads((ROOT / "config/inputs.json").read_text())
    directory = ROOT / "generated" / args.module
    report_path = directory / "report.json"
    report = json.loads(report_path.read_text())
    if report.get("status") != "GENERATED":
        raise ValueError("A complete module is required")
    addresses = [int(value, 0) for value in report["dispatch_entries"]]
    new = [address for address in args.addresses if address not in addresses]
    if len(new) != len(set(new)):
        raise ValueError("Duplicate requested entries")
    if not new:
        print("All requested entries are already present")
        return
    binary, image_hash = prepare(spec, Path(inputs["source_root"]))
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
    functions = {}
    for address in new:
        function = decoder.disassemble_function(address, imports)
        if function is None or not function.blocks:
            raise ValueError(f"Cannot decode verified entry {address:#x}")
        functions[address] = function
    serial = report.get("supplemental_generation", 0) + 1
    header = f"supplemental_{serial:04d}.h"
    emitted = emit_functions(directory, imports, decoder, functions,
        label=spec["file"], header_name=header, chunk_prefix=f"supplemental_{serial:04d}",
        additional_entries=addresses)
    report["supplemental_generation"] = serial
    report.setdefault("supplemental_headers", []).append(header)
    report["sources"].extend(emitted["sources"])
    report["dispatch_entries"] = [hex(address) for address in sorted(addresses + new)]
    report["functions"] = len(report["dispatch_entries"])
    for key in ("emitted_instructions_with_overlap", "eh_owners", "cold_eh_added_instructions"):
        report[key] = report.get(key, 0) + emitted[key]
    traps = set(report["functions_containing_traps"]) | set(emitted["functions_containing_traps"])
    report["functions_containing_traps"] = sorted(traps, key=lambda value: int(value, 0))
    report.setdefault("supplemental_entries", []).extend(hex(address) for address in new)
    write_json(report_path, report)
    pe.close()
    merge_dispatch(specs)
    print(f"Added {len(new)} entries to {args.module}: " + ", ".join(hex(address) for address in new))


if __name__ == "__main__":
    main()

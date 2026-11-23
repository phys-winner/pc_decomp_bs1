"""Remove only independently rejected supplemental entries and refresh dispatch."""
import argparse
import json
from pathlib import Path
import re

from lift_bioshock import ROOT, merge_dispatch, write_json


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("addresses", nargs="+", type=lambda value: int(value, 0))
    parser.add_argument("--module", default="bioshock")
    args = parser.parse_args()
    specs = json.loads((ROOT / "config/modules.json").read_text())["modules"]
    spec = next(module for module in specs if module["name"] == args.module)
    directory = ROOT / "generated" / args.module
    report_path = directory / "report.json"
    report = json.loads(report_path.read_text())
    if report.get("status") != "GENERATED":
        raise ValueError("A complete module is required")
    requested = set(args.addresses)
    existing = {int(value, 0) for value in report["dispatch_entries"]}
    supplemental = {int(value, 0) for value in report.get("supplemental_entries", [])}
    if requested - existing or requested - supplemental:
        raise ValueError("Every removal must be an existing supplemental entry: " +
                         ", ".join(hex(value) for value in sorted(requested - existing | requested - supplemental)))
    removed_instructions = 0
    for header in report.get("supplemental_headers", []):
        header_path = directory / header
        declarations = header_path.read_text()
        for address in sorted(requested):
            declarations, count = re.subn(rf"^void sub_{address:08X}\(void\);\n", "", declarations, flags=re.M)
            if count > 1:
                raise ValueError(f"Duplicate declaration for {address:#x}")
        header_path.write_text(declarations)
    for relative in report["sources"]:
        if "supplemental_" not in relative:
            continue
        source = directory / relative
        text = source.read_text()
        for address in sorted(requested):
            pattern = rf"^void sub_{address:08X}\(void\) \{{\n.*?^\}}\n"
            match = re.search(pattern, text, flags=re.M | re.S)
            if match:
                removed_instructions += len(re.findall(r"/\* 0x[0-9a-fA-F]+:", match.group(0)))
                text = text[:match.start()] + text[match.end():]
        source.write_text(text)
    remaining = sorted(existing - requested)
    report["dispatch_entries"] = [hex(value) for value in remaining]
    report["functions"] = len(remaining)
    report["supplemental_entries"] = [value for value in report.get("supplemental_entries", [])
                                      if int(value, 0) not in requested]
    report["functions_containing_traps"] = [value for value in report["functions_containing_traps"]
                                             if int(value, 0) not in requested]
    report["emitted_instructions_with_overlap"] -= removed_instructions
    if report["emitted_instructions_with_overlap"] < 0:
        raise ValueError("Instruction accounting became negative")
    write_json(report_path, report)
    merge_dispatch(specs)
    print(f"Removed {len(requested)} entries from {args.module}: " +
          ", ".join(hex(value) for value in sorted(requested)))


if __name__ == "__main__":
    main()

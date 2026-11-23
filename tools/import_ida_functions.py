"""Convert IDA's batch assembly into a SHA-bound BioShock function catalog."""
import argparse
import hashlib
import json
from pathlib import Path
import re

import pefile

PROJECT = Path(__file__).resolve().parents[1]
PATTERN = re.compile(rb"^sub_([0-9A-Fa-f]{8})\s+proc\s", re.MULTILINE)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("assembly", type=Path)
    parser.add_argument("--input", type=Path, default=Path(r"C:\Games\BioShock\Builds\Release\Bioshock.exe"))
    parser.add_argument("--output", type=Path, default=PROJECT / "config/bioshock.functions.json")
    args = parser.parse_args()
    raw = args.input.read_bytes()
    image_sha = hashlib.sha256(raw).hexdigest()
    assembly = args.assembly.read_bytes()
    if hashlib.sha256(raw).hexdigest().upper().encode() not in assembly[:512]:
        raise ValueError("IDA assembly does not identify the pinned input SHA-256")
    pe = pefile.PE(data=raw)
    executable = [
        (pe.OPTIONAL_HEADER.ImageBase + section.VirtualAddress,
         pe.OPTIONAL_HEADER.ImageBase + section.VirtualAddress + max(section.Misc_VirtualSize, section.SizeOfRawData))
        for section in pe.sections if section.Characteristics & 0x20000000
    ]
    addresses = sorted({int(match, 16) for match in PATTERN.findall(assembly)})
    invalid = [address for address in addresses if not any(low <= address < high for low, high in executable)]
    if invalid:
        raise ValueError(f"IDA functions outside executable sections: {invalid[:8]}")
    entry = pe.OPTIONAL_HEADER.ImageBase + pe.OPTIONAL_HEADER.AddressOfEntryPoint
    if entry not in addresses:
        raise ValueError(f"IDA catalog lacks the PE entry point {entry:#x}")
    if len(addresses) < 1000:
        raise ValueError(f"Implausibly small IDA function catalog: {len(addresses)}")
    result = {
        "schema_version": 1,
        "module": args.input.name,
        "original_sha256": image_sha,
        "ida_assembly": str(args.assembly),
        "ida_assembly_sha256": hashlib.sha256(assembly).hexdigest(),
        "functions": [{"address": address} for address in addresses],
    }
    args.output.parent.mkdir(parents=True, exist_ok=True)
    temporary = args.output.with_suffix(".json.tmp")
    temporary.write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    temporary.replace(args.output)
    print(f"Imported {len(addresses)} IDA functions; entry {entry:#x}; SHA {image_sha}")


if __name__ == "__main__":
    main()

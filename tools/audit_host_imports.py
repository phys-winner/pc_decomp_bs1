"""Compare BioShock's static imports with the registered Win32 host shims."""
import json
from pathlib import Path
import re

import pefile

PROJECT = Path(__file__).resolve().parents[1]
INPUT = Path(r"C:\Games\BioShock\Builds\Release\Bioshock.exe")


def implemented():
    result = set()
    aliases = {}
    for path in (PROJECT / "android/host").glob("*.c"):
        text = path.read_text(encoding="utf-8")
        aliases[path.name] = dict(re.findall(r'^#define\s+([A-Za-z_][A-Za-z0-9_]*)\s+"([^"]+)"', text, re.M))
        for match in re.finditer(r'\b(WINAPI_EXPORT|CDECL_EXPORT|RAW_EXPORT)\s*\(\s*("[^"]+"|[A-Za-z_][A-Za-z0-9_]*)\s*,\s*([A-Za-z_][A-Za-z0-9_]*)\s*[,)]', text):
            macro, dll, name = match.groups()
            dll = dll.strip('"') if dll.startswith('"') else aliases[path.name].get(dll)
            if dll:
                result.add((dll.lower(), name.lower(), 0))
        for match in re.finditer(r'\bWINAPI_ORDINAL\s*\(\s*"([^"]+)"\s*,\s*([A-Za-z_][A-Za-z0-9_]*)\s*,\s*(\d+)\s*,', text):
            dll, name, ordinal = match.groups()
            result.add((dll.lower(), name.lower(), int(ordinal)))
        for match in re.finditer(r'\bWINAPI_EXPORT_AS\s*\(\s*("[^"]+"|[A-Za-z_][A-Za-z0-9_]*)\s*,\s*([A-Za-z_][A-Za-z0-9_]*)\s*,\s*"([^"]+)"\s*,\s*(\d+)\s*,', text):
            dll, symbol, name, ordinal = match.groups()
            dll = dll.strip('"') if dll.startswith('"') else aliases[path.name].get(dll)
            if dll:
                result.add((dll.lower(), name.lower(), int(ordinal)))
    return result


def main():
    pe = pefile.PE(str(INPUT))
    have = implemented()
    missing = []
    covered = []
    for module in getattr(pe, "DIRECTORY_ENTRY_IMPORT", []):
        dll = module.dll.decode().lower()
        for symbol in module.imports:
            name = symbol.name.decode() if symbol.name else None
            ordinal = 0 if name else symbol.ordinal
            found = any(dll == d and ((name and n == name.lower()) or (not name and o == ordinal)) for d, n, o in have)
            row = {"dll": dll, "name": name, "ordinal": symbol.ordinal, "iat_va": f"0x{symbol.address:08x}"}
            (covered if found else missing).append(row)
    delay = []
    for module in getattr(pe, "DIRECTORY_ENTRY_DELAY_IMPORT", []):
        for symbol in module.imports:
            delay.append({"dll": module.dll.decode().lower(), "name": symbol.name.decode() if symbol.name else None, "ordinal": symbol.ordinal})
    report = {"input": str(INPUT), "covered": covered, "missing": missing, "delay_imports": delay, "host_shims": len(have)}
    output = PROJECT / "research/host-import-audit.json"
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(f"Host shims: {len(have)}; covered: {len(covered)}; missing: {len(missing)}; delay: {len(delay)}")
    for row in missing:
        print(f"{row['dll']}!{row['name'] or '#' + str(row['ordinal'])}")


if __name__ == "__main__":
    main()

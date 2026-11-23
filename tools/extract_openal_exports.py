"""Record the production OpenAL bridge API and verify its native exports."""
import hashlib
import json
from pathlib import Path
import re
import subprocess

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "android/host/openal.c"
LIBRARY = ROOT / "build/native-audio/runtime/libopenal.so"
READELF = Path.home() / "AppData/Local/Android/Sdk/ndk/27.2.12479018/toolchains/llvm/prebuilt/windows-x86_64/bin/llvm-readelf.exe"


def main():
    source = SOURCE.read_text(encoding="utf-8")
    block = re.search(r"#define OPENAL_ROUTES\(X\) \\\n(.*?)\n\ntypedef struct", source, re.S)
    if not block:
        raise ValueError("OPENAL_ROUTES block not found")
    names = re.findall(r"X\((al(?:c)?[A-Z][A-Za-z0-9]*)\)", block.group(1))
    if len(names) != 118 or len(set(names)) != 118:
        raise ValueError(f"Expected 118 unique bridge APIs, got {len(names)}")
    output = subprocess.check_output([str(READELF), "--dyn-syms", "--wide", str(LIBRARY)], text=True)
    exports = {match.group(1) for line in output.splitlines()
               if (match := re.search(r"\b(al(?:c)?[A-Z][A-Za-z0-9]*)$", line))}
    missing = [name for name in names if name not in exports]
    if missing:
        raise ValueError("Missing native exports: " + ", ".join(missing))
    report = {
        "source": SOURCE.relative_to(ROOT).as_posix(),
        "library": LIBRARY.relative_to(ROOT).as_posix(),
        "library_sha256": hashlib.sha256(LIBRARY.read_bytes()).hexdigest(),
        "apis": [{"name": name, "group": "AL" if index < 85 else "ALC_EXT_EFX"}
                 for index, name in enumerate(names)],
    }
    destination = ROOT / "research/openal-dynamic-exports.json"
    destination.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(json.dumps({"output": str(destination), "apis": len(names), "native_exports_checked": len(exports)}))


if __name__ == "__main__":
    main()

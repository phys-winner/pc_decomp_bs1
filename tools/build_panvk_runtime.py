"""Stage the pinned Mesa PanVK driver of the native Vulkan renderer (renderer mode "vulkan-panvk").

The driver is the shared reference of all ports in ../launcher/drivers/panvk (MANIFEST.json names the
sources, patch and recipe; owner's decision of 24 November 2026). This tool verifies the pins of
dependencies-panvk.json, checks that the shared manifest describes the pinned library and states the
Android level the manifest pins, copies the driver and its notice into build/panvk-runtime and writes the
receipt tools/build_android.py checks. Nothing is downloaded or built.
"""
import hashlib
import json
from pathlib import Path
import shutil
import struct

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / "build/panvk-runtime"


def sha(path):
    with Path(path).open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def main():
    manifest = ROOT / "dependencies-panvk.json"
    pins = json.loads(manifest.read_text())
    OUT.mkdir(parents=True, exist_ok=True)
    outputs = []
    library = None
    for name, spec in pins["files"].items():
        source = (ROOT / name).resolve()
        if not source.is_file() or sha(source) != spec["sha256"]:
            raise ValueError("Pinned PanVK file is missing or changed: " + name)
        if source.name == "MANIFEST.json":
            shared = json.loads(source.read_text(encoding="utf-8"))
            continue
        if "target" not in spec:
            continue
        target = OUT / spec["target"]
        if target.suffix == ".so":
            data = source.read_bytes()
            if data[:5] != b"\x7fELF\x02" or struct.unpack_from("<HH", data, 16) != (3, 183):
                raise ValueError("The PanVK driver is not an AArch64 ELF DSO")
            library = spec["sha256"]
        if not target.is_file() or sha(target) != spec["sha256"]:
            shutil.copyfile(source, target)
        outputs.append({"path": target.relative_to(ROOT).as_posix(), "sha256": spec["sha256"],
                        "bytes": target.stat().st_size})
    if shared["library"]["sha256"] != library:
        raise ValueError("The shared PanVK manifest describes another library than the pinned one")
    if shared["requirements"]["min_android_api"] != pins["min_android_api"]:
        raise ValueError("dependencies-panvk.json and the shared manifest name different Android levels")
    receipt = {"manifest_sha256": sha(manifest), "tool_sha256": sha(Path(__file__)), "outputs": outputs,
               "min_android_api": pins["min_android_api"], "source": pins["source"]}
    (OUT / "build-receipt.json").write_text(json.dumps(receipt, indent=2) + "\n")
    print(json.dumps(receipt, indent=2))


if __name__ == "__main__":
    main()

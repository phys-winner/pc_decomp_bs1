"""Stage BioShock's measured Mesa PanVK build from the verified shared ELF and a forward Git binary patch.

Run python tools/build_panvk_runtime.py from a checkout beside the shared launcher. The native Android build
invokes this step automatically. dependencies-panvk.json pins the shared and final ELF, notice, and both patches.

To rebuild the driver from source on Linux or WSL, use the NDK r29, Meson, Rust and bindgen toolchain
documented in ../launcher/drivers/panvk/MANIFEST.json. From the BioShock checkout use a fresh directory:

    recipe=$(mktemp -d)
    cp ../launcher/drivers/panvk/build_panvk.sh "$recipe/"
    cat ../launcher/drivers/panvk/panvk-r10.patch tools/panvk-jm-occlusion.patch > "$recipe/panvk-r10.patch"
    XDG_CACHE_HOME="$recipe/cache" sh "$recipe/build_panvk.sh" "$ANDROID_NDK_HOME"

The result is $recipe/build/libvulkan_panfrost-fixed.so. Build paths and toolchain versions enter the ELF.
The binary patch reproduces the measured bytes across hosts. Its first section comes from git diff --binary
--full-index between the two versions of libvulkan_panfrost.so in an isolated checkout.
"""
import hashlib
import json
from pathlib import Path
import shutil
import struct
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / "build/panvk-runtime"


def sha(path):
    with Path(path).open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def check_driver(path):
    with path.open("rb") as stream:
        header = stream.read(20)
    if len(header) != 20 or header[:6] != b"\x7fELF\x02\x01" or struct.unpack_from("<HH", header, 16) != (3, 183):
        raise ValueError("The PanVK driver is not a little-endian AArch64 ELF DSO")


def main():
    manifest = ROOT / "dependencies-panvk.json"
    pins = json.loads(manifest.read_text(encoding="utf-8"))
    files, variant = pins["files"], pins["variant"]
    source_patch_sha = files[variant["source_patch"]]["sha256"]
    binary_patch_sha = files[variant["binary_patch"]]["sha256"]
    for name, spec in files.items():
        path = ROOT / name
        if not path.is_file() or sha(path) != spec["sha256"]:
            raise ValueError("Pinned PanVK file is missing or changed: " + name)
    shared_dir = Path(pins["source"])
    shared = json.loads((ROOT / shared_dir / "MANIFEST.json").read_text(encoding="utf-8"))
    base = (shared_dir / shared["library"]["file"]).as_posix()
    notice = (shared_dir / shared["licence"]["file"]).as_posix()
    if shared["library"]["sha256"] != files[base]["sha256"] or shared["library"]["bytes"] != (ROOT / base).stat().st_size:
        raise ValueError("The shared PanVK manifest describes another library than the pinned one")
    if shared["licence"]["sha256"] != files[notice]["sha256"]:
        raise ValueError("The shared PanVK manifest describes a different notice")
    if shared["requirements"]["min_android_api"] != pins["min_android_api"]:
        raise ValueError("dependencies-panvk.json and the shared manifest name different Android levels")
    check_driver(ROOT / base)

    OUT.mkdir(parents=True, exist_ok=True)
    outputs = []
    with tempfile.TemporaryDirectory(prefix=".stage-", dir=OUT) as tmp:
        stage = Path(tmp)
        for name, digest in ((base, variant["sha256"]), (notice, files[notice]["sha256"])):
            target = OUT / files[name]["target"]
            if (not target.is_file() or (name == base and target.stat().st_size != variant["bytes"])
                    or sha(target) != digest):
                candidate = stage / target.name
                shutil.copyfile(ROOT / name, candidate)
                if name == base:
                    # Explicit work tree keeps Git inside this temporary directory even within a parent checkout.
                    subprocess.run(["git", "--git-dir=.git", "--work-tree=.", "apply", "--binary",
                                    str(ROOT / variant["binary_patch"])], cwd=stage, check=True)
                    check_driver(candidate)
                if sha(candidate) != digest or (name == base and candidate.stat().st_size != variant["bytes"]):
                    raise ValueError("Staged PanVK file differs from its pin: " + target.name)
                candidate.replace(target)
            outputs.append({"path": target.relative_to(ROOT).as_posix(), "sha256": digest,
                            "bytes": target.stat().st_size})
        receipt = {"manifest_sha256": sha(manifest), "tool_sha256": sha(Path(__file__)), "outputs": outputs,
                   "min_android_api": pins["min_android_api"], "source": pins["source"],
                   "base_sha256": files[base]["sha256"], "source_patch_sha256": source_patch_sha,
                   "binary_patch_sha256": binary_patch_sha}
        data = (json.dumps(receipt, indent=2) + "\n").encode("utf-8")
        target = OUT / "build-receipt.json"
        if not target.is_file() or target.read_bytes() != data:
            candidate = stage / target.name
            candidate.write_bytes(data)
            candidate.replace(target)
    print(data.decode("utf-8"), end="")


if __name__ == "__main__":
    main()

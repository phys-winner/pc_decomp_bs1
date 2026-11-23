"""Stage the pinned Mesa PanVK driver of the native Vulkan renderer (renderer mode "vulkan-panvk").

The driver is the one the Frostpunk port builds and ships (dependencies-panvk.json names the recipe);
this tool only copies the pinned files into build/panvk-runtime and writes a receipt that
tools/build_android.py checks.  Nothing is downloaded or rebuilt here.
"""
import hashlib
import json
import shutil
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / "build/panvk-runtime"


def sha(path):
    with Path(path).open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def main():
    pins = json.loads((ROOT / "dependencies-panvk.json").read_text())
    source = (ROOT / pins["source"]).resolve()
    # An exact staged copy is independent of subsequent changes to the upstream recipe.
    missing = {name: spec for name, spec in pins["files"].items()
               if not (OUT / spec["target"]).is_file() or sha(OUT / spec["target"]) != spec["sha256"]}
    if missing:
        for name, expected in pins["recipe_sha256"].items():
            path = (ROOT / name).resolve()
            if not path.is_file() or sha(path) != expected:
                raise ValueError("PanVK recipe changed or missing: " + name)
        for name, spec in missing.items():
            src = source / name
            if not src.is_file() or sha(src) != spec["sha256"]:
                raise ValueError("pinned PanVK file missing or changed: %s" % src)
    OUT.mkdir(parents=True, exist_ok=True)
    outputs = []
    for name, spec in pins["files"].items():
        target = OUT / spec["target"]
        if name in missing:
            shutil.copyfile(source / name, target)
        outputs.append({"path": target.relative_to(ROOT).as_posix(), "sha256": spec["sha256"], "bytes": target.stat().st_size})
    receipt = {"manifest_sha256": sha(ROOT / "dependencies-panvk.json"), "tool_sha256": sha(Path(__file__)),
               "outputs": outputs}
    (OUT / "build-receipt.json").write_text(json.dumps(receipt, indent=2) + "\n")
    print(json.dumps(receipt, indent=2))


if __name__ == "__main__":
    main()

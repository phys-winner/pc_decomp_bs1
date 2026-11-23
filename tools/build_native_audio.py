"""Build and stage ARM64 OpenAL/Oboe from pinned sources, licenses and build provenance.

Staging uses the pinned Git submodules and local Android toolchain.
--rebuild replays the exact retained recipe in a private build directory; it
never runs its device fixture, and accepts only byte-identical pinned outputs.
"""
import argparse
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import shutil
import struct
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
REFERENCE = ROOT / "reference/ut3/reference/audio-runtime"
MANIFEST = ROOT / "dependencies-audio.json"
BUILD = ROOT / "build/native-audio"
AUDIO_API = 26


def sha(path):
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def verify(path, digest):
    if not path.is_file() or sha(path) != digest:
        raise ValueError("Native audio hash mismatch or missing file: " + str(path))


def copy_verified(source, destination, digest):
    verify(source, digest)
    if destination.exists():
        verify(destination, digest)
    else:
        destination.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(source, destination)
        verify(destination, digest)


def load(path):
    return json.loads(path.read_text(encoding="utf-8"))


def relpath(text):
    return Path(text.replace("\\", "/"))


def verify_snapshot(manifest):
    for relative, digest in manifest["files"].items():
        verify(ROOT / relative, digest)
    count = 0
    for name, source in manifest["sources"].items():
        info = load(ROOT / source["provenance"])
        if info["revision"] != source["revision"]:
            raise ValueError("Native audio revision changed: " + name)
        tree = ROOT / "reference" / name / "source"
        for relative, digest in info["source_tree_sha256"].items():
            verify(tree / relpath(relative), digest)
            count += 1
    for name, relative in manifest["runtime"].items():
        data = (ROOT / relative).read_bytes()[:64]
        if data[:6] != b"\x7fELF\x02\x01" or struct.unpack_from("<H", data, 18)[0] != 183:
            raise ValueError("Expected little-endian AArch64 ELF64: " + name)
    return count


def rebuild(manifest, api=None):
    config = dict(manifest["toolchain"])
    sdk = Path(os.environ.get("ANDROID_SDK_ROOT", Path.home() / "AppData/Local/Android/Sdk"))
    config["android_sdk"] = str(sdk)
    compiler = sdk / "ndk" / config["android_ndk_version"] / "toolchains/llvm/prebuilt/windows-x86_64/bin/clang++.exe"
    verify(compiler, config["compiler_sha256"])
    private = BUILD / ("rebuild" if api is None else f"rebuild-api{api}")
    for name in manifest["sources"]:
        shutil.copytree(ROOT / "reference" / name, private / "reference" / name, dirs_exist_ok=True,
                        ignore=shutil.ignore_patterns(".git", "__pycache__"))
    for name in ("metrics.cpp", "metrics.h", "native_probe.cpp"):
        target = private / "android/audio" / name
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(REFERENCE / "rebuild/android/audio" / name, target)
    recipe = private / "tools/build_native_audio.py"
    recipe.parent.mkdir(parents=True, exist_ok=True)
    source = (ROOT / manifest["upstream_recipe"]).read_text(encoding="utf-8")
    if api is not None:
        original = '"-DANDROID_PLATFORM=android-30"'
        if source.count(original) != 1:
            raise ValueError("Pinned OpenAL recipe's Android platform changed")
        source = source.replace(original, f'"-DANDROID_PLATFORM=android-{api}"')
    recipe.write_text(source, encoding="utf-8", newline="\n")
    (private / "research").mkdir(exist_ok=True)
    (private / "port.json").write_text(json.dumps(config, indent=2) + "\n")
    # Do not pass --run: the retained recipe's device fixture is never executed.
    subprocess.run([sys.executable, str(recipe)], cwd=private, check=True)
    runtime = private / "build/native-audio/runtime"
    if api is None:
        for name, relative in manifest["runtime"].items():
            verify(runtime / name, manifest["files"][relative])
    return runtime


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--rebuild", action="store_true")
    args = parser.parse_args()
    if not MANIFEST.is_file():
        parser.error("Missing dependencies-audio.json")
    manifest = load(MANIFEST)
    count = verify_snapshot(manifest)
    if args.rebuild:
        rebuild(manifest)
    manifest_sha, tool_sha = sha(MANIFEST), sha(Path(__file__))
    receipt = BUILD / "build-receipt.json"
    previous = load(receipt) if receipt.is_file() else {}
    runtime = BUILD / "runtime"
    if (previous.get("manifest_sha256") != manifest_sha or previous.get("tool_sha256") != tool_sha
            or previous.get("configuration", {}).get("api") != AUDIO_API
            or any(not (runtime / name).is_file() or sha(runtime / name) != previous.get("runtime", {}).get(name)
                   for name in manifest["runtime"])):
        rebuilt = rebuild(manifest, AUDIO_API)
        runtime.mkdir(parents=True, exist_ok=True)
        for name in manifest["runtime"]:
            shutil.copyfile(rebuilt / name, runtime / name)
    for name, relative in manifest["licenses"].items():
        copy_verified(ROOT / relative, BUILD / "licenses" / name, manifest["files"][relative])
    copy_verified(ROOT / manifest["upstream_receipt"], BUILD / "upstream-build-receipt.json",
                  manifest["files"][manifest["upstream_receipt"]])
    report = {"timestamp_utc": datetime.now(timezone.utc).isoformat(), "mode": "rebuild" if args.rebuild else "stage",
              "manifest_sha256": manifest_sha, "tool_sha256": tool_sha,
              "verified_source_files": count, "runtime": {name: sha(BUILD / "runtime" / name) for name in manifest["runtime"]},
              "licenses": {name: sha(BUILD / "licenses" / name) for name in manifest["licenses"]},
              "configuration": dict(manifest["configuration"], api=AUDIO_API), "device_commands_executed": False,
              "current_UT3_device_audio_verified": False}
    receipt.write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps(report, indent=2))


if __name__ == "__main__":
    main()

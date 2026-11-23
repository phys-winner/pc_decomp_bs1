"""Build the launcher's texture converter (libtexconv.so) and its device check tool (bioshock-tex).

libtexconv.so: android/texconv/bioshock_tex.cpp (BioShock's packages and bulk files) with the pinned codec of
the UT3/The Void ports (android/texconv/codec.cpp, astcenc with NEON, etcpak), loaded by
org.bioshock.port.TextureConverter in the launcher process.  bioshock-tex: the same converter as a command
(TEXCONV_CLI) for `adb shell` checks against the game folder or a stored ZIP on a device.  Both link android/host/zipmount.c, the
game's reader of the launcher's archive index.  Needs NDK 27.2 (ANDROID_NDK_HOME).
"""
from concurrent.futures import ThreadPoolExecutor
import hashlib
import json
import os
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / "build/texconv"
SDK = Path(os.environ.get("ANDROID_SDK_ROOT", Path.home() / "AppData/Local/Android/Sdk"))
NDK = Path(os.environ.get("ANDROID_NDK_HOME", SDK / "ndk/27.2.12479018"))
CXX = NDK / "toolchains/llvm/prebuilt/windows-x86_64/bin/clang++.exe"
CC = NDK / "toolchains/llvm/prebuilt/windows-x86_64/bin/clang.exe"
ASTC = ["-DASTCENC_NEON=1", "-DASTCENC_SVE=0", "-DASTCENC_SSE=0", "-DASTCENC_AVX=0", "-DASTCENC_POPCNT=0", "-DASTCENC_F16C=0"]
FLAGS = ["--target=aarch64-linux-android26", "-O2", "-fPIC", "-fvisibility=hidden", "-ffunction-sections", "-fdata-sections",
         "-std=c++17", "-ffile-prefix-map=" + str(ROOT) + "=."]
C_FLAGS = [flag if flag != "-std=c++17" else "-std=c11" for flag in FLAGS]


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    if "27." not in str(NDK):
        raise SystemExit(f"NDK 27.2 is required (ANDROID_NDK_HOME is {NDK})")
    manifest = ROOT / "dependencies-textures.json"
    pins = json.loads(manifest.read_text())["sources"]
    for name, digest in pins.items():
        if hashlib.sha256((ROOT / name).read_bytes().replace(b"\r\n", b"\n")).hexdigest() != digest:
            raise ValueError("Texture codec source differs from its pin: " + name)
    (OUT / "obj").mkdir(parents=True, exist_ok=True)
    sources = [(p, ASTC) for p in sorted((ROOT / "reference/astcenc/Source").glob("astcenc_*.cpp"))]
    sources += [(ROOT / "reference/etcpak" / n, ["-DBCDEC_STATIC", "-DBCDEC_IMPLEMENTATION"])
                for n in ("ProcessRGB.cpp", "Tables.cpp", "Dither.cpp", "Decode.cpp")]
    sources += [(ROOT / "android/texconv/codec.cpp", ASTC)]
    sources += [(ROOT / "android/host/zipmount.c", [])]
    converter = ROOT / "android/texconv/bioshock_tex.cpp"
    headers = sorted((ROOT / "android/texconv").glob("*.h")) + [ROOT / "android/host/zipmount.h"]
    digest = hashlib.sha256((sha(manifest) + str(CXX) + "".join(sha(p) for p in headers)).encode()).hexdigest()

    def compile(unit):
        source, extra, name = unit
        compiler, flags = (CC, C_FLAGS) if source.suffix == ".c" else (CXX, FLAGS)
        obj = OUT / "obj" / name
        stamp = obj.with_suffix(".sha256")
        key = hashlib.sha256((digest + sha(source) + json.dumps(flags + extra)).encode()).hexdigest()
        if not obj.is_file() or not stamp.is_file() or stamp.read_text() != key:
            subprocess.run([str(compiler), *flags, *extra, "-c", str(source), "-o", str(obj)], check=True)
            stamp.write_text(key)
        return obj

    units = [(s, extra, s.parent.name + "_" + s.stem + ".o") for s, extra in sources]
    units += [(converter, [], "bioshock_tex.o"), (converter, ["-DTEXCONV_CLI"], "bioshock_tex_cli.o")]
    with ThreadPoolExecutor(max_workers=6) as pool:
        objects = list(pool.map(compile, units))
    codec, library_obj, cli_obj = objects[:-2], objects[-2], objects[-1]   # codec: also zipmount.o
    link = [str(CXX), FLAGS[0], "-fuse-ld=lld", "-static-libstdc++", "-Wl,--gc-sections", "-Wl,--no-undefined",
            "-Wl,-z,max-page-size=16384", "-Wl,--build-id=sha1"]
    lib = OUT / "libtexconv.so"
    subprocess.run([*link, "-shared", *map(str, codec), str(library_obj), "-lz", "-llog", "-o", str(lib)], check=True)
    tool = OUT / "bioshock-tex"
    subprocess.run([*link, "-pie", *map(str, codec), str(cli_obj), "-lz", "-llog", "-o", str(tool)], check=True)
    receipt = {"sha256": sha(lib), "tool_sha256": sha(Path(__file__)), "cli_sha256": sha(tool), "manifest_sha256": sha(manifest),
               "sources": {p.relative_to(ROOT).as_posix(): sha(p) for p in headers + [s for s, _ in sources] + [converter]}}
    (OUT / "receipt.json").write_text(json.dumps(receipt, indent=2) + "\n")
    print(f"Texture converter: {lib} ({lib.stat().st_size} bytes), check tool {tool}")


if __name__ == "__main__":
    main()

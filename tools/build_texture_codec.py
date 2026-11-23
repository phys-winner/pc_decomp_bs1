"""Build the pinned UT3 DXT-to-ASTC/ETC2 codec for offline Windows cooking."""
from concurrent.futures import ThreadPoolExecutor
import hashlib
import json
import os
from pathlib import Path
import subprocess
import time

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / "build/texture-codec"


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    if os.name != "nt":
        raise SystemExit("This host builder requires Visual Studio C++ and the Windows SDK")
    pins = json.loads((ROOT / "dependencies-textures.json").read_text())["sources"]
    for name, digest in pins.items():
        if hashlib.sha256((ROOT/name).read_bytes().replace(b"\r\n", b"\n")).hexdigest() != digest:
            raise ValueError("Texture codec source differs from its pin: "+name)
    vswhere = Path(os.environ["ProgramFiles(x86)"]) / "Microsoft Visual Studio/Installer/vswhere.exe"
    installation = Path(subprocess.check_output([str(vswhere), "-latest", "-products", "*", "-property", "installationPath"], text=True).strip())
    msvc = sorted((installation/"VC/Tools/MSVC").iterdir())[-1]
    sdk = Path(os.environ["ProgramFiles(x86)"]) / "Windows Kits/10"
    version = sorted((sdk/"Lib").iterdir())[-1].name
    env = os.environ.copy()
    env["PATH"] = str(msvc/"bin/Hostx64/x64") + os.pathsep + env["PATH"]
    env["INCLUDE"] = ";".join(map(str, [msvc/"include", sdk/f"Include/{version}/ucrt", sdk/f"Include/{version}/shared", sdk/f"Include/{version}/um"]))
    env["LIB"] = ";".join(map(str, [msvc/"lib/x64", sdk/f"Lib/{version}/ucrt/x64", sdk/f"Lib/{version}/um/x64"]))
    astc, etc = ROOT/"reference/astcenc/Source", ROOT/"reference/etcpak"
    sources = sorted(p for p in astc.glob("astcenc_*.cpp") if not p.name.startswith("astcenccli"))
    sources += [etc/name for name in ("ProcessRGB.cpp", "Tables.cpp", "Dither.cpp", "Decode.cpp")]
    sources += [ROOT/"android/texconv/codec.cpp"]
    flags = ["/nologo", "/c", "/O2", "/std:c++17", "/EHsc", "/MD", "/DNOMINMAX", "/DWIN32_LEAN_AND_MEAN",
             "/DASTCENC_NEON=0", "/DASTCENC_SVE=0", "/DASTCENC_SSE=41", "/DASTCENC_AVX=0",
             "/DASTCENC_POPCNT=0", "/DASTCENC_F16C=0", "/D__SSE4_1__", "/DBCDEC_STATIC", "/DBCDEC_IMPLEMENTATION",
             "/I"+str(astc), "/I"+str(etc)]
    OUT.mkdir(parents=True, exist_ok=True)
    def compile(source):
        obj = OUT/(source.stem+".obj")
        command = [str(msvc/"bin/Hostx64/x64/cl.exe"), *flags, str(source), "/Fo"+str(obj)]
        result = subprocess.run(command, env=env, capture_output=True, text=True)
        (OUT/(source.stem+".log")).write_text(result.stdout+result.stderr)
        if result.returncode:
            raise RuntimeError(source.name+": "+result.stdout+result.stderr)
        return obj
    start = time.monotonic()
    with ThreadPoolExecutor(4) as pool:
        objects = list(pool.map(compile, sources))
    exports = ["texconv_convert_dds", "texconv_free", "texconv_decode_dxt", "texconv_decode_converted", "texconv_key"]
    library = OUT/"texconv.dll"
    subprocess.run([str(msvc/"bin/Hostx64/x64/link.exe"), "/NOLOGO", "/DLL", "/OUT:"+str(library),
                    *map(str, objects), *["/EXPORT:"+name for name in exports]], env=env, check=True)
    (OUT/"receipt.json").write_text(json.dumps(dict(seconds=time.monotonic()-start, sha256=sha(library),
        dependencies_sha256=sha(ROOT/"dependencies-textures.json"), sources={str(p.relative_to(ROOT)):sha(p) for p in sources}, flags=flags), indent=2)+"\n")
    print(f"PASS {library} ({library.stat().st_size} bytes)")


if __name__ == "__main__":
    main()

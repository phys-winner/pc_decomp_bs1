"""Build the BioShock ARM64 Android launcher and native runtime."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import ssl
import struct
import subprocess
import sys
import tempfile
import zipfile
from build_android import LAUNCHER, launcher_native_inputs, native_dependencies, sha
from patch_native_settings import build as build_native_settings
from patch_native_minigames import build as build_native_minigames, build_plasminow
from patch_hud_layout import build as build_native_hud, build_map
from patch_loading_indicator import build as build_loading_indicator
from elf_identity import assert_runtime_identity
import extract_ios_controls

ROOT = Path(__file__).resolve().parents[1]
NATIVE_PROFILE_DEFAULT_SHA256 = "da5b52de385974392d025cff06252357f8c150b85098682235bdf1c0b14ffac0"


def run(*args):
    subprocess.run([str(arg) for arg in args], cwd=ROOT, check=True)

def launcher_sources():
    sources = list((LAUNCHER / "android/src").rglob("*.java"))
    sources.extend(LAUNCHER / name for name in ("tools/launcher.py", "tools/shader_apk.py", "native/vulkan_probe.c",
                                               "native/vulkan_requirements.h"))
    sources.extend(LAUNCHER / "native" / name for name in launcher_native_inputs())
    return {path.relative_to(LAUNCHER).as_posix(): sha(path) for path in sorted(sources)}


def stage_native_profile_default(destination):
    source = ROOT / "build/localization/Builds/Release/Default.ini"
    data = source.read_bytes()
    digest = hashlib.sha256(data).hexdigest()
    if digest != NATIVE_PROFILE_DEFAULT_SHA256:
        raise ValueError("Pinned localized Default.ini does not match the native profile template")
    destination.write_bytes(data)
    return {"source": source.relative_to(ROOT).as_posix(), "sha256": digest, "bytes": len(data)}


SHADERLIB = ROOT / "build/render10/bs_shaderlib10.bin"
SHADERLIB_ASSET = "assets/shaders/bs_shaderlib10.bin"
SHADER_WARMUP = ROOT / "android/render10/bs_warmup.bin"


def shader_library():
    """The build-time shader library of the native Direct3D 10 renderer (tools/build_shaderlib10.py)."""
    manifest = SHADERLIB.with_suffix(".json")
    receipt = json.loads(manifest.read_text()) if manifest.is_file() else {}
    generator = ROOT / "tools/build_shaderlib10.py"
    variants = ROOT / "tools/shaderlib10/variants.py"
    if (not SHADERLIB.is_file() or receipt.get("vulkan") != "1.0" or receipt.get("version") != 3
            or receipt.get("generator_sha256") != sha(generator) or receipt.get("variants_sha256") != sha(variants)):
        run(sys.executable, "-X", "utf8", "-B", generator, "build")
        receipt = json.loads(manifest.read_text())
    if receipt["sha256"] != hashlib.sha256(SHADERLIB.read_bytes()).hexdigest():
        raise ValueError("The shader library differs from its manifest: run tools/build_shaderlib10.py build")
    return {k: receipt[k] for k in ("library", "sha256", "bytes", "key", "pcs10_sha256", "unique_dxbc")}


def stage_ios_controls(destination, ipa):
    cache = ROOT / "build/ios-touch"
    manifest = cache / "receipt.json"
    if ipa is not None:
        cache.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(prefix="extract-", dir=cache) as tmp:
            prepared = Path(tmp)
            receipt = extract_ios_controls.extract(ipa, prepared)
            for name, digest in receipt["assets"].items():
                if Path(name).name != name or Path(name).suffix != ".png" or sha(prepared / name) != digest:
                    raise ValueError(f"Invalid extracted iOS control asset {name}")
                (prepared / name).replace(cache / name)
            (prepared / manifest.name).write_text(json.dumps(receipt, indent=2) + "\n", encoding="utf-8")
            (prepared / manifest.name).replace(manifest)
    if not manifest.is_file():
        raise ValueError("iOS controls are missing. Build once with --ios-ipa pointing to the original BioShock 1.3.5 IPA.")
    receipt = json.loads(manifest.read_text(encoding="utf-8"))
    expected_files = {name + ".png" for name in extract_ios_controls.HUD.keys() | extract_ios_controls.ICONS.keys()}
    expected_sources = {path: digest for path, (_, digest) in extract_ios_controls.SOURCES.items()}
    if set(receipt["assets"]) != expected_files or receipt["source_members"] != expected_sources:
        raise ValueError("The extracted iOS asset manifest differs from the expected original. Rebuild with --ios-ipa.")
    destination.mkdir()
    for name, digest in receipt["assets"].items():
        if Path(name).name != name or Path(name).suffix != ".png":
            raise ValueError(f"Invalid iOS control asset name {name}")
        shipped = destination / name
        shutil.copyfile(cache / name, shipped)
        if sha(shipped) != digest:
            raise ValueError(f"iOS control asset {name} differs from its receipt. Rebuild with --ios-ipa.")
    return receipt


def stage_native_minigames(assets):
    return build_native_minigames(assets / "native-combolock.swf")


TEXCONV = ROOT / "build/texconv/libtexconv.so"


def texture_converter():
    """The launcher's texture card converter (tools/build_texconv.py), rebuilt when a source changed."""
    receipt_path = TEXCONV.parent / "receipt.json"
    def current():
        if not TEXCONV.is_file() or not receipt_path.is_file():
            return None
        receipt = json.loads(receipt_path.read_text())
        if receipt.get("sha256") != sha(TEXCONV) or receipt.get("tool_sha256") != sha(ROOT / "tools/build_texconv.py"):
            return None
        if any(not (ROOT / name).is_file() or sha(ROOT / name) != digest for name, digest in receipt["sources"].items()):
            return None
        return receipt
    receipt = current()
    if receipt is None:
        run(sys.executable, ROOT / "tools/build_texconv.py")
        receipt = current()
    if receipt is None:
        raise ValueError("The texture converter differs from its receipt: run tools/build_texconv.py")
    return {"library": "lib/arm64-v8a/libtexconv.so", "sha256": receipt["sha256"]}


def stage_shipping_library(library, destination, strip):
    """Keep profiling symbols and strip a separate shipping copy, never the build."""
    source = library.read_bytes()
    symbols = destination / "libbioshock.unstripped.so"
    shipped = destination / "libbioshock.stripped.so"
    if library.resolve() == shipped.resolve():
        raise ValueError("Shipping output must differ from the source library")
    if library.resolve() != symbols.resolve():
        shutil.copy2(library, symbols)
    run(strip, "--strip-unneeded", "-o", shipped, symbols)
    identity = assert_runtime_identity(source, shipped.read_bytes())
    if not identity.get("build_id"):
        # Without it simpleperf/llvm-symbolizer match a profile to a stale cached copy (wiki note 13, PF5c).
        raise ValueError("The game library has no GNU build ID: link it with -Wl,--build-id=sha1")
    if not identity["has_symbols"] or symbols.read_bytes() != source or library.read_bytes() != source:
        raise ValueError("Exact unstripped profiling symbols were not preserved")
    return shipped, {"library": str(shipped), "library_sha256": sha(shipped),
                     "source_library": str(library), "source_library_sha256": hashlib.sha256(source).hexdigest(),
                     "symbols_library": str(symbols), "symbols_sha256": sha(symbols),
                     "allocated_identity": identity["allocated_sha256"], "elf_identity": identity,
                     "source_library_bytes": len(source), "library_bytes": shipped.stat().st_size}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--library", type=Path, help="Use an already built libbioshock.so")
    parser.add_argument("--output", type=Path, default=ROOT / "build/apk/bioshock.apk")
    parser.add_argument("--diagnostics", action="store_true", help="Package guest diagnostic instrumentation")
    parser.add_argument("--ios-ipa", type=Path, help="Extract the original controls from a local BioShock iOS 1.3.5 IPA")
    args = parser.parse_args()
    if LAUNCHER is None:
        parser.error("The launcher base is missing: set PORT_LAUNCHER or use the sibling launcher folder")
    launcher_inputs = launcher_sources()
    sdk = Path(os.environ.get("ANDROID_SDK_ROOT", Path.home() / "AppData/Local/Android/Sdk"))
    tools = sdk / "build-tools/36.0.0"
    android = sdk / "platforms/android-37.0/android.jar"
    java = Path(os.environ["JAVA_HOME"]) / "bin"
    library = args.library or ROOT / "build/android/libbioshock.so"
    if not args.library:
        run(sys.executable, ROOT / "tools/build_android.py", "--shared", *(["--diagnostics"] if args.diagnostics else []))
    library = library.resolve()
    data = library.read_bytes()
    if len(data) < 64 or data[:5] != b"\x7fELF\x02" or struct.unpack_from("<HH", data, 16) != (3, 183):
        parser.error("The APK requires an AArch64 ELF shared library")
    runtime, pinned, licenses = native_dependencies()
    native_pins = {path.relative_to(ROOT).as_posix(): digest for path, digest in pinned.items()}
    built = json.loads(library.with_suffix(".json").read_text())
    if built.get("sha256") != hashlib.sha256(data).hexdigest() or built.get("native_dependencies") != native_pins:
        parser.error("Game library receipt does not match the current native audio/Vulkan driver bundle; rebuild it first")
    if built.get("launcher_native_headers") != launcher_native_inputs():
        parser.error("Game library receipt does not match the launcher's native headers; rebuild it first")
    out = args.output.resolve()
    out.parent.mkdir(parents=True, exist_ok=True)
    machine = json.loads((Path.home() / ".agents/skills/adapt-arm32-game/machine_config.json").read_text())
    signing = machine["signer"]
    signer_dir = Path(signing["dir"])
    signer = signer_dir / signing["apksigner"]
    certificate = signer_dir / signing["cert"]
    key = signer_dir / signing["key"]
    for required in (signer, certificate, key):
        if not required.is_file():
            parser.error(f"Configured portable signing input is missing: {required}")
    certificate_sha = hashlib.sha256(ssl.PEM_cert_to_DER_cert(certificate.read_text())).hexdigest()
    if certificate_sha != signing["expected_cert_sha256"]:
        parser.error("Configured signing certificate SHA256 mismatch")
    strip = sdk / "ndk/27.2.12479018/toolchains/llvm/prebuilt/windows-x86_64/bin/llvm-strip.exe"
    shipped_library, library_receipt = stage_shipping_library(library, out.parent, strip)
    with tempfile.TemporaryDirectory(prefix="bioshock-apk-", dir=out.parent) as temporary:
        stage = Path(temporary)
        classes, dex, assets = stage / "classes", stage / "dex", stage / "assets"
        for directory in (classes, dex, assets):
            directory.mkdir()
        ios_controls = stage_ios_controls(assets / "ios", args.ios_ipa)
        shaders = shader_library()
        textures = texture_converter()
        native_settings = build_native_settings(assets / "native-settings-pause.swf")
        native_minigames = stage_native_minigames(assets)
        native_plasminow = build_plasminow(assets / "native-plasminow.swf")
        loading_indicator = build_loading_indicator(assets / "native-loading.swf")
        native_hud = build_native_hud(ROOT / "build/native-hud")
        for name in native_hud:
            shutil.copy2(ROOT / "build/native-hud" / name, assets / name)
        build_map(assets / "native-map.swf")
        native_profile = stage_native_profile_default(assets / "native-profile-default.ini")
        for name, source in (("THIRD_PARTY_NOTICES.md", ROOT / "THIRD_PARTY_NOTICES.md"),
                             ("GPL-3.0.txt", ROOT / "reference/ut3/reference/homm5/LICENSE"),
                             ("pcrecomp-license.txt", ROOT / "reference/pcrecomp/LICENSE"),
                             ("x87-attribution.md", ROOT / "reference/ut3/reference/homm5/x87-attribution/README.md")):
            if source.is_file():
                shutil.copy2(source, assets / name)
        native_licenses = assets / "native-licenses"
        native_licenses.mkdir()
        for source in licenses:
            shutil.copy2(source, native_licenses / source.name)
        # libtexconv.so (the launcher's texture card) links the Arm ASTC Encoder and etcpak.
        shutil.copy2(ROOT / "reference/astcenc/LICENSE.txt", native_licenses / "astcenc-LICENSE.txt")
        shutil.copy2(ROOT / "reference/etcpak/LICENSE.txt", native_licenses / "etcpak-LICENSE.txt")
        sys.path.insert(0, str(LAUNCHER / "tools"))
        import launcher as port_launcher
        prepared = port_launcher.prepare(ROOT / "android/port.json", ROOT / "android/app/AndroidManifest.xml", stage / "launcher")
        shutil.copytree(Path(prepared["assets"]) / "launcher", assets / "launcher")
        min_sdk = int(port_launcher.manifest_attribute(Path(prepared["manifest"]).read_text(encoding="utf-8"),
                                                       "android:minSdkVersion"))
        manifest_sha = sha(Path(prepared["manifest"]))
        probe = port_launcher.build_probe(stage / "probe", prepared["package"], api=min_sdk) if prepared["probe"] else None
        resources = stage / "resources.zip"
        run(tools / "aapt2.exe", "compile", "--dir", ROOT / "android/app/res", "-o", resources)
        run(tools / "aapt2.exe", "link", "-I", android, "--manifest", prepared["manifest"],
            "-A", assets, resources, "-o", stage / "base.apk")
        sources = sorted((ROOT / "android/app/src").rglob("*.java")) + [Path(p) for p in prepared["sources"]]
        run(java / "javac.exe", "--release", "8", "-encoding", "UTF-8", "-cp", android, "-d", classes, *sources)
        run(java / "java.exe", "-cp", tools / "lib/d8.jar", "com.android.tools.r8.D8",
            "--lib", android, "--min-api", min_sdk, "--output", dex, *sorted(classes.rglob("*.class")))
        unsigned = stage / "unsigned.apk"
        shutil.copy2(stage / "base.apk", unsigned)
        with zipfile.ZipFile(unsigned, "a", zipfile.ZIP_DEFLATED) as apk:
            apk.write(dex / "classes.dex", "classes.dex")
            apk.write(shipped_library, "lib/arm64-v8a/libbioshock.so")
            for name, source in sorted(runtime.items()):
                apk.write(source, "lib/arm64-v8a/" + name)
            apk.write(TEXCONV, textures["library"])
            # Stored: the renderer maps the library from the APK (AAsset_getBuffer) without a copy.
            apk.write(SHADERLIB, SHADERLIB_ASSET, compress_type=zipfile.ZIP_STORED)
            apk.write(SHADER_WARMUP, "assets/shaders/bs_warmup.bin")
            if probe is not None:
                apk.write(probe, "lib/arm64-v8a/" + probe.name)
            probe_sha = hashlib.sha256(probe.read_bytes()).hexdigest() if probe is not None else None
        run(tools / "zipalign.exe", "-f", "-P", "16", "4", unsigned, stage / "aligned.apk")
        run(sys.executable, LAUNCHER / "tools/shader_apk.py", stage / "aligned.apk", "--asset", SHADERLIB_ASSET, SHADERLIB)
        run(java / "java.exe", "-jar", signer, "sign", "--key", key, "--cert", certificate,
            "--out", out, stage / "aligned.apk")
        run(java / "java.exe", "-jar", signer, "verify", "--verbose", out)
        run(tools / "zipalign.exe", "-c", "-P", "16", "4", out)
    with zipfile.ZipFile(out) as apk:
        panvk = runtime["libvulkan_panfrost.so"]
        if hashlib.sha256(apk.read(f"lib/arm64-v8a/{panvk.name}")).hexdigest() != pinned[panvk]:
            raise ValueError("Packaged PanVK driver differs from the pinned native bundle")
        for name, digest in ios_controls["assets"].items():
            if hashlib.sha256(apk.read(f"assets/ios/{name}")).hexdigest() != digest:
                raise ValueError(f"Packaged iOS control asset {name} differs from the extracted original")
    if any(sha(path) != digest for path, digest in pinned.items()):
        raise ValueError("Native dependencies changed during APK packaging")
    if launcher_inputs != launcher_sources():
        raise ValueError("Launcher sources changed during APK packaging")
    if sha(library) != library_receipt["source_library_sha256"] or sha(shipped_library) != library_receipt["library_sha256"]:
        raise ValueError("Source or shipping library changed during APK packaging")
    receipt = {"apk": str(out), "sha256": hashlib.sha256(out.read_bytes()).hexdigest(),
               **library_receipt,
               "package": "org.bioshock.port", "native_dependencies": native_pins,
               "renderers": ["gles", "vulkan", "vulkan-turnip", "vulkan-panvk"],
               "min_sdk": min_sdk,
               "renderer": "OpenGL ES 3.2 and Vulkan 1.1 or 1.0 with VK_KHR_maintenance1",
               "shader_library": shaders, "shader_warmup_sha256": sha(SHADER_WARMUP),
               "texture_converter": textures, "audio": "FMOD Ex and Bink over AAudio; OpenAL Soft and DirectSound compatibility",
               "native_settings": native_settings,
               "native_minigames": native_minigames,
               "native_plasminow": native_plasminow,
               "loading_indicator": loading_indicator,
               "native_hud": native_hud,
               "ios_controls": ios_controls,
               "native_profile": native_profile,
               "launcher": {"package": prepared["package"], "commit": port_launcher.git_commit(LAUNCHER),
                            "source_hashes": launcher_inputs, "manifest_sha256": manifest_sha, "probe_sha256": probe_sha},
               "guest_diagnostics": "-DRECOMP_TRACE" in built["cflags"],
               "gameplay": "NOT YET VERIFIED"}
    out.with_suffix(".json").write_text(json.dumps(receipt, indent=2) + "\n")
    print(json.dumps(receipt, indent=2))


if __name__ == "__main__":
    main()

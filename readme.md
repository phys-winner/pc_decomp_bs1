# pc_decomp_bs1

**Version `0.3-dev`** · Experimental Android ARM64 port of BioShock™ Classic.
The port recompiles the original Windows x86 code ahead of time and runs it with
an Android Win32 compatibility layer. Its native Direct3D 10 backend supports
Vulkan and OpenGL ES, with touch controls and gamepad input.

## Compatible game edition

This port is compatible with **BioShock™ Classic for Windows, patch 1.1, GOG
build 127339**. GOG includes Classic with a purchase of
[BioShock™ Remastered](https://www.gog.com/en/game/bioshock_remastered).
Install the Classic edition from your GOG library. The Remastered game files
are outside the supported input set. The build inputs are identified by SHA-256
in [`config/inputs.example.json`](config/inputs.example.json) and
[`config/modules.example.json`](config/modules.example.json).

## Install and play

1. The APK requires ARM64, Android 8.0 (API 26) or newer, and OpenGL ES 3.2.
   OpenGL ES is the default. The Vulkan backends require Vulkan 1.1 or
   Vulkan 1.0 with `VK_KHR_maintenance1`. The bundled Turnip driver requires
   Android 11 or newer, and PanVK requires Android 11 or newer.
2. Copy your installed Classic game folder to the device. Keep both `Builds/`
   and `Content/` at its root and retain `Builds/Release/Bioshock.exe`.
   The default location is `/sdcard/BioShock/game`.
3. Open the launcher and select the game folder under **Game data**. Grant
   **All files access** on Android 11 or newer or storage access on older versions
   when prompted. You can also select a
   ZIP of the installed game. The launcher can play a ZIP with stored entries
   directly or extract a compressed ZIP.
4. Select a renderer and press **Play**. Vulkan and OpenGL ES are available,
   alongside driver options for compatible Adreno and Mali devices. The launcher
   offers optional texture conversion on GPUs that need it. See
   [Texture cache](docs/texture-cache.md) for details.

Archives with `.rus_fsd` banks require an APK with FSD1 support. Keep each
delta beside its matching English `.fsb` in `Content/Sounds_Windows`. Select
Russian under **Voice language** to play using the compact banks.

The APK requires your own copy of the original game data. The `0.3-dev` build
is intended for sideloading. Device, driver and full-campaign coverage is limited.

## Build in a prepared Windows workspace

The build uses Python 3.12+, a JDK (`JAVA_HOME`), Android SDK Build Tools
`36.0.0`, the API 37 platform, NDK `27.2.12479018` and SDK CMake `3.22.1`.
Set `ANDROID_NDK_HOME` to that NDK if the environment already defines it.
Provide the shared port launcher through `PORT_LAUNCHER`, `android/launcher`,
or a sibling `launcher` directory.

On a fresh checkout, copy `config/inputs.example.json` to `config/inputs.json`
and set `source_root` to your installed Classic game folder. Copy
`config/modules.example.json` to `config/modules.json` and set each `source`
field to the matching local Windows DLL. Keep the pinned SHA-256 values.
If regenerating lifted C, keep the IDA function catalogs for `bioshock` and
`d3dx9_33` under `config/` as `*.functions.json`. The APK build uses the prepared
`generated/` sources.

Initialize the pinned third-party source and runtime submodules before building.

```powershell
git -c core.autocrlf=false submodule update --init
git -C reference/adrenotools -c core.autocrlf=false submodule update --init -- lib/linkernsbypass
git -C reference/dxvk -c core.autocrlf=false submodule update --init -- include/vulkan include/spirv
```

The build also requires the locally prepared lifted C in `generated/`, the
locally staged PanVK runtime, the shader library or its original game inputs,
the pinned localized profile template under `build/localization/`, and the
builder's local Android signing configuration. See
[`tools/build_apk.py`](tools/build_apk.py) and
[`THIRD_PARTY_NOTICES.md`](THIRD_PARTY_NOTICES.md) for dependency details. A clean
checkout by itself does not reproduce the prepared workspace or the release APK.

```powershell
python tools/build_android.py --shared --x87-locals --jobs 8
python tools/build_apk.py --library build/android/libbioshock.so --output build/apk/bioshock.apk
```

On the first APK build, pass `--ios-ipa "C:/path/to/BioShock 1.3.5.ipa"` to
extract the original iPhone and iPad control images. Later builds verify and
reuse the extracted images in `build/ios-touch/`.

Local release copies use `pub/bioshock-v-0.3-dev-<sha256-prefix>.apk`, with the
first eight characters of the APK's SHA-256 and a companion `.apk.sha256` file.
Release files and local build inputs stay outside Git.

## Release update manifests

The launcher reads `releases/latest/download/update.json` from this repository.
The **Update manifest** workflow generates `update.json` and `versions.json`
from published release APKs and attaches both files to the latest release.
It runs on release changes, manually, and every six hours to catch APKs attached
after publication or restore missing manifests. Drafts and pre-releases are
excluded. APK metadata, signing certificates and SHA-256 hashes come from the
actual release assets; `versions.json` caches those facts for later runs.

The root JSON files are a versioned snapshot of the current release metadata.
To regenerate them locally with Android build tools installed:

```powershell
python tools/update_manifest.py --repo phys-winner/pc_decomp_bs1 --out update.json --state versions.json
```

The workflow uses its own fresh cache from release assets. Rebuilding the APK
is not required when updating these manifests.

## Credits and licenses

This is an unofficial port. BioShock and its original game content belong to
their respective rights holders. Original port code and the adapted Win32 host
are licensed under [GPL-3.0-or-later](LICENSE). See
[third-party notices](THIRD_PARTY_NOTICES.md) for component licenses and provenance.

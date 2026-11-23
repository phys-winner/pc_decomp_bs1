# Third-party notices

The Win32/D3D9 compatibility host in `android/host` and the private ARM64 runtime
were copied and adapted from the local Heroes V port, itself based on the local
The Void port. That project's authored port code is GPL-3.0-or-later; its license
and original notices are retained in `reference/ut3/reference/homm5/`. Upstream-derived portions
retain the terms described below. Exact source and copied-file hashes are recorded
in `dependencies-host.json`; the lifter has its separate dependency records.

| Component | Revision/version | License | Retained notice |
| --- | --- | --- | --- |
| pcrecomp runtime and lifting components | `904ac3129d0c9d05efbeec19a1dce6abbe7f5d26` | MIT | `reference/pcrecomp/LICENSE` |
| dlmalloc | 2.8.6, relicensed 2023 | MIT-0 | Notice at the beginning of `reference/ut3/reference/dlmalloc/malloc.c` |
| Adapted x87 sliding-window algorithm by sistemas3cecasem | NFSU2 revision `62a1d3a17fc7619b9d32df182b4b47d1a4f2e959` | MIT | Original patch and attribution in `reference/ut3/reference/homm5/x87-attribution/` |

The compatibility host's former Direct3D 9 path included a GLES layer, MojoShader,
and a DXVK adapter. BioShock ships the OpenGL ES and Vulkan backends in
`android/render10`. The former sources, with MojoShader (Zlib, revision
`cf2661bcb55d9a8732e86f79922592d7efdf5383`, an altered snapshot with the H5/The Void
port's marked local extensions), are archived in `work/removed-d3d9-dxvk-20261119/`.
The optional offline texture cache converter uses the codec sources listed below.
Android system libraries,
the NDK, Java, Python and installed Python packages are separate development/runtime
dependencies with their own notices.

BioShock executable files, DLLs and game data remain the property of their
respective rights holders. The local APK builder uses the user's own game files
to generate the Android runtime and associated resources. The APK requires a
separate installation of the original game data.

## Native audio runtime

The ARM64 native audio dependency is OpenAL Soft 1.25.2, using Oboe
1.11.0 for physical AAudio output. Its two dynamic libraries are retained in
`reference/ut3/reference/audio-runtime/arm64-v8a/` and staged into `build/native-audio/runtime/`
by `tools/build_native_audio.py`. Exact binary, source, archive, recipe and
license hashes are recorded in `dependencies-audio.json` and the retained
upstream build receipt.

| Component | Revision/version | License | Retained notice |
| --- | --- | --- | --- |
| OpenAL Soft | 1.25.2, `b2c48f7718ef3fcf67921a8b6534c4914e328970` | LGPL-2.0-or-later | `reference/ut3/reference/audio-runtime/licenses/OpenAL-COPYING.txt` |
| Oboe | 1.11.0, `b115f47593969fd67a21e9f63640ffef749b5067` | Apache-2.0 | `reference/ut3/reference/audio-runtime/licenses/Oboe-LICENSE.txt` |
| LLVM libc++ shared runtime | Android NDK 27.2.12479018 | Apache-2.0 with LLVM exceptions and included notices | `reference/ut3/reference/audio-runtime/licenses/LLVM-NDK-NOTICE.txt` |
| fmt bundled with OpenAL Soft | 11.2.0 | MIT | `reference/ut3/reference/audio-runtime/licenses/fmt-LICENSE.txt` |
| Microsoft GSL bundled with OpenAL Soft | Snapshot in the pinned OpenAL source tree | MIT | `reference/ut3/reference/audio-runtime/licenses/GSL-LICENSE.txt` |
| PFFFT bundled with OpenAL Soft | Snapshot in the pinned OpenAL source tree | BSD-style terms, including FFTPACK attribution | `reference/ut3/reference/audio-runtime/licenses/OpenAL-PFFFT-LICENSE.txt` |

Pristine OpenAL Soft and Oboe source trees, original archives and provenance are
retained in `reference/openal-soft/source/` and `reference/oboe/source/`. The exact local build
recipe and its authored metrics/fixture sources are retained in
`reference/ut3/reference/audio-runtime/rebuild/`. This is an altered native build: its private
overlay patches and compiler/configuration are recorded in
`reference/ut3/reference/audio-runtime/upstream-build-receipt.json`. It provides AAudio playback,
with capture, EAX, OpenSL, null and wave backends disabled. The original
`FrostpunkAudio_*` instrumentation export names remain in this reused binary.

When packaging these audio DSOs, retain the notices in
`build/native-audio/licenses/`, including the source provenance notice. The
source snapshot and rebuild recipe above correspond to the shipped native
audio dependency.

## Shader library translators (build time only)

`tools/build_shaderlib10.py` translates the game's shaders on the PC with
`tools/shaderlib10/bs_shaderc.cpp`, which compiles DXVK 2.4.1's DXBC compiler
(revision `0cf05780abd7250c2cd713b7749cf32180157cf5`, retained in `reference/dxvk/`
with source hashes in `dependencies-dxvk.json`, zlib/libpng license
`reference/dxvk/LICENSE`) and SPIRV-Cross (`reference/spirv-cross/`, Apache-2.0,
`reference/spirv-cross/LICENSE`). The APK holds only their output (SPIR-V and GLSL ES
text in `assets/shaders/bs_shaderlib10.bin`), no DXVK or SPIRV-Cross code. The former
run-time DXVK renderer (`android/dxvk/`, `android/host/d3d9_dxvk.c`,
`tools/build_dxvk.py`) is archived in `work/removed-d3d9-dxvk-20261119/`.

## Optional native texture cache

The offline converter in `tools/bioshock_textures.py` writes the UT3 port's
content-keyed UTX3 v2 format. Its unmodified BCn decoder and ASTC/ETC2 codec
are retained in `android/texconv/`, adapted from the local UT3/The Void GPL host.
The BioShock package/catalog reader is an independent implementation; the
BioShock serialization documented by UEViewer (MIT, Konstantin Nosov) was used
as a reference. No UEViewer source is compiled or imported by the tool.

| Component | Revision | License | Retained notice |
| --- | --- | --- | --- |
| Arm ASTC Encoder | `baff485b0ff36d2f95d28961605106502c653966` | Apache-2.0 | `reference/astcenc/LICENSE.txt` and source headers |
| etcpak | `3d716e1550023dc5ff8b602636af8b76584e0b66` | BSD-3-Clause | `reference/etcpak/LICENSE.txt` |
| bcdec, included by etcpak | v0.96 | MIT or Unlicense | Full terms at the end of `reference/etcpak/bcdec.h` |

`dependencies-textures.json` pins every retained codec source and notice.
The standalone host DLL and its build receipt are created by
`tools/build_texture_codec.py`; they are not Android application libraries.
The game runtime only reads the optional pack. Proprietary textures stay in
user-prepared data and are not packaged into the APK.

## AMD FidelityFX Super Resolution 1

FSR1 EASU/RCAS of the native renderer are GLSL generated at build time from AMD's float kernels (`tools/build_android.py`: SPIR-V for Vulkan, GLSL ES for OpenGL ES), run on the scaled scene before the original HUD. Source: <https://github.com/GPUOpen-Effects/FidelityFX-FSR> at `a21ffb8f6c13233ba336352bdff293894c706575`. The retained source is `reference/fsr1/ffx-fsr/ffx_fsr1.h`; the MIT licence is `reference/fsr1/license.txt` and is included in the APK.

## Community fixes and improvements for BioShock (ideas and data, no code copied)

The "Game and fixes" page of the port's Additional settings and its anisotropy row follow three
community projects for the PC game. No source, shader or binary of them is compiled into, copied
into or shipped with the port: the port reimplements what they do inside its own native host and
renderer (`android/host/bioshock_tweaks.c`, `android/render10/r10_present.c`). Their authors are
credited here for the findings the port relies on.

| Project | Author | Revision | License | What the port uses |
| --- | --- | --- | --- | --- |
| [Bioshock Physics Unlocker](https://github.com/TurtleHx/Bioshock-Physics-Unlocker) | TurtleHx | `974a4d2f9d34e2fea079ccaf13110fe569f3e1b7` | MIT (Copyright (c) 2015 TurtleHx) | The location of the Havok fixed physics step (the float 36 bytes after `HkUnrealCharacterPointCollector@@` in `Bioshock.exe`, 1/30 s) and the idea of raising it; setting "Physics: 60 Hz" |
| [BioshockGraphicalUpgrade](https://github.com/garamond13/ReShade-shaders/tree/main/Addons/BioshockGraphicalUpgrade) (ReShade add-on) | garamond13 | `0a0209cf1c47a38adbbb6055812e98117f63ce28` | No license file in the repository | The idea of forcing 16x anisotropic filtering on the game's samplers; setting "Anisotropy" |
| [BioshockMouseSensitivity](https://github.com/garamond13/BioshockMouseSensitivity) | garamond13 | `da14145327a17c66949e0c73cc1dad44f329f42d` | No license file in the repository | The game settings it changes (`MouseSmoothingMode 0`, `MouseAccelThreshold 0.0`); setting "Raw mouse" applies the first to the live input object, the second is already the default of this edition |

The two garamond13 repositories carry no license, so none of their code or shaders (including the
tonemapping, bloom, GTAO and SMAA integration of BioshockGraphicalUpgrade) is reused. The MIT
notice of the Physics Unlocker, which applies to its source, reads:

> The MIT License (MIT)
>
> Copyright (c) 2015 TurtleHx
>
> Permission is hereby granted, free of charge, to any person obtaining a copy of this software and
> associated documentation files (the "Software"), to deal in the Software without restriction,
> including without limitation the rights to use, copy, modify, merge, publish, distribute,
> sublicense, and/or sell copies of the Software, and to permit persons to whom the Software is
> furnished to do so, subject to the following conditions:
>
> The above copyright notice and this permission notice shall be included in all copies or
> substantial portions of the Software.
>
> THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT
> NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
> NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES
> OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
> CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.

The bug descriptions the original-game fixes answer (Langford's safe with the Scrounger tonic,
cutscenes above 60 FPS, subtitles vanishing with the controller HUD) are listed by PCGamingWiki's
BioShock article. The fixes themselves were derived from the game's own UnrealScript sources,
`Bioshock.exe` and `HUDRadial.swf` of the user's installation; the third-party "Gamepad Subtitle
Fix" replacement of `HUDRadial.swf` is neither used nor shipped.

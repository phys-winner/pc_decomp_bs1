# Converted texture pack (ASTC / ETC2)

Most mobile GPUs cannot sample the game's BC1-3 (DXT1/3/5) textures with their system drivers: Adreno's
and Mali's drivers have no BC at all, PowerVR's OpenGL ES driver has BC1 only (its Vulkan driver samples
BC1-3, see below). Without a converted copy the native Direct3D 10 renderer decodes every such level to
RGBA8 on the game thread while the level loads or streams in: a quarter (BC3) or an eighth (BC1) of the GPU
memory becomes the full RGBA8 size, and streaming while the camera turns costs long frames.

`<game>/Content/port-textures.tvt` holds every DXT level of the game's packages and bulk files with its
ASTC 4x4 or ETC2 copy (the UTX3 v2 container of the UT3 port, `android/texconv/ut3tx.h`, keyed by each
level's own blocks). It is optional: the originals are never changed, and anything not in the pack keeps the
ordinary path.

Where the pack lives (`android/host/texture_cache.c` reads it, the card writes it):
- a game folder: `<game>/Content/port-textures.tvt`;
- a stored ZIP played in place ("Play from archive"): the archive's sidecar folder, which the launcher
  passes as the game folder (`<app files>/launcher/mount-bioshock-game/Content/port-textures.tvt`, the folder
  that also holds the archive's few compressed files; `files.c` maps `C:\Games\BioShock` there). The
  converter reads the archive's stored entries in place through the launcher's index (`mount-*.zmi`) and the
  archive's descriptor, with the game's own reader `android/host/zipmount.c`;
- a separate cache root of the launcher (port.json `"textures": {"separateCache": true}`, not set for
  BioShock): `<root>/Content/port-textures.tvt`, passed to the game as `PORT_TEXTURE_CACHE_ROOT`.

## Making the pack

**On the device (the launcher's texture card).** When the selected renderer cannot sample DXT, the launcher
shows the card: "Convert to ASTC" (if the GPU has ASTC LDR) or "Convert to ETC2", and later "Delete the
converted textures". The work runs in the launcher's foreground service with a progress bar
(`org.bioshock.port.TextureConverter` → `libtexconv.so`, `android/texconv/bioshock_tex.cpp`, built by
`tools/build_texconv.py`). On the Pixel 10 (Tensor G5, 8 threads) the full ASTC pack took about 3.5 minutes
at astcenc's medium preset; it needs about 5.5 GB of free space (5.28 GB pack). Measured on 21 November 2026:
20,905 textures, 6,870 unique mip chains, 39,466 level keys, 5,277,799,712 bytes. The realme RMX3371
(Snapdragon 870) made the same pack from a stored ZIP played in place in about 4.7 minutes: 5,277,799,712
bytes again, in the sidecar folder.

For device checks the same converter is a command (`build/texconv/bioshock-tex`, pushed to
`/data/local/tmp`; for an archive run it as the app with `run-as` so that it can read the launcher's index):

```sh
bioshock-tex /sdcard/BioShock/game scan
bioshock-tex /sdcard/BioShock/game convert 2 8          # 1 ETC2, 2 ASTC; threads
bioshock-tex <sidecar> --archive <zip> --index <zmi> convert 2 8
bioshock-tex <game> [--cache <root>] undo
```

**On a PC.** `tools/bioshock_textures.py` makes the same container from a Windows copy of the game
(the host codec is built with `python tools/build_texture_codec.py`):

```powershell
python tools/bioshock_textures.py --game-root C:/Games/BioShock --output build/texture-cache/full/Content/port-textures.tvt --threads 4 --quality 60
```

Copy only the completed pack to `<gameRoot>/Content/port-textures.tvt` on the device. The JSON receipt
next to the host output records the pack/codec SHA-256, quality, counts and every original input hash; a
failed or interrupted cook keeps the previous complete pack, and a preexisting `.part` is never silently
overwritten.

Both readers are the same port of the original formats: BioShock PC v141 packages (`Content/Maps/*.bsm`,
`Content/System/*.u` and the engine's script packages `Builds/Release/*.U`, zlib blocks), their `Texture`
exports and `Content/BulkContent/Catalog.bdc` with its BLK files. The script packages hold what the game
draws every frame (the weapons in hand, plasmids, ammo, fonts, the cursor, the HUD masks): without them 171
levels missed the pack on the Pixel 10, with them 4 (textures the game makes at run time). Missing top levels are reconstructed from bulk records whose size ends on a level boundary and
agrees with the embedded lower levels. A level whose blocks head chains with different lower levels gets a
one-level entry (since 21 November 2026; the PC cooker used to drop these 793 keys).

## Using the pack (`android/render10/r10_pack.c`)

- A BC1-3 texture the backend cannot sample is created in the pack's format (ASTC 4x4, or ETC2 RGBA8 EAC
  for an ETC2 pack, sRGB where the game's format is sRGB). Every whole level the game writes
  (`UpdateSubresource`, initial data) is looked up by its blocks; a hit uploads the converted blocks.
- Copies between converted textures (the engine's streaming copies its lower levels into a larger texture,
  `FD3DTexture2D10`, Bioshock.exe 0x1099CAA0) copy blocks and keep each level's original.
- A level the pack lacks, a partial write, or a copy the pack cannot express (a box, another BC format)
  first moves the texture to the decoded RGBA8 storage, rewriting the levels it held from the pack's copy of
  their original blocks. The picture never depends on the pack; only the speed and memory do.
- A texture that left the pack stays decoded but keeps the original blocks of its levels. A whole-level copy
  from a converted texture into a decoded one decodes the level's original blocks into the destination; one
  from a decoded texture into a converted one uploads the pack's converted copy of the source level's
  blocks. Neither side is restored: restoring both sides spread the decoded storage through the engine's
  texture pool (RMX3371 sweep of 21 November 2026: 48 restores after four levels, 1006 after fifteen; with
  level copies the restores equal the misses, 47 after nine levels).
- The log names the first misses (`pack: level not in the pack: key ...`) and counts hits, misses, copies
  (block copies and level copies) and restores at powers of two.
- QA: `BIOSHOCK_TEXTURE_PACK=0` in a QA profile's `perf-flags.env` ignores the pack (A/B runs).
- Memory: the pack is mapped read-only without readahead (`MADV_RANDOM`), and the bytes a lookup compares and an
  upload copies leave the process's mapping right after (`MADV_DONTNEED`, `void_texture_cache_done`); they stay in
  the page cache. With every loaded level left mapped, the Arcadia save kept 327 MiB of the pack resident on the
  Lenovo TB373FU (Mali-G615), more than the 256 MiB of GPU memory the pack saved there
  (work/ha22-perf-20261122).

## BC without the textureCompressionBC feature

The Vulkan backend decides BC per format from the format queries (sampled and linearly filtered), not from
`textureCompressionBC`, which promises every one of BC1-7. PowerVR's driver (DXT-48-1536) samples BC1-3
without the feature, so the Vulkan renderer needs no pack on the Pixel 10; the launcher's probe already
read it that way (`stock.bc=111`) and hides the card for Vulkan there. `BIOSHOCK_R10_DEBUG` bit 131072
decodes BC on the CPU as before for comparisons.

`dependencies-textures.json` pins the copied UT3 codec and the required ASTC/ETC2 source subset; the
third-party notices and licenses stay in the project.

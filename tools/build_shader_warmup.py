"""Pack pipeline recipes from a Vulkan capture and shader pairs from GLES draw traces."""

import argparse
from collections import Counter
from pathlib import Path
import re
import struct


ROOT = Path(__file__).resolve().parents[1]
RECIPE_SIZE = 1088  # android/render10/r10_vulkan.c, warm_pipeline
LIMIT = 512
# android/render10/r10_shaderlib.h R10_WARM_MAX: the background warm-up (2026-11-25) takes every recorded recipe.
FULL_LIMIT = 16384
DRAW = re.compile(rb"TRACE draw \d+: vs ([\da-f]{16}) ps ([\da-f]{16})")
GLES_FIRST_USE = re.compile(rb"GLES first use frame (\d+): vs ([\da-f]{16}) ps ([\da-f]{16})")


def build(captures: list[tuple[bytes, int]], traces: list[bytes], gles_captures: list[tuple[bytes, int]],
          limit: int = LIMIT, common_first: bool = False) -> bytes:
    stride = 8 + RECIPE_SIZE
    gameplay = []
    loading = []
    for capture, loaded_frame in captures:
        if not capture or len(capture) % stride:
            raise ValueError("Expected complete records from r10-pipelines.raw")
        records = [(struct.unpack_from("<Q", capture, at)[0], capture[at + 8:at + stride])
                   for at in range(0, len(capture), stride)]
        batches = Counter(frame for frame, _ in records)
        gameplay.extend((batches[frame], recipe) for frame, recipe in records if frame > loaded_frame)
        loading.extend(recipe for frame, recipe in records if frame <= loaded_frame)
    gameplay.sort(key=lambda item: -item[0])
    first_use = [recipe for _, recipe in gameplay]
    ordered = first_use + loading
    if common_first:
        # Sessions recorded at both ends of the graphics options: what every session made comes first, then the rest
        # in the usual order (gameplay recipes by the size of the burst their frame compiled, then loading ones).
        sets = [set(capture[at + 8:at + 8 + RECIPE_SIZE] for at in range(0, len(capture), 8 + RECIPE_SIZE))
                for capture, _ in captures]
        common = set.intersection(*sets) if sets else set()
        ordered = [r for r in ordered if r in common] + [r for r in ordered if r not in common]
    recipes = list(dict.fromkeys(ordered))[:limit]
    gles_pairs = []
    for capture, loaded_frame in gles_captures:
        records = [(int(frame), int(vs, 16), int(ps, 16)) for frame, vs, ps in GLES_FIRST_USE.findall(capture)]
        if not records:
            raise ValueError("Expected GLES first-use records in the log")
        batches = Counter(frame for frame, _, _ in records)
        gles_pairs.extend((batches[frame], struct.pack("<QQ", vs, ps))
                          for frame, vs, ps in records if frame > loaded_frame)
    gles_pairs.sort(key=lambda item: -item[0])
    pairs = [pair for _, pair in gles_pairs]
    pairs.extend(recipe[:16] for recipe in first_use)
    pairs.extend(struct.pack("<QQ", int(vs, 16), int(ps, 16))
                 for trace in traces for vs, ps in DRAW.findall(trace))
    pairs.extend(recipe[:16] for recipe in recipes)
    pairs = list(dict.fromkeys(pairs))[:limit]
    header = struct.pack("<4s5I", b"BSWP", 1, len(pairs), len(recipes), RECIPE_SIZE, 0)
    return b"".join([header, *pairs, *recipes])


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("capture", type=Path, help="Vulkan r10-pipelines.raw capture")
    parser.add_argument("--loaded-frame", type=int, required=True, help="Last frame recorded during the level load")
    parser.add_argument("--also", nargs=2, metavar=("CAPTURE", "LOADED_FRAME"), action="append", default=[])
    parser.add_argument("--gles-capture", nargs=2, metavar=("LOG", "LOADED_FRAME"), action="append", default=[])
    parser.add_argument("--gles-trace", type=Path, action="append", default=[])
    parser.add_argument("--output", type=Path, default=ROOT / "android/render10/bs_warmup.bin")
    parser.add_argument("--full", action="store_true",
                        help=f"keep up to {FULL_LIMIT} recipes and pairs, recipes of every capture first (background warm-up)")
    options = parser.parse_args()
    captures = [(options.capture.read_bytes(), options.loaded_frame)]
    captures.extend((Path(path).read_bytes(), int(frame)) for path, frame in options.also)
    gles_captures = [(Path(path).read_bytes(), int(frame)) for path, frame in options.gles_capture]
    contents = build(captures, [path.read_bytes() for path in options.gles_trace], gles_captures,
                     FULL_LIMIT if options.full else LIMIT, options.full)
    options.output.write_bytes(contents)
    _, _, pairs, pipelines, _, _ = struct.unpack_from("<4s5I", contents)
    print(f"Prepared {pairs} GLES programs and {pipelines} Vulkan pipelines in {options.output}")


if __name__ == "__main__":
    main()

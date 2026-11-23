"""Build a BioShock ZIP with lossless Russian Bink remuxes or legacy sidecars."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import shutil
import struct
import subprocess
import tempfile
import zipfile


def movie(path):
    data = path.read_bytes()
    signature, size, frames, _, _, width, height, rate, divisor, flags, tracks = struct.unpack_from("<4s10I", data)
    if signature not in (b"BIKg", b"BIKh", b"BIKi") or size + 8 != len(data) or not 0 < frames <= 1000000 or tracks > 256:
        raise ValueError(f"Invalid Bink header: {path}")
    index = 44 + tracks * 12
    offsets = [offset & ~1 for offset in struct.unpack_from(f"<{frames}I", data, index)] + [len(data)]
    if offsets[0] < index + frames * 4:
        raise ValueError(f"Invalid Bink index: {path}")
    packets = []
    digest = hashlib.sha256()
    for start, end in zip(offsets, offsets[1:]):
        if not start < end <= len(data):
            raise ValueError(f"Invalid Bink frame: {path}")
        video = start
        for _ in range(tracks):
            if video + 4 > end:
                raise ValueError(f"Invalid Bink audio packet: {path}")
            video += 4 + struct.unpack_from("<I", data, video)[0]
        if video >= end:
            raise ValueError(f"Missing Bink video packet: {path}")
        packets.append((start, video, end))
        digest.update(struct.pack("<I", end - video))
        digest.update(memoryview(data)[video:end])
    return (signature, frames, width, height, rate, divisor, flags, digest.digest()), packets, data


def sidecars(original, directory, out):
    bases = {}
    localized = sorted(directory.glob("*_rus.bik"))
    for path in directory.glob("*.bik"):
        if path in localized:
            continue
        identity, _, _ = movie(path)
        bases.setdefault(identity, []).append(path)
    for path in localized:
        identity, packets, data = movie(path)
        choices = bases.get(identity, ())
        if not choices:
            print(f"Keep complete: {path.name}")
            continue
        preferred = path.stem[:-4].casefold() + ".bik"
        base = next((p for p in choices if p.name.casefold() == preferred), choices[0])
        _, source, source_data = movie(base)
        name = base.name.encode("ascii")
        if not re.fullmatch(rb"[A-Za-z0-9_-]{1,100}\.bik", name, re.IGNORECASE):
            raise ValueError(f"Invalid base name: {base.name}")
        output = out / path.with_suffix(".bka").name
        crc = original.getinfo("Content/BinkMovies/" + path.name).CRC
        recovered = hashlib.sha256()
        with output.open("wb") as file:
            file.write(struct.pack("<4sIIII", b"BKA1", len(name), len(packets), len(data), crc))
            file.write(name)
            previous = 0
            for (_, audio_end, end), (_, video, video_end) in zip(packets, source):
                audio = data[previous:audio_end]
                frame = memoryview(source_data)[video:video_end]
                if frame != memoryview(data)[audio_end:end]:
                    raise ValueError(f"Video varies within a candidate pair: {path.name}")
                file.write(struct.pack("<III", len(audio), video, len(frame)))
                file.write(audio)
                recovered.update(audio)
                recovered.update(frame)
                previous = end
        if recovered.digest() != hashlib.sha256(data).digest():
            raise ValueError(f"Bink reconstruction mismatch: {path.name}")
        yield path.name, output.name


def track_ids(data):
    count = struct.unpack_from("<I", data, 40)[0]
    return struct.unpack_from(f"<{count}I", data, 44 + count * 8)


def audio_packet(data, start, track):
    for _ in range(track):
        start += 4 + struct.unpack_from("<I", data, start)[0]
    end = start + 4 + struct.unpack_from("<I", data, start)[0]
    return memoryview(data)[start:end]


def remux(original, localized, output):
    identity, frames, source = movie(original)
    voice_identity, voice_frames, voice = movie(localized)
    ids, voice_ids = track_ids(source), track_ids(voice)
    if identity != voice_identity or set(ids) != set(range(5)) or len(ids) != 5 or voice_ids.count(0) != 1:
        return False
    voice_track = voice_ids.index(0)
    index = 44 + len(ids) * 12
    if frames[0][0] != index + 4 * (len(frames) + 1) or struct.unpack_from("<I", source, frames[0][0] - 4)[0] != len(source):
        raise ValueError(f"Unexpected Bink frame index: {original.name}")

    header = bytearray(source[:44])
    struct.pack_into("<I", header, 40, len(ids) + 1)
    for table in range(3):
        start = 44 + table * len(ids) * 4
        header.extend(source[start:start + len(ids) * 4])
        voice_start = 44 + (table * len(voice_ids) + voice_track) * 4
        header.extend(voice[voice_start:voice_start + 4] if table < 2 else struct.pack("<I", 5))
    offsets = []
    largest = 0
    with output.open("wb") as file:
        file.seek(len(header) + 4 * (len(frames) + 1))
        for i, ((start, video, end), (ru_start, ru_video, ru_end)) in enumerate(zip(frames, voice_frames)):
            if memoryview(source)[video:end] != memoryview(voice)[ru_video:ru_end]:
                raise ValueError(f"Bink video packets differ: {original.name}")
            offsets.append(file.tell() | (struct.unpack_from("<I", source, index + i * 4)[0] & 1))
            packet = audio_packet(voice, ru_start, voice_track)
            largest = max(largest, end - start + len(packet))
            file.write(memoryview(source)[start:video])
            file.write(packet)
            file.write(memoryview(source)[video:end])
        size = file.tell()
        if size >= 1 << 31 or size & 1:
            raise ValueError(f"Invalid remuxed Bink size: {original.name}")
        struct.pack_into("<I", header, 4, size - 8)
        struct.pack_into("<I", header, 12, largest)
        file.seek(0)
        file.write(header)
        file.write(struct.pack(f"<{len(offsets) + 1}I", *offsets, size))
    return True


def sixth_tracks(directory, out):
    localized = sorted(directory.glob("*_rus.bik"))
    originals = {path.name.casefold(): path for path in directory.glob("*.bik")}
    changed, aliases = {}, {}
    masters = {}
    out.mkdir(parents=True)
    for path in localized:
        base = originals.get(path.stem[:-4].casefold() + ".bik")
        if base and remux(base, path, out / base.name):
            changed[path.name] = base.name
            masters[hashlib.sha256(path.read_bytes()).digest()] = base.name
    for path in localized:
        if path.name in changed:
            continue
        base = masters.get(hashlib.sha256(path.read_bytes()).digest())
        if not base:
            print(f"Keep complete: {path.name}")
            continue
        name = base.encode("ascii")
        if not re.fullmatch(rb"[A-Za-z0-9_-]{1,100}\.bik", name, re.IGNORECASE):
            raise ValueError(f"Invalid Bink alias target: {base}")
        alias = path.with_suffix(".bkr").name
        (out / alias).write_bytes(b"BKR1" + name)
        aliases[path.name] = base
    return changed, aliases


def build(source, destination, seven_zip, sixth_track=False):
    if source.resolve() == destination.resolve() or destination.exists():
        raise ValueError("Choose a new output ZIP")
    with tempfile.TemporaryDirectory(prefix="bioshock-bink-", dir=destination.resolve().parent) as tmp:
        root = Path(tmp)
        prefix = "Content/BinkMovies/"
        movies = root / prefix
        movies.mkdir(parents=True)
        with zipfile.ZipFile(source) as archive:
            original = {i.filename: (i.file_size, i.CRC) for i in archive.infolist() if not i.is_dir()}
            for name in original:
                if name.startswith(prefix) and name.lower().endswith(".bik"):
                    with archive.open(name) as inp, (movies / Path(name).name).open("wb") as out:
                        shutil.copyfileobj(inp, out)
            if sixth_track:
                stage = root / "replacement"
                merged, aliases = sixth_tracks(movies, stage / prefix)
                removed = [*merged, *aliases, *merged.values()]
                additions = [*merged.values(), *(Path(name).with_suffix(".bkr").name for name in aliases)]
            else:
                stage = root
                pairs = list(sidecars(archive, movies, movies))
                removed, additions = zip(*pairs) if pairs else ((), ())
        if not removed:
            raise ValueError("No Bink tracks could be separated")
        draft = root / "archive.zip"
        shutil.copyfile(source, draft)
        replaced = [prefix + name for name in removed]
        added = [prefix + name for name in additions]
        subprocess.run([seven_zip, "d", draft, *replaced, "-bsp0"], check=True)
        subprocess.run([seven_zip, "a", draft, *added, "-tzip", "-mm=Deflate", "-mx=6", "-bsp0"], cwd=stage, check=True)
        with zipfile.ZipFile(draft) as archive:
            entries = {i.filename: (i.file_size, i.CRC) for i in archive.infolist() if not i.is_dir()}
            if set(entries) != (original.keys() - set(replaced)) | set(added) or any(
                    entries[name] != meta for name, meta in original.items() if name not in replaced):
                raise ValueError("Compact ZIP inventory differs")
            bad = archive.testzip()
            if bad is not None:
                raise ValueError(f"ZIP CRC failure: {bad}")
        draft.rename(destination.resolve())
        saved = source.stat().st_size - destination.stat().st_size
        if sixth_track:
            report = dict(remuxed=merged, aliases=aliases, archive_bytes_saved=saved,
                          installed_bytes_saved=sum(original[name][0] for name in replaced) - sum(entries[name][0] for name in added))
            print(json.dumps(report, indent=2))
        else:
            print(f"Separated {len(pairs)} movies; saved {saved:,} bytes")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", type=Path)
    parser.add_argument("destination", type=Path)
    parser.add_argument("--seven-zip", default="7z")
    parser.add_argument("--sixth-track", action="store_true", help="Append Russian as track ID 5 for the Android port")
    args = parser.parse_args()
    build(args.source, args.destination, args.seven_zip, args.sixth_track)

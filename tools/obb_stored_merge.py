"""Create only the ZIP64 directory needed to join two stored OBB files without duplicating payload."""
import hashlib
import json
from pathlib import Path
import struct
import zipfile

UINT32_MAX = 0xFFFFFFFF


def sha(path):
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def records(path):
    raw = path.read_bytes()
    with zipfile.ZipFile(path) as archive:
        start = archive.start_dir
        count = len(archive.infolist())
    result = []
    position = start
    for _ in range(count):
        if raw[position:position + 4] != b'PK\x01\x02':
            raise ValueError('Invalid central record signature')
        name_length, extra_length, comment_length = struct.unpack_from('<HHH', raw, position + 28)
        total = 46 + name_length + extra_length + comment_length
        result.append(bytearray(raw[position:position + total]))
        position += total
    return result


def zip64_extra(extra, offset, had_offset):
    fields = bytearray()
    position = 0
    found = False
    while position + 4 <= len(extra):
        kind, size = struct.unpack_from('<HH', extra, position)
        if kind != 1:
            break
        value = extra[position + 4:position + 4 + size]
        if len(value) != size:
            raise ValueError('Truncated extra field')
        found = True
        if had_offset:
            if len(value) < 8:
                raise ValueError('Truncated ZIP64 offset')
            value = value[:-8] + struct.pack('<Q', offset)
        else:
            value += struct.pack('<Q', offset)
        fields += struct.pack('<HH', kind, len(value)) + value
        position += 4 + size
    if had_offset and not found:
        raise ValueError('ZIP64 offset is missing')
    if not found:
        fields += struct.pack('<HHQ', 1, 8, offset)
    fields += extra[position:]
    return bytes(fields)


def join_records(main_size, main_records, patch_records):
    output = []
    for source, delta in ((main_records, 0), (patch_records, main_size)):
        for record in source:
            original = struct.unpack_from('<I', record, 42)[0]
            if not delta:
                output.append(bytes(record))
                continue
            adjusted = original + delta
            if original != UINT32_MAX and adjusted <= UINT32_MAX:
                struct.pack_into('<I', record, 42, adjusted)
                output.append(bytes(record))
                continue
            had_offset = original == UINT32_MAX
            name_length, extra_length = struct.unpack_from('<HH', record, 28)[:2]
            extra_start = 46 + name_length
            extra = zip64_extra(record[extra_start:extra_start + extra_length], adjusted, had_offset)
            record[extra_start:extra_start + extra_length] = extra
            struct.pack_into('<H', record, 30, len(extra))
            struct.pack_into('<I', record, 42, UINT32_MAX)
            output.append(bytes(record))
    return output


def metadata_parts(main_size, main_records, patch_size, patch_records, main_sha256, patch_sha256):
    central = b''.join(join_records(main_size, main_records, patch_records))
    combined_size = main_size + patch_size
    count = len(main_records) + len(patch_records)
    zip64_offset = combined_size + len(central)
    trailer = struct.pack('<4sQ2H2L4Q', b'PK\x06\x06', 44, 45, 45, 0, 0, count, count, len(central), combined_size)
    trailer += struct.pack('<4sIQI', b'PK\x06\x07', 0, zip64_offset, 1)
    trailer += struct.pack('<4s4H2IH', b'PK\x05\x06', 0, 0, count, count, len(central), min(combined_size, UINT32_MAX), 0)
    return central + trailer, {
        'entries': count,
        'main_bytes': main_size,
        'main_sha256': main_sha256,
        'patch_bytes': patch_size,
        'patch_sha256': patch_sha256,
        'combined_bytes': combined_size + len(central) + len(trailer),
    }


def metadata(main, patch):
    return metadata_parts(main.stat().st_size, records(main), patch.stat().st_size, records(patch), sha(main), sha(patch))


def write_metadata(main, patch, output):
    data, receipt = metadata(main, patch)
    receipt['metadata_sha256'] = hashlib.sha256(data).hexdigest()
    output.write_bytes(data)
    output.with_suffix('.json').write_text(json.dumps(receipt, indent=2) + '\n')
    return receipt


def main():
    import argparse
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--main', type=Path, default=Path('build/obb/main.1.org.bioshock.port.obb'))
    parser.add_argument('--patch', type=Path, default=Path('build/obb/patch.1.org.bioshock.port.obb'))
    parser.add_argument('--output', type=Path, default=Path('build/obb-stored-merge/metadata.bin'))
    args = parser.parse_args()
    args.output.parent.mkdir(parents=True, exist_ok=True)
    print(json.dumps(write_metadata(args.main, args.patch, args.output), indent=2))


if __name__ == '__main__':
    main()

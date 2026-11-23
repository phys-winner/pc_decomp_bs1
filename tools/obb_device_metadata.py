"""Build ZIP64 join metadata for OBBs on a test device without downloading their payload."""
import argparse
import hashlib
import json
from pathlib import Path
import struct
import subprocess

from obb_stored_merge import UINT32_MAX, metadata_parts


def adb(serial, *args, binary=False):
    result = subprocess.run(['adb', '-s', serial, *args], check=True, capture_output=True)
    return result.stdout if binary else result.stdout.decode().strip()


def source_info(serial, path):
    size = int(adb(serial, 'shell', 'stat', '-c', '%s', path))
    digest = adb(serial, 'shell', 'sha256sum', path).split()[0]
    tail_size = min(size, 8 << 20)
    raw = adb(serial, 'exec-out', f'tail -c {tail_size} {path}', binary=True)
    position = raw.rfind(b'PK\x05\x06')
    if position < 0:
        raise ValueError('EOCD not found')
    values = struct.unpack_from('<4s4H2IH', raw, position)
    count, central_size, offset = values[3], values[5], values[6]
    if offset == UINT32_MAX:
        position = raw.rfind(b'PK\x06\x06')
        if position < 0:
            raise ValueError('ZIP64 EOCD not found')
        values = struct.unpack_from('<4sQ2H2L4Q', raw, position)
        count, central_size, offset = values[7], values[8], values[9]
    start = offset - (size - len(raw))
    if start < 0 or start + central_size > len(raw):
        raise ValueError('Central directory is outside the tail')
    raw = raw[start:start + central_size]
    result = []
    position = 0
    for _ in range(count):
        if raw[position:position + 4] != b'PK\x01\x02':
            raise ValueError('Invalid central record signature')
        name_length, extra_length, comment_length = struct.unpack_from('<HHH', raw, position + 28)
        total = 46 + name_length + extra_length + comment_length
        result.append(bytearray(raw[position:position + total]))
        position += total
    if position != len(raw):
        raise ValueError('Central directory size mismatch')
    return size, result, digest


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--serial', required=True)
    parser.add_argument('--main', default='/sdcard/Android/obb/org.bioshock.port/main.1.org.bioshock.port.obb')
    parser.add_argument('--patch', default='/sdcard/Android/obb/org.bioshock.port/patch.1.org.bioshock.port.obb')
    parser.add_argument('--output', type=Path, default=Path('build/obb-device-merge/metadata.bin'))
    args = parser.parse_args()
    main_size, main_records, main_sha = source_info(args.serial, args.main)
    patch_size, patch_records, patch_sha = source_info(args.serial, args.patch)
    data, receipt = metadata_parts(main_size, main_records, patch_size, patch_records, main_sha, patch_sha)
    receipt['metadata_sha256'] = hashlib.sha256(data).hexdigest()
    receipt['device_model'] = 'RMX3371'
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_bytes(data)
    args.output.with_suffix('.json').write_text(json.dumps(receipt, indent=2) + '\n')
    print(json.dumps(receipt, indent=2))


if __name__ == '__main__':
    main()

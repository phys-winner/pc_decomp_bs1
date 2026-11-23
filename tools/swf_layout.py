"""Print SWF button placements from the original resource, without modifying it."""
import argparse
import struct
import zlib


class Bits:
    def __init__(self, data):
        self.data, self.position = data, 0

    def read(self, count):
        value = 0
        for index in range(count):
            bit = (self.data[self.position >> 3] >> (7 - (self.position & 7))) & 1
            value = (value << 1) | bit
            self.position += 1
        return value

    def signed(self, count):
        if not count:
            return 0
        value = self.read(count)
        return value - (1 << count) if value & (1 << (count - 1)) else value


def matrix(data):
    bits = Bits(data)
    if bits.read(1):
        bits.read(5)
        bits.signed(bits.read(5))
        bits.signed(bits.read(5))
    if bits.read(1):
        bits.signed(bits.read(5))
        bits.signed(bits.read(5))
    count = bits.read(5)
    x = bits.signed(count) if count else 0
    y = bits.signed(count) if count else 0
    return (x / 20, y / 20, (bits.position + 7) // 8)


def color_transform(data):
    bits = Bits(data)
    add, multiply = bits.read(1), bits.read(1)
    count = bits.read(4)
    if multiply:
        for _ in range(4):
            bits.signed(count)
    if add:
        for _ in range(4):
            bits.signed(count)
    return (bits.position + 7) // 8


def walk(data, container, buttons):
    placements = []
    position = 0
    while position + 2 <= len(data):
        record, = struct.unpack_from("<H", data, position)
        position += 2
        code, length = record >> 6, record & 63
        if length == 63:
            length, = struct.unpack_from("<I", data, position)
            position += 4
        payload = data[position:position + length]
        if code in (7, 34):
            if len(payload) >= 2:
                buttons.add(struct.unpack_from("<H", payload)[0])
        elif code == 39 and len(payload) >= 2:
            sprite = struct.unpack_from("<H", payload)[0]
            placements.extend(walk(payload[2:], f"{container}/sprite:{sprite}", buttons))
        elif code == 4 and len(payload) >= 6:
            character, depth = struct.unpack_from("<HH", payload)
            try:
                x, y, _ = matrix(payload[4:])
            except IndexError:
                print("bad_place4", payload.hex())
                raise
            placements.append((container, character, depth, "", (x, y)))
        elif code in (26, 70) and payload:
            flags, depth = payload[0], struct.unpack_from("<H", payload, 1)[0]
            offset = 3
            character = struct.unpack_from("<H", payload, offset)[0] if flags & 2 else 0
            offset += 2 if flags & 2 else 0
            try:
                x, y, used = matrix(payload[offset:]) if flags & 4 else (0, 0, 0)
            except IndexError:
                print("bad_place", hex(flags), "depth", depth, "offset", offset, "bytes", len(payload), "hex", payload[offset:].hex())
                raise
            offset += used
            offset += color_transform(payload[offset:]) if flags & 8 else 0
            offset += 2 if flags & 16 else 0
            name = ""
            if flags & 32:
                end = payload.index(0, offset)
                name = payload[offset:end].decode("utf-8", errors="replace")
            if character in buttons or name:
                placements.append((container, character, depth, name, (x, y)))
        position += length
    return placements


def parse(path):
    raw = path.read_bytes()
    if raw[:3] == b"CWS":
        raw = zlib.decompress(raw[8:])
    elif raw[:3] != b"FWS":
        raise ValueError(f"Unsupported SWF signature: {raw[:3]}")
    bits = Bits(raw)
    count = bits.read(5)
    header = (5 + count * 4 + 7) // 8 + 12 + (8 if raw[:3] == b"FWS" else 0)
    buttons = set()
    return buttons, walk(raw[header:], "", buttons)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("path", type=__import__('pathlib').Path)
    args = parser.parse_args()
    buttons, placements = parse(args.path)
    print("button_ids", sorted(buttons))
    for container, character, depth, name, placement in placements:
        print(f"{container} button={character} depth={depth} name={name!r} xy={placement}")


if __name__ == "__main__":
    main()

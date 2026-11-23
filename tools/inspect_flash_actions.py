"""Disassemble SWF action streams containing a requested string, read-only."""
import argparse
import struct
import zlib
from pathlib import Path

NAMES = {
    0x00: "End", 0x04: "NextFrame", 0x05: "PreviousFrame", 0x06: "Play", 0x07: "Stop",
    0x08: "ToggleQuality", 0x09: "StopSounds", 0x0A: "FSCommand", 0x0C: "Delete",
    0x12: "Not", 0x17: "Pop", 0x1C: "GetVariable", 0x1D: "SetVariable", 0x20: "SetTarget2",
    0x22: "GetProperty", 0x23: "SetProperty", 0x24: "CloneSprite", 0x26: "Trace",
    0x2D: "RemoveSprite", 0x34: "GetTime", 0x3D: "CallFunction", 0x40: "NewObject",
    0x47: "Add", 0x48: "Less", 0x49: "Equals", 0x4E: "GetMember", 0x4F: "SetMember",
    0x50: "Increment", 0x51: "Decrement", 0x52: "CallMethod", 0x53: "NewMethod",
    0x54: "InstanceOf", 0x55: "Enumerate2", 0x60: "BitAnd", 0x61: "BitOr", 0x62: "BitXor",
    0x63: "BitLShift", 0x64: "BitRShift", 0x65: "BitURShift", 0x66: "StrictEquals",
    0x81: "GotoFrame", 0x83: "GetURL", 0x87: "StoreRegister", 0x88: "ConstantPool",
    0x8A: "WaitForFrame", 0x8B: "SetTarget", 0x8C: "GotoLabel", 0x8D: "WaitForFrame2",
    0x96: "Push", 0x99: "Jump", 0x9A: "GetURL2", 0x9B: "DefineFunction", 0x9D: "If",
    0x9E: "Call", 0x9F: "GotoFrame2",
}


def parse_actions(data, base):
    p = 0
    while p < len(data):
        start = p
        op = data[p]
        p += 1
        size = struct.unpack_from("<H", data, p)[0] if op >= 0x80 else 0
        p += 2 if op >= 0x80 else 0
        payload = data[p:p + size]
        text = NAMES.get(op, f"Op_{op:02X}")
        if op == 0x96:
            values = []
            q = 0
            while q < len(payload):
                kind = payload[q]
                q += 1
                if kind == 0:
                    end = payload.index(0, q)
                    values.append(repr(payload[q:end].decode("latin-1")))
                    q = end + 1
                elif kind == 1:
                    values.append(str(struct.unpack_from("<f", payload, q)[0]))
                    q += 4
                elif kind in (2, 3):
                    values.append("null" if kind == 2 else "undefined")
                elif kind == 4:
                    values.append(f"r{payload[q]}")
                    q += 1
                elif kind == 5:
                    values.append(str(bool(payload[q])))
                    q += 1
                elif kind == 6:
                    values.append(str(struct.unpack_from("<d", payload, q)[0]))
                    q += 8
                elif kind == 7:
                    values.append(str(struct.unpack_from("<i", payload, q)[0]))
                    q += 4
                elif kind in (8, 10):
                    values.append(f"c{payload[q]}" if kind == 8 else f"const{payload[q]}")
                    q += 1
                elif kind == 9:
                    values.append(f"c{struct.unpack_from('<H', payload, q)[0]}")
                    q += 2
                else:
                    raise ValueError(f"Unknown push type {kind} at {base + p + q}")
            text += " " + ", ".join(values)
        elif op in (0x83, 0x8B, 0x8C):
            text += " " + repr(payload.rstrip(b"\0").decode("latin-1"))
        elif op == 0x88:
            count = struct.unpack_from("<H", payload)[0]
            values = payload[2:].split(b"\0")[:count]
            text += " " + repr([value.decode("latin-1") for value in values])
        elif op in (0x99, 0x9D):
            displacement = struct.unpack_from("<h", payload)[0]
            text += f" -> {base + p + size + displacement:08X}"
        elif op in (0x81,):
            text += f" {struct.unpack_from('<H', payload)[0]}"
        elif op == 0x9B:
            count = struct.unpack_from("<H", payload, len(payload) - 2)[0]
            text += f" body_to={base + p + count:08X}"
        elif size:
            text += " " + payload.hex()
        yield base + start, text
        p += size


def tags(data, base):
    frame = 0
    position = 0
    while position + 6 <= len(data):
        at = position
        header = struct.unpack_from("<H", data, position)[0]
        position += 2
        tag, size = header >> 6, header & 63
        if size == 63:
            size = struct.unpack_from("<I", data, position)[0]
            position += 4
        body = data[position:position + size]
        if tag == 1:
            frame += 1
        yield tag, frame, base + at, body, base + position
        if tag == 39 and len(body) >= 2:
            yield from tags(body[2:], base + position + 2)
        position += size


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("path", type=Path)
    parser.add_argument("needle", nargs="?", default="LaunchMainMenu")
    args = parser.parse_args()
    data = args.path.read_bytes()
    if data[:3] == b"CWS":
        data = b"FWS" + data[3:8] + zlib.decompress(data[8:])
    if data[:3] != b"FWS":
        raise ValueError("Only FWS/CWS input is supported")
    p = 8
    bits = data[p] >> 3
    p += (5 + 4 * bits + 7) // 8 + 4
    for tag, frame, at, body, p in tags(data[p:], p):
        if tag in (12, 59) and args.needle.encode("latin-1") in body:
            actions = body[2:] if tag == 59 else body
            print(f"tag={tag} frame={frame} address={at:08X} bytes={len(body)}")
            for address, text in parse_actions(actions, p + (2 if tag == 59 else 0)):
                print(f"{address:08X}: {text}")
        elif tag == 34 and len(body) >= 5 and args.needle.encode("latin-1") in body:
            cursor = 3 + struct.unpack_from("<H", body, 3)[0]
            print(f"tag={tag} frame={frame} address={at:08X} bytes={len(body)}")
            while cursor + 3 <= len(body):
                flags, key = struct.unpack_from("<HB", body, cursor)
                cursor += 3
                print(f"button_event flags=0x{flags:04X} key={key}")
                for address, text in parse_actions(body[cursor:], p + cursor):
                    print(f"{address:08X}: {text}")
                    if text == "End":
                        cursor = address - p + 1
                        break
                else:
                    break


if __name__ == "__main__":
    main()

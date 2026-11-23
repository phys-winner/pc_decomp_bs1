"""Identity of an AArch64 shared library's runtime payload, independent of strip."""
import hashlib
import json
import struct


def _require(condition, message):
    if not condition:
        raise ValueError(message)


def _sha(data):
    return hashlib.sha256(data).hexdigest()


class _Elf:
    def __init__(self, data):
        self.data = data
        _require(len(data) >= 64 and data[:6] == b"\x7fELF\x02\x01", "Expected a little-endian ELF64 library")
        h = struct.unpack_from("<16sHHIQQQIHHHHHH", data)
        _require(h[1:4] == (3, 183, 1), "Expected an AArch64 shared library")
        _, _, _, _, _, phoff, shoff, _, _, phsize, phcount, shsize, shcount, shstrings = h
        _require(phsize == 56 and shsize == 64 and shcount > 0 and shstrings < shcount,
                 "Unsupported ELF header tables")
        self.span(phoff, phsize * phcount)
        self.span(shoff, shsize * shcount)
        self.programs = [struct.unpack_from("<IIQQQQQQ", data, phoff + i * phsize) for i in range(phcount)]
        headers = [struct.unpack_from("<IIQQQQIIQQ", data, shoff + i * shsize) for i in range(shcount)]
        strings = self.span(headers[shstrings][4], headers[shstrings][5])
        self.sections = {}
        for header in headers:
            name = self.string(strings, header[0])
            _require(name not in self.sections, "Duplicate ELF section name")
            self.sections[name] = header

    def span(self, offset, size):
        _require(0 <= offset <= len(self.data) and 0 <= size <= len(self.data) - offset, "ELF range outside file")
        return self.data[offset:offset + size]

    @staticmethod
    def string(data, offset):
        _require(0 <= offset < len(data), "Invalid ELF string offset")
        end = data.find(b"\0", offset)
        _require(end >= offset, "Unterminated ELF string")
        return data[offset:end].decode("utf-8")

    def content(self, name):
        _require(name in self.sections, "Missing ELF section: " + name)
        h = self.sections[name]
        return b"" if h[1] == 8 else self.span(h[4], h[5])

    def build_id(self):
        for name, h in self.sections.items():
            if h[1] != 7:
                continue
            data, offset = self.content(name), 0
            while offset + 12 <= len(data):
                names, size, kind = struct.unpack_from("<III", data, offset)
                start = offset + 12 + ((names + 3) & ~3)
                _require(start + size <= len(data), "Truncated ELF note")
                if data[offset + 12:offset + 12 + names] == b"GNU\0" and kind == 3:
                    return data[start:start + size].hex()
                offset = start + ((size + 3) & ~3)
        return None


def _normalized_header(data):
    header = bytearray(data[:64])
    # Stripping moves/removes section headers. These fields are not executable
    # payload; program headers, entry address and all other header fields stay exact.
    header[40:48] = b"\0" * 8
    header[60:64] = b"\0" * 4
    return bytes(header)


def runtime_identity(data):
    """Return canonical runtime/ALLOC hashes and ABI evidence; build_id can be None."""
    elf = _Elf(data)
    allocated = []
    for name, h in sorted(elf.sections.items()):
        if h[2] & 2:
            allocated.append({"name": name, "type": h[1], "flags": h[2], "address": h[3],
                              "offset": h[4], "size": h[5], "alignment": h[8], "entry_size": h[9],
                              "sha256": _sha(elf.content(name))})
    _require(allocated, "ELF has no allocated sections")
    loads = []
    for h in elf.programs:
        if h[0] != 1:
            continue
        payload = elf.span(h[2], h[5])
        if h[2] < 64:
            normalized = _normalized_header(data)
            end = min(64, h[2] + h[5])
            payload = normalized[h[2]:end] + payload[end - h[2]:]
        loads.append({"program_header": list(h), "sha256": _sha(payload)})
    _require(loads, "ELF has no loadable segments")
    dynstr = elf.content(".dynstr")
    needed = [elf.string(dynstr, value) for kind, value in struct.iter_unpack("<qQ", elf.content(".dynamic")) if kind == 1]
    symbols = list(struct.iter_unpack("<IBBHQQ", elf.content(".dynsym")))
    exports = [elf.string(dynstr, name) for name, info, other, section, value, size in symbols
               if info >> 4 in (1, 2) and section != 0]
    jni = sorted(name for name in exports if name.startswith("Java_") or name == "JNI_OnLoad")
    _require(jni, "Game library has no JNI exports")
    relocations = {name: _sha(elf.content(name)) for name, h in elf.sections.items() if h[2] & 2 and h[1] in (4, 9, 19)}
    canonical = lambda value: _sha(json.dumps(value, sort_keys=True, separators=(",", ":")).encode())
    allocated_sha = canonical(allocated)
    result = {"allocated_sha256": allocated_sha, "allocated_sections": allocated,
              "load_segments": loads, "build_id": elf.build_id(), "needed": needed,
              "dynsym_sha256": _sha(elf.content(".dynsym")), "dynamic_symbol_records": len(symbols),
              "exports": sorted(exports), "jni_exports": jni, "relocations": relocations,
              "header_sha256": _sha(_normalized_header(data)), "program_headers": [list(h) for h in elf.programs]}
    result["sha256"] = canonical(result)
    result["has_symbols"] = ".symtab" in elf.sections and ".strtab" in elf.sections
    return result


def assert_runtime_identity(source, shipped):
    """Require identical runtime bytes/ABI; return source identity, or raise ValueError."""
    before, after = runtime_identity(source), runtime_identity(shipped)
    _require(before["sha256"] == after["sha256"], "Stripped library runtime payload or ABI differs from its symbols")
    return before

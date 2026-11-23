"""Probe the original game's limited Flash String prototype on its x86 instructions."""
import hashlib
import json
from pathlib import Path
import struct
import unittest

import pefile
from unicorn import Uc, UC_ARCH_X86, UC_MODE_32, UC_HOOK_CODE
from unicorn.x86_const import (UC_X86_REG_EAX, UC_X86_REG_EBX, UC_X86_REG_ECX,
                              UC_X86_REG_EDX, UC_X86_REG_ESI, UC_X86_REG_EDI,
                              UC_X86_REG_EBP, UC_X86_REG_ESP, UC_X86_REG_EIP)

ROOT = Path(__file__).resolve().parents[1]
ENTRY, STRICMP_IAT = 0x10C04DA0, 0x10FCC5A0
STRICMP, RETURN, STACK = 0x50001000, 0x50002000, 0x108000
RESULT, RECEIVER, METHOD = 0x200000, 0x201000, 0x202000


class BioshockStringTest(unittest.TestCase):
    def test_original_string_split_is_undefined_and_indexof_is_supported(self):
        inputs = json.loads((ROOT / "config/inputs.json").read_text(encoding="utf-8"))
        raw = (Path(inputs["source_root"]) / "Builds/Release/Bioshock.exe").read_bytes()
        self.assertEqual(hashlib.sha256(raw).hexdigest(),
                         "7f2ee821ad60eaf08322b4bc6ebbf839d7fe010b4d0255ec12b5be4a2966f7f6")
        pe = pefile.PE(data=raw)
        base = pe.OPTIONAL_HEADER.ImageBase
        self.assertEqual(base, 0x10900000)
        for address, name in ((0x11040B68, b"charCodeAt"), (0x11040B60, b"indexOf"), (0x11040B54, b"substring")):
            self.assertEqual(pe.get_data(address - base, len(name) + 1), name + b"\0")
        imports = {symbol.address: symbol.name for dll in pe.DIRECTORY_ENTRY_IMPORT for symbol in dll.imports}
        self.assertEqual(imports[STRICMP_IAT], b"_stricmp")

        for method, argument_count, expected_type, expected_number in ((b"split", 1, 0, 0.0), (b"indexOf", 0, 4, -1.0)):
            with self.subTest(method=method.decode()):
                uc = Uc(UC_ARCH_X86, UC_MODE_32)
                uc.mem_map(base, pe.OPTIONAL_HEADER.SizeOfImage)
                uc.mem_write(base, pe.get_memory_mapped_image())
                uc.mem_map(0, 0x1000)  # The entry's ordinary FS:[0] SEH save/restore; FS defaults to base zero.
                uc.mem_map(0x100000, 0x10000)
                uc.mem_map(0x200000, 0x4000)
                uc.mem_map(0x50000000, 0x3000)
                uc.mem_write(0, struct.pack("<I", 0xDEADBEEF))
                uc.mem_write(STRICMP_IAT, struct.pack("<I", STRICMP))
                uc.mem_write(RESULT, b"\xA5" * 28)
                uc.mem_write(RECEIVER, bytes([5]) + b"0,60\0")
                uc.mem_write(METHOD, bytes([len(method) + 1]) + method + b"\0")
                # cdecl: result, argument-array, receiver string, method string, argument count, top argument index.
                uc.mem_write(STACK, struct.pack("<7I", RETURN, RESULT, 0, RECEIVER, METHOD, argument_count, 0))
                uc.reg_write(UC_X86_REG_ESP, STACK)
                preserved = {UC_X86_REG_EBX: 0x11223344, UC_X86_REG_ESI: 0x22334455,
                             UC_X86_REG_EDI: 0x33445566, UC_X86_REG_EBP: 0x44556677}
                for register, value in preserved.items():
                    uc.reg_write(register, value)
                uc.reg_write(UC_X86_REG_ECX, 0x55667788)
                uc.reg_write(UC_X86_REG_EDX, 0x66778899)
                comparisons, visited = [], set()

                def string_at(address):
                    text = bytes(uc.mem_read(address, 64))
                    self.assertIn(b"\0", text)
                    return text.split(b"\0", 1)[0]

                def boundary(machine, address, size, data):
                    visited.add(address)
                    if address != STRICMP:
                        return
                    sp = machine.reg_read(UC_X86_REG_ESP)
                    resume, left, right = struct.unpack("<3I", machine.mem_read(sp, 12))
                    a, b = string_at(left), string_at(right)
                    comparisons.append((a, b))
                    # Only the documented CRT boundary is supplied; every prototype branch is original PE code.
                    machine.reg_write(UC_X86_REG_EAX, (int(a.lower() > b.lower()) - int(a.lower() < b.lower())) & 0xFFFFFFFF)
                    machine.reg_write(UC_X86_REG_ESP, sp + 4)
                    machine.reg_write(UC_X86_REG_EIP, resume)

                uc.hook_add(UC_HOOK_CODE, boundary)
                uc.emu_start(ENTRY, RETURN, count=2000)
                self.assertEqual(uc.reg_read(UC_X86_REG_EIP), RETURN)
                self.assertEqual(uc.reg_read(UC_X86_REG_ESP), STACK + 4)
                self.assertEqual(uc.reg_read(UC_X86_REG_EAX), RESULT)
                for register, value in preserved.items():
                    self.assertEqual(uc.reg_read(register), value)
                self.assertEqual(bytes(uc.mem_read(0, 4)), struct.pack("<I", 0xDEADBEEF))
                value = bytes(uc.mem_read(RESULT, 28))
                self.assertEqual(struct.unpack_from("<I", value)[0], expected_type)
                self.assertEqual(struct.unpack_from("<d", value, 20)[0], expected_number)
                self.assertEqual(comparisons, [(method, name) for name in
                                 ([b"charCodeAt", b"indexOf", b"substring"] if method == b"split" else [b"charCodeAt", b"indexOf"])])
                self.assertIn(0x10C0508A if method == b"split" else 0x10C04E9E, visited)
        print("PASS original x86 Flash String prototype: split returns Undefined(type0); supported indexOf/noargs returns Number(type4,-1), cdecl/SEH/callee-saved state preserved; only _stricmp is a boundary fixture")


if __name__ == "__main__":
    unittest.main()

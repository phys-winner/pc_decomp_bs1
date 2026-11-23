"""Original x86 and checked-emission oracles for the two mobile PCS seams."""
import hashlib
import json
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest

import pefile
from capstone import Cs, CS_ARCH_X86, CS_MODE_32
from unicorn import Uc, UC_ARCH_X86, UC_MODE_32, UC_HOOK_CODE
from unicorn.x86_const import (UC_X86_REG_EAX, UC_X86_REG_EBX, UC_X86_REG_ECX, UC_X86_REG_EDX,
                              UC_X86_REG_ESI, UC_X86_REG_EDI, UC_X86_REG_EBP, UC_X86_REG_ESP,
                              UC_X86_REG_EIP, UC_X86_REG_EFLAGS)

from tests.host_environment import CLANG, environment

ROOT = Path(__file__).resolve().parents[1]
EXE_SHA256 = "7f2ee821ad60eaf08322b4bc6ebbf839d7fe010b4d0255ec12b5be4a2966f7f6"
REGISTERS = (UC_X86_REG_EAX, UC_X86_REG_ECX, UC_X86_REG_EDX, UC_X86_REG_EBX,
             UC_X86_REG_ESP, UC_X86_REG_EBP, UC_X86_REG_ESI, UC_X86_REG_EDI)
INITIAL = (0x12345678, 0x23456789, 0x3456789A, 0x456789AB,
           0x108000, 0x108080, 0x56789ABC, 0x6789ABCD)


def original():
    inputs = json.loads((ROOT / "config/inputs.json").read_text(encoding="utf-8"))
    raw = (Path(inputs["source_root"]) / "Builds/Release/Bioshock.exe").read_bytes()
    assert hashlib.sha256(raw).hexdigest() == EXE_SHA256
    return pefile.PE(data=raw)


def checked_emissions():
    # The vendor has its own tools package. This function runs only in a child
    # interpreter so later unittest imports retain the project's tools package.
    sys.path.insert(0, str(ROOT / "tools"))
    from lift_game import CheckedLifter
    from disasm.disasm32 import Instruction

    pe = original()
    decoder = Cs(CS_ARCH_X86, CS_MODE_32)
    decoder.detail = True

    def instruction(data, address):
        decoded = next(decoder.disasm(data, address))
        return Instruction(decoded.address, len(data), decoded.mnemonic, decoded.op_str, data, decoded.operands)

    compare = pe.get_data(0x10D7A848 - pe.OPTIONAL_HEADER.ImageBase, 7)
    branch = pe.get_data(0x10D7A84F - pe.OPTIONAL_HEADER.ImageBase, 2)
    end_call = pe.get_data(0x10D478A3 - pe.OPTIONAL_HEADER.ImageBase, 5)
    push = pe.get_data(0x10AF7770 - pe.OPTIONAL_HEADER.ImageBase, 1)
    assert compare.hex() == "833d48031c1100" and branch.hex() == "7455"
    assert end_call.hex() == "e8c8fedaff" and push.hex() == "53"

    def primed():
        lifter = CheckedLifter()
        prefix = lifter.lift_instruction(instruction(compare, 0x10D7A848))
        return lifter, prefix

    lifter, prefix = primed()
    state, sequence = lifter._flag_state, lifter._flag_seq
    gate = lifter.lift_instruction(instruction(branch, 0x10D7A84F))
    assert (lifter._flag_state, lifter._flag_seq) == (state, sequence)
    assert len(gate) == 1 and "CMP_EQ(_flag_a, _flag_b) && !void_bioshock_mobile_cache(" in gate[0]
    assert "RECOMP_FLAGS" not in gate[0] and "MEM32(0x111C0348) =" not in gate[0]
    for changed in (bytes.fromhex("7555"), bytes.fromhex("7454"), bytes.fromhex("0f8451000000")):
        lifter, _ = primed()
        try:
            lifter.lift_instruction(instruction(changed, 0x10D7A84F))
            raise AssertionError("Changed original store branch accepted")
        except ValueError as error:
            assert "store gate changed" in str(error)
    lifter, _ = primed()
    assert "void_bioshock_mobile_cache" not in "\n".join(lifter.lift_instruction(instruction(branch, 0x10D7A850)))

    lifter, baseline = CheckedLifter(), CheckedLifter()
    end = lifter.lift_instruction(instruction(end_call, 0x10D478A3))
    unchanged = super(CheckedLifter, baseline).lift_instruction(instruction(end_call, 0x10D478A3))
    assert (lifter._flag_state, lifter._flag_seq) == (baseline._flag_state, baseline._flag_seq)
    assert end[1:] == unchanged == ["RECOMP_CALL(sub_10AF7770); /* 0x10D478A3: call 0x10af7770 */", "RECOMP_FLAGS_IN();"]
    assert len(end) == 3
    order = ("RECOMP_FLAGS_OUT();", "RECOMP_REGS_OUT();", "void_bioshock_mobile_cache_flush(ecx);",
             "RECOMP_REGS_IN();", "RECOMP_FLAGS_IN();")
    assert [end[0].index(token) for token in order] == sorted(end[0].index(token) for token in order)
    for changed in (bytes.fromhex("e8c9fedaff"), bytes.fromhex("e9c8fedaff"), bytes.fromhex("e8c8fedaff90")):
        try:
            CheckedLifter().lift_instruction(instruction(changed, 0x10D478A3))
            raise AssertionError("Changed original EndPrecaching clear call accepted")
        except ValueError as error:
            assert "EndPrecaching" in str(error)
    assert "mobile_cache_flush" not in "\n".join(CheckedLifter().lift_instruction(instruction(end_call, 0x10D478A4)))
    # Reader reloads and destruction see only partial runtime maps after End.
    # Their shared Clear must retain exactly its original first push.
    assert CheckedLifter().lift_instruction(instruction(push, 0x10AF7770)) == [
        "PUSH32(esp, ebx); /* 0x10AF7770: push ebx */"]

    source = ROOT / "generated/bioshock"
    expected = {"functions_0191.c": "\n".join("    " + line for line in end),
                "functions_0197.c": "    " + gate[0]}
    assert "void_bioshock_mobile_cache" not in (source / "functions_0097.c").read_text(encoding="utf-8")
    found = set()
    for path in source.glob("*.c"):
        text = path.read_text(encoding="utf-8")
        if "void_bioshock_mobile_cache" in text:
            found.add(path.name)
            assert path.name in expected and text.count(expected[path.name]) == 1
    assert found == set(expected)
    assert all("void_bioshock_mobile_cache" not in path.read_text(encoding="utf-8") for path in source.glob("*.h"))
    return {"compare": prefix, "gate": gate, "end": end}


def inspect_in_subprocess():
    result = subprocess.run([sys.executable, "-B", "-c",
                             "import json; from tests.test_mobile_cache_persistence_oracle import checked_emissions; "
                             "print(json.dumps(checked_emissions()))"], cwd=ROOT, capture_output=True,
                            text=True, encoding="utf-8", errors="replace", timeout=30)
    if result.returncode:
        raise AssertionError(result.stdout + result.stderr)
    return json.loads(result.stdout)


class MobileCachePersistenceOracleTest(unittest.TestCase):
    def test_checked_lifter_pins_both_sites_without_headers_or_global_flag_write(self):
        emission = inspect_in_subprocess()
        self.assertEqual(len(emission["end"]), 3)
        self.assertEqual(len(emission["gate"]), 1)

    def test_emitted_store_gate_matches_original_x86_off_and_preserves_flags(self):
        emission = inspect_in_subprocess()
        pe = original()
        initial_text = ",".join(hex(value) for value in INITIAL)
        program = r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
enum {FK_NONE=0,FK_CMP=1};
#define CMP_B(a,b) ((uint32_t)(a)<(uint32_t)(b))
#define CMP_EQ(a,b) ((uint32_t)(a)==(uint32_t)(b))
static uint32_t registers[]={/* INITIAL */},editor;
static int mobile,calls;
#define ebp registers[5]
static uint32_t memory(uint32_t address){
    if(address==0x111C0348)return editor;
    if(address==registers[5]+8)return 0x200000;
    if(address==0x20015C)return 0x201000;
    if(address==0x2010C8)return 0x202000;
    assert(0);return 0;
}
#define MEM32(address) memory(address)
int void_bioshock_mobile_cache(uint32_t cache){assert(cache==0x202000);++calls;return mobile;}
int main(int argc,char **argv){
    assert(argc==3);editor=(uint32_t)strtoul(argv[1],NULL,0);mobile=atoi(argv[2]);
    uint32_t _cf=0,_flag_k=FK_NONE,_flag_a=0,_flag_b=0;
    /* COMPARE */
    uint32_t before[]={_cf,_flag_k,_flag_a,_flag_b};
    /* GATE */
    unsigned target=0x10D7A851u;goto done;
L_10D7A8A6: target=0x10D7A8A6u;
done:
    assert(_cf==before[0]&&_flag_k==before[1]&&_flag_a==before[2]&&_flag_b==before[3]);
    printf("{\"target\":%u,\"editor\":%u,\"calls\":%d,\"registers\":[",target,editor,calls);
    for(unsigned i=0;i<8;++i)printf("%s%u",i?",":"",registers[i]);
    puts("]}");
}
'''.replace("/* INITIAL */", initial_text).replace("/* COMPARE */", "\n".join(emission["compare"]))
        program = program.replace("/* GATE */", "\n".join(emission["gate"]))
        env, linker = environment("x64")
        with tempfile.TemporaryDirectory(prefix="mobile-pcs-gate-", dir=ROOT / "build") as temporary:
            folder = Path(temporary)
            probe, obj, binary = folder / "check.c", folder / "check.obj", folder / "check.exe"
            probe.write_text(program, encoding="utf-8")
            for command in ([str(CLANG), "/nologo", "/O2", "/W4", "/WX", "/std:c11", "/clang:--target=x86_64-pc-windows-msvc", "/c", str(probe), "/Fo" + str(obj)],
                            [str(linker), "/nologo", str(obj), "/OUT:" + str(binary)]):
                result = subprocess.run(command, env=env, capture_output=True, text=True, encoding="utf-8", errors="replace", timeout=30)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            for editor in (0, 1, 0x80000000, 0xFFFFFFFF):
                for mobile in (0, 1):
                    with self.subTest(editor=editor, mobile=mobile):
                        observed = json.loads(subprocess.check_output([str(binary), hex(editor), str(mobile)], text=True, encoding="utf-8", timeout=10))
                        uc = Uc(UC_ARCH_X86, UC_MODE_32)
                        base = pe.OPTIONAL_HEADER.ImageBase
                        uc.mem_map(base, pe.OPTIONAL_HEADER.SizeOfImage)
                        uc.mem_write(base, pe.get_memory_mapped_image())
                        uc.mem_map(0x100000, 0x10000)
                        uc.mem_write(0x111C0348, struct.pack("<I", editor))
                        for register, value in zip(REGISTERS, INITIAL):
                            uc.reg_write(register, value)
                        uc.reg_write(UC_X86_REG_EFLAGS, 0x202)
                        before_branch = []

                        def boundary(machine, address, size, unused):
                            if address == 0x10D7A84F:
                                before_branch.append(machine.reg_read(UC_X86_REG_EFLAGS))
                                if mobile:
                                    machine.reg_write(UC_X86_REG_EIP, 0x10D7A851)
                            if address in (0x10D7A851, 0x10D7A8A6):
                                machine.emu_stop()

                        uc.hook_add(UC_HOOK_CODE, boundary)
                        uc.emu_start(0x10D7A848, 0, count=8)
                        self.assertEqual(len(before_branch), 1)
                        self.assertEqual(uc.reg_read(UC_X86_REG_EFLAGS), before_branch[0])
                        self.assertEqual(observed["target"], uc.reg_read(UC_X86_REG_EIP))
                        self.assertEqual(observed["registers"], [uc.reg_read(register) for register in REGISTERS])
                        self.assertEqual(observed["registers"], list(INITIAL))
                        self.assertEqual(observed["editor"], editor)
                        self.assertEqual(bytes(uc.mem_read(0x111C0348, 4)), struct.pack("<I", editor))
                        self.assertEqual(observed["calls"], int(editor == 0))

    def test_original_save_zero_serializes_key_hash_timestamp_and_bytecode(self):
        pe = original()
        base = pe.OPTIONAL_HEADER.ImageBase
        uc = Uc(UC_ARCH_X86, UC_MODE_32)
        uc.mem_map(base, pe.OPTIONAL_HEADER.SizeOfImage)
        uc.mem_write(base, pe.get_memory_mapped_image())
        uc.mem_map(0, 0x1000)
        uc.mem_map(0x100000, 0x10000)
        uc.mem_map(0x200000, 0x400000)
        uc.mem_map(0x50000000, 0x10000)
        cache, table = 0x200000, 0x200100
        manager, manager_vt, archive, archive_vt = 0x206000, 0x206100, 0x207000, 0x207100
        allocator, allocator_vt = 0x208000, 0x208100
        create, serialize, count, destroy = 0x50001000, 0x50002000, 0x50003000, 0x50004000
        alloc, realloc, free, copy, finish = 0x50005000, 0x50006000, 0x50007000, 0x50008000, 0x5000F000

        def word(address, value):
            uc.mem_write(address, struct.pack("<I", value))

        def wide(address, text):
            raw = (text + "\0").encode("utf-16-le")
            uc.mem_write(address, raw)
            return len(raw) // 2

        filename = "ShaderCache.pcs"
        rows = [("Base.hlslPSMain", bytes(range(16)), 0x12345678, struct.pack("<2I", 0xFFFF0300, 0xFFFF)),
                ("LongShader_" + "A" * 70, bytes(range(16, 32)), 0x87654321, struct.pack("<3I", 0xFFFE0300, 0, 0xFFFF))]
        word(cache, table)
        word(cache + 4, len(rows))
        word(cache + 8, len(rows))
        word(cache + 0x14, 0x201000)
        word(cache + 0x18, wide(0x201000, filename))
        word(cache + 0x1C, len(filename) + 1)
        word(cache + 0x20, 0)  # Original archive byte order: native little endian.
        word(cache + 0x24, 1)
        for index, (name, key, timestamp, code) in enumerate(rows):
            record = table + 36 * index
            name_at, value_at, code_at = 0x202000 + 0x1000 * index, 0x204000 + 0x20 * index, 0x205000 + 0x100 * index
            units = wide(name_at, name)
            uc.mem_write(record + 4, struct.pack("<3I", name_at, units, units))
            uc.mem_write(record + 16, key)
            word(record + 32, value_at)
            uc.mem_write(value_at, struct.pack("<3I", len(code), timestamp, code_at))
            uc.mem_write(code_at, code)
        word(0x11129750, manager)
        word(manager, manager_vt)
        word(manager_vt + 8, create)
        word(archive, archive_vt)
        word(archive + 4, 2226)
        word(archive + 0x14, 1)  # Saving; the original FString serializer reads these flags.
        word(archive_vt, destroy)
        word(archive_vt + 0x14, serialize)
        word(archive_vt + 0x24, count)
        word(0x111BE304, allocator)
        word(allocator, allocator_vt)
        for slot, target in ((4, alloc), (8, realloc), (12, free)):
            word(allocator_vt + slot, target)
        imports = {symbol.address: symbol.name for dll in pe.DIRECTORY_ENTRY_IMPORT for symbol in dll.imports}
        self.assertEqual(imports[0x10FCC4FC], b"memcpy")
        word(0x10FCC4FC, copy)
        stream, calls, blocks = bytearray(), [], {}
        fail_create = False
        bump = 0x210000

        def memory_allocate(size):
            nonlocal bump
            address = bump
            bump = (bump + max(size, 1) + 15) & ~15
            self.assertLess(bump, 0x600000)
            blocks[address] = size
            return address

        def boundary(machine, address, size, unused):
            if address not in (create, serialize, count, destroy, alloc, realloc, free, copy):
                return
            sp = machine.reg_read(UC_X86_REG_ESP)
            resume, a, b, c = struct.unpack("<4I", machine.mem_read(sp, 16))
            if address == create:
                self.assertEqual(bytes(machine.mem_read(a, 2 * (len(filename) + 1))).decode("utf-16-le"), filename + "\0")
                self.assertEqual(b, 0)
                calls.append("create")
                result, arguments = 0 if fail_create else archive, 3
            elif address == serialize:
                stream.extend(bytes(machine.mem_read(a, b)))
                result, arguments = archive, 2
            elif address == count:
                result, arguments = 0, 2
            elif address == destroy:
                calls.append("close")
                result, arguments = 0, 1
            elif address == alloc:
                result, arguments = memory_allocate(a), 2
            elif address == realloc:
                result = memory_allocate(b) if b else 0
                if a and result:
                    machine.mem_write(result, bytes(machine.mem_read(a, min(blocks.get(a, b), b))))
                blocks.pop(a, None)
                arguments = 3
            elif address == free:
                blocks.pop(a, None)
                result, arguments = 0, 1
            else:
                machine.mem_write(a, bytes(machine.mem_read(b, c)))
                result, arguments = a, 0  # CRT memcpy is cdecl.
            machine.reg_write(UC_X86_REG_EAX, result)
            machine.reg_write(UC_X86_REG_ESP, sp + 4 + 4 * arguments)
            machine.reg_write(UC_X86_REG_EIP, resume)

        for register, value in zip(REGISTERS, INITIAL):
            uc.reg_write(register, value)
        uc.reg_write(UC_X86_REG_ECX, cache)
        uc.mem_write(INITIAL[4], struct.pack("<2I", finish, 0))
        uc.hook_add(UC_HOOK_CODE, boundary)
        # File/archive/heap/CRT memory boundaries only: Save, FString copy,
        # compact-index encoding and original serialization instructions run.
        uc.emu_start(0x10AF7060, finish, count=50000)
        self.assertEqual(uc.reg_read(UC_X86_REG_EIP), finish)
        self.assertEqual(uc.reg_read(UC_X86_REG_ESP), INITIAL[4] + 8)
        self.assertEqual(calls, ["create", "close"])

        def compact(value):
            first, value = value & 63, value >> 6
            out = bytearray([first | (64 if value else 0)])
            while value:
                part, value = value & 127, value >> 7
                out.append(part | (128 if value else 0))
            return out

        expected = bytearray(struct.pack("<I", 13))
        for name, key, timestamp, code in rows:
            expected.extend(compact(len(name) + 1))
            expected.extend((name + "\0").encode("utf-16-le"))
            expected.extend(key)
            expected.extend(struct.pack("<II", len(code), timestamp))
            expected.extend(code)
        self.assertEqual(stream, expected)
        for index in (3, 5, 6, 7):  # Callee-saved x86 registers.
            self.assertEqual(uc.reg_read(REGISTERS[index]), INITIAL[index])
        self.assertEqual(struct.unpack("<I", uc.mem_read(cache + 0x24, 4))[0], 1)
        # Save's original return value is deliberately not a success certificate.
        fail_create = True
        stream.clear()
        calls.clear()
        for register, value in zip(REGISTERS, INITIAL):
            uc.reg_write(register, value)
        uc.reg_write(UC_X86_REG_ECX, cache)
        uc.mem_write(INITIAL[4], struct.pack("<2I", finish, 0))
        uc.emu_start(0x10AF7060, finish, count=50000)
        self.assertEqual(uc.reg_read(UC_X86_REG_EAX), 1)
        self.assertEqual(stream, b"")
        self.assertEqual(calls, ["create"])


if __name__ == "__main__":
    unittest.main()

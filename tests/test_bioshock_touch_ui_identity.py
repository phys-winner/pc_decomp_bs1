"""Keep ScriptText UI on its PC route without disconnecting touch movement."""
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
from unicorn import Uc, UC_ARCH_X86, UC_MODE_32
from unicorn.x86_const import (UC_X86_REG_EAX, UC_X86_REG_ECX, UC_X86_REG_EDX,
                              UC_X86_REG_EBX, UC_X86_REG_ESP, UC_X86_REG_EBP,
                              UC_X86_REG_ESI, UC_X86_REG_EDI, UC_X86_REG_EFLAGS)

from tests.host_environment import CLANG, environment

ROOT = Path(__file__).resolve().parents[1]
REGISTERS = (UC_X86_REG_EAX, UC_X86_REG_ECX, UC_X86_REG_EDX, UC_X86_REG_EBX,
             UC_X86_REG_ESP, UC_X86_REG_EBP, UC_X86_REG_ESI, UC_X86_REG_EDI,
             UC_X86_REG_EFLAGS)
ENGINE, CLIENT, VTABLE, FRAME, RESULT = (0x200000 + i * 0x1000 for i in range(5))
STACK, STOP, STORE = 0x108000, 0x50000000, 0x10A7354E


def original():
    inputs = json.loads((ROOT / "config/inputs.json").read_text(encoding="utf-8"))
    raw = (Path(inputs["source_root"]) / "Builds/Release/Bioshock.exe").read_bytes()
    assert hashlib.sha256(raw).hexdigest() == "7f2ee821ad60eaf08322b4bc6ebbf839d7fe010b4d0255ec12b5be4a2966f7f6"
    return pefile.PE(data=raw)


def checked_emission():
    # Keep the vendor's separate tools package out of the unittest interpreter.
    sys.path.insert(0, str(ROOT / "tools"))
    from lift_game import CheckedLifter
    from disasm.disasm32 import Instruction

    pe = original()
    decoder = Cs(CS_ARCH_X86, CS_MODE_32)
    decoder.detail = True

    def instruction(code, address):
        decoded = next(decoder.disasm(code, address))
        return Instruction(decoded.address, len(code), decoded.mnemonic, decoded.op_str, code, decoded.operands)

    code = pe.get_data(STORE - pe.OPTIONAL_HEADER.ImageBase, 2)
    assert code.hex() == "8901"
    lifter = CheckedLifter()
    state, sequence = lifter._flag_state, lifter._flag_seq
    lines = lifter.lift_instruction(instruction(code, STORE))
    assert (lifter._flag_state, lifter._flag_seq) == (state, sequence)
    assert len(lines) == 1 and "MEM32(ecx) = eax && void_input_touch_pc_mouse() ? 0u : eax;" in lines[0]
    assert "RECOMP_FLAGS" not in lines[0] and "eax =" not in lines[0]
    source = (ROOT / "generated/bioshock/functions_0078.c").read_text(encoding="utf-8")
    assert source.count("    " + lines[0] + "\n") == 1
    assert source.count("void_input_touch_pc_mouse") == 2
    for changed in ("8902", "8903", "890190"):
        try:
            CheckedLifter().lift_instruction(instruction(bytes.fromhex(changed), STORE))
            raise AssertionError("Changed controller identity store accepted")
        except ValueError as error:
            assert "identity result changed" in str(error)
    assert "void_input_touch_pc_mouse" not in "\n".join(
        CheckedLifter().lift_instruction(instruction(code, STORE + 1)))
    return lines[0]


def inspect_emission():
    result = subprocess.run([sys.executable, "-B", "-c",
                             "import json; from tests.test_bioshock_touch_ui_identity import checked_emission; "
                             "print(json.dumps(checked_emission()))"], cwd=ROOT, capture_output=True,
                            text=True, encoding="utf-8", errors="replace", timeout=30)
    if result.returncode:
        raise AssertionError(result.stdout + result.stderr)
    return json.loads(result.stdout)


class BioshockTouchUiIdentityTest(unittest.TestCase):
    def test_checked_lifter_and_canonical_single_store(self):
        self.assertIn("touch retains PC ScriptText UI", inspect_emission())

    def test_original_controller_query_and_emitted_result_preserve_original_state(self):
        emission = inspect_emission()
        pe = original()
        base = pe.OPTIONAL_HEADER.ImageBase
        # This method is also used natively; its production bytes remain untouched.
        self.assertEqual(pe.get_data(0x10D9D420 - base, 7).hex(), "8b81d4000000c3")
        program = r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static uint32_t registers[9], output=0xa5a5a5a5;
static int enabled,calls;
#define eax registers[0]
#define ecx registers[1]
#define MEM32(address) (*(assert((address)==0x204000u), &output))
int void_input_touch_pc_mouse(void){++calls;return enabled;}
int main(int argc,char **argv){
    assert(argc==11);enabled=atoi(argv[1]);
    for(unsigned i=0;i<9;++i)registers[i]=(uint32_t)strtoul(argv[i+2],NULL,0);
    uint32_t before[9];memcpy(before,registers,sizeof before);
    /* STORE */
    assert(!memcmp(before,registers,sizeof before));
    printf("{\"output\":%u,\"calls\":%d,\"registers\":[",output,calls);
    for(unsigned i=0;i<9;++i)printf("%s%u",i?",":"",registers[i]);
    puts("]}");return 0;
}
'''.replace("/* STORE */", emission)
        env, linker = environment("x64")
        with tempfile.TemporaryDirectory(prefix="bioshock-ui-identity-") as temporary:
            out = Path(temporary)
            source, obj, binary = out / "check.c", out / "check.obj", out / "check.exe"
            source.write_text(program, encoding="utf-8")
            for command in ([str(CLANG), "/nologo", "/O2", "/W4", "/WX", "/std:c11",
                             "/clang:--target=x86_64-pc-windows-msvc", "/c", str(source), "/Fo" + str(obj)],
                            [str(linker), "/nologo", str(obj), "/OUT:" + str(binary)]):
                result = subprocess.run(command, env=env, capture_output=True, text=True,
                                        encoding="utf-8", errors="replace", timeout=30)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            for controller in (0, 1, 0x80000000, 0xFFFFFFFF):
                for cursor in (0, 0x7FFFFFFF, 0xFFFFFFFF):
                    uc = Uc(UC_ARCH_X86, UC_MODE_32)
                    uc.mem_map(base, pe.OPTIONAL_HEADER.SizeOfImage)
                    uc.mem_write(base, pe.get_memory_mapped_image())
                    for address, size in ((0x100000, 0x10000), (ENGINE, 0x5000), (STOP, 0x1000)):
                        uc.mem_map(address, size)

                    def word(address, value):
                        uc.mem_write(address, struct.pack("<I", value))

                    word(0x111BE314, ENGINE)
                    word(ENGINE + 0x44, CLIENT)
                    word(CLIENT, VTABLE)
                    word(CLIENT + 212, controller)
                    word(VTABLE + 0x118, 0x10D9D420)
                    word(FRAME + 12, cursor)
                    word(RESULT, 0xA5A5A5A5)
                    uc.mem_write(STACK, struct.pack("<3I", STOP, FRAME, RESULT))
                    initial = (0x12345678, 0x23456789, 0x3456789A, 0x456789AB,
                               STACK, 0x56789ABC, 0x6789ABCD, 0x789ABCDE, 0xA97)
                    for register, value in zip(REGISTERS, initial):
                        uc.reg_write(register, value)
                    uc.emu_start(0x10A73530, STORE, count=100)
                    state = [uc.reg_read(register) for register in REGISTERS]
                    self.assertEqual(state[0], controller)
                    self.assertEqual(state[1], RESULT)
                    self.assertEqual(struct.unpack("<I", uc.mem_read(FRAME + 12, 4))[0], (cursor + 1) & 0xFFFFFFFF)
                    for enabled in (0, 1):
                        observed = json.loads(subprocess.check_output(
                            [str(binary), str(enabled), *map(str, state)], text=True, encoding="utf-8", timeout=10))
                        self.assertEqual(observed["registers"], state)
                        self.assertEqual(observed["output"], 0 if enabled else controller)
                        self.assertEqual(observed["calls"], int(controller != 0))
                    uc.emu_start(STORE, STOP, count=10)
                    self.assertEqual(struct.unpack("<I", uc.mem_read(RESULT, 4))[0], controller)
                    self.assertEqual(uc.reg_read(UC_X86_REG_ESP), STACK + 12)
                    for index, register in enumerate(REGISTERS):
                        if register != UC_X86_REG_ESP:
                            self.assertEqual(uc.reg_read(register), state[index])
                    self.assertEqual(struct.unpack("<I", uc.mem_read(CLIENT + 212, 4))[0], controller)
        print("PASS original execGetUseXBoxController + emitted C: 12 original states/24 virtual-touch cases; "
              "PC script result only, original controller field/GPR/EFLAGS/frame cursor preserved")


if __name__ == "__main__":
    unittest.main()

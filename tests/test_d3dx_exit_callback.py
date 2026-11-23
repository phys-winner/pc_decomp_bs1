"""Compare all three observed D3DX exit callbacks with their original x86 instructions."""
import hashlib
import json
from pathlib import Path
import struct
import subprocess
import tempfile
import unittest

import pefile
from unicorn import Uc, UC_ARCH_X86, UC_MODE_32, UC_HOOK_CODE
from unicorn.x86_const import (UC_X86_REG_EAX, UC_X86_REG_EBX, UC_X86_REG_ECX,
                              UC_X86_REG_EDX, UC_X86_REG_ESI, UC_X86_REG_EDI,
                              UC_X86_REG_EBP, UC_X86_REG_ESP, UC_X86_REG_EIP)

from tests.host_environment import CLANG, environment
from tests.test_mobile_shaders import function

ROOT = Path(__file__).resolve().parents[1]
BASE = 0x20000000
CALLBACKS = (
    (0x20320A15, 0x20356BDC, "b9dc6b3520e9cda1ffff", "supplemental_0005_0000.c"),
    (0x203209F5, 0x2033EA20, "b920ea3320e91255d4ff", "supplemental_0006_0000.c"),
    (0x20320A04, 0x20352EBC, "68bc2e3520ff15c4100020c3", "supplemental_0006_0000.c"),
)
FREE, DELETE, RETURN = 0x40001000, 0x40002000, 0x40003000
REGISTERS = (UC_X86_REG_EAX, UC_X86_REG_ECX, UC_X86_REG_EDX, UC_X86_REG_EBX,
             UC_X86_REG_ESP, UC_X86_REG_EBP, UC_X86_REG_ESI, UC_X86_REG_EDI)
INITIAL = (0x11111111, 0x22222222, 0x33333333, 0x44444444,
           0x101000, 0x55555555, 0x66666666, 0x77777777)


class D3dxExitCallbackTest(unittest.TestCase):
    def test_original_exit_callback_and_dispatch(self):
        inputs = json.loads((ROOT / "config/inputs.json").read_text(encoding="utf-8"))
        original = Path(inputs["source_root"]) / "Builds/Release/d3dx9_33.dll"
        raw = original.read_bytes()
        self.assertEqual(hashlib.sha256(raw).hexdigest(),
                         "c6b7b2ad7742dde5dd8d1a35fdc1c185e586e551ad9c74d3fb21759cd8ca4da8")
        rebased = pefile.PE(str(ROOT / "generated/input/d3dx9_33.dll"))
        source = pefile.PE(data=raw)
        source.relocate_image(BASE)
        self.assertEqual(rebased.OPTIONAL_HEADER.ImageBase, BASE)
        self.assertEqual(rebased.get_data(0x65F11, 18), bytes.fromhex("8bff568bf1ff36ff152c1100208326005ec3"))
        for entry, obj, preimage, translation in CALLBACKS:
            self.assertEqual(rebased.get_data(entry - BASE, len(bytes.fromhex(preimage))), bytes.fromhex(preimage))
        for address, size in ([(entry, len(bytes.fromhex(preimage))) for entry, obj, preimage, translation in CALLBACKS] +
                              [(0x2031ABEC, 30), (0x20065F11, 18)]):
            self.assertEqual(rebased.get_data(address - BASE, size), source.get_data(address - BASE, size))
            self.assertTrue(rebased.get_section_by_rva(address - BASE).Characteristics & 0x20000000)
        report = json.loads((ROOT / "generated/d3dx9_33/report.json").read_text(encoding="utf-8"))
        dispatch = (ROOT / "generated/dispatch.c").read_text(encoding="utf-8")
        for entry, obj, preimage, translation in CALLBACKS:
            self.assertIn(hex(entry), report["dispatch_entries"])
            self.assertIn(f"{{0x{entry:08X}u, sub_{entry:08X}}}", dispatch)
        self.assertEqual({entry for entry, obj, preimage, translation in CALLBACKS}, {0x203209F5, 0x20320A04, 0x20320A15})
        production = function((ROOT / "generated/d3dx9_33/functions_0000.c").read_text(encoding="utf-8"), "void sub_20065F11(")
        for entry, obj, preimage, translation in CALLBACKS:
            production += "\n" + function((ROOT / "generated/d3dx9_33" / translation).read_text(encoding="utf-8"), f"void sub_{entry:08X}(")
        program = r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static uint8_t image[0x370000],stack[0x2000];
static uint32_t eax,ecx,edx,ebx,esp,ebp,esi,edi;
static uint32_t entry,object,handle;
static uint32_t calls[2][2];static unsigned count;
static void *guest(uint32_t address) {
    if(address>=0x20000000 && address<=0x20370000-4)return image+(address-0x20000000);
    assert(address>=0x100000 && address<=0x102000-4);return stack+(address-0x100000);
}
#define MEM32(address) (*(uint32_t *)guest((uint32_t)(address)))
#define PUSH32(s,value) do{(s)-=4;MEM32(s)=(uint32_t)(value);}while(0)
static uint32_t pop(uint32_t *sp) { uint32_t value=MEM32(*sp);*sp+=4;return value; }
#define POP32_VAL(s) pop(&(s))
#define FK_NONE 0
#define FK_TEST 1
#define FK_ADD 2
#define TEST_Z(a,b) (((a)&(b))==0)
#define RECOMP_REGS_LOCALS
#define RECOMP_ENTER(va) assert((va)==entry || (va)==0x20065F11)
#define RECOMP_BACKEDGE() ((void)0)
#define RECOMP_REGS_OUT() ((void)0)
#define RECOMP_FLAGS_IN() ((void)0)
#define RECOMP_FLAGS_OUT() ((void)0)
static void original_import(uint32_t target) {
    assert(count<2);uint32_t arg=MEM32(esp+4);calls[count][0]=target;calls[count++][1]=arg;
    if(target==0x40001000){assert(entry!=0x20320A04 && arg==handle);if(entry==0x20320A15)assert(arg);eax=arg?1u:0u;ecx=0xA1A1A1A1;edx=0xA2A2A2A2;}
    else {assert(target==0x40002000 && entry!=0x203209F5 && arg==object+(entry==0x20320A15?4u:0u));MEM32(arg)=0;eax=0xD0D0D0D0;ecx=0xD1D1D1D1;edx=0xD2D2D2D2;}
    assert(MEM32(esp)==0xDEAD0000);esp+=8;
}
#define RECOMP_ICALL(target) do{uint32_t va=(target);PUSH32(esp,0xDEAD0000);original_import(va);}while(0)
#define RECOMP_ITAIL(target) do{assert((target)==0x20065F11);sub_20065F11();}while(0)
/* PRODUCTION */
int main(int argc,char **argv) {
    assert(argc==3);entry=(uint32_t)strtoul(argv[1],NULL,0);handle=(uint32_t)strtoul(argv[2],NULL,0);
    assert(entry==0x20320A15 || entry==0x203209F5 || entry==0x20320A04);
    object=entry==0x20320A15?0x20356BDC:entry==0x203209F5?0x2033EA20:0x20352EBC;
    MEM32(0x2000112C)=0x40001000;MEM32(0x200010C4)=0x40002000;
    MEM32(object)=entry==0x20320A04?0x12345678:handle;MEM32(object+4)=0x12345678;
    eax=0x11111111;ecx=0x22222222;edx=0x33333333;ebx=0x44444444;esp=0x101000;
    ebp=0x55555555;esi=0x66666666;edi=0x77777777;MEM32(esp)=0x40003000;
    if(entry==0x20320A15)sub_20320A15();else if(entry==0x203209F5)sub_203209F5();else sub_20320A04();
    assert(esp==0x101004 && ebx==0x44444444 && ebp==0x55555555 && esi==0x66666666 && edi==0x77777777);
    assert(count==(entry==0x20320A15 && handle?2u:1u) && MEM32(0x101000)==0x40003000);
    if(entry==0x20320A15)assert(MEM32(object)==handle && !MEM32(object+4));
    else assert(!MEM32(object) && MEM32(object+4)==0x12345678);
    printf("{\"registers\":[%u,%u,%u,%u,%u,%u,%u,%u],\"calls\":[",eax,ecx,edx,ebx,esp,ebp,esi,edi);
    for(unsigned i=0;i<count;++i)printf("%s[%u,%u]",i?",":"",calls[i][0],calls[i][1]);
    printf("],\"object\":[%u,%u]}\n",MEM32(object),MEM32(object+4));return 0;
}
'''
        env, linker = environment("x64")
        with tempfile.TemporaryDirectory(prefix="bioshock-d3dx-exit-") as temporary:
            folder = Path(temporary)
            c, obj, binary = folder / "probe.c", folder / "probe.obj", folder / "probe.exe"
            c.write_text(program.replace("/* PRODUCTION */", production), encoding="utf-8")
            subprocess.run([str(CLANG), "/nologo", "/O2", "/std:c11", "/clang:--target=x86_64-pc-windows-msvc",
                            "/c", str(c), "/Fo" + str(obj)], env=env, check=True)
            subprocess.run([str(linker), "/nologo", str(obj), "/OUT:" + str(binary)], env=env, check=True)
            cases = [(entry, obj, handle) for entry, obj, preimage, translation in CALLBACKS
                     for handle in ((0,) if entry == 0x20320A04 else (0, 0x42424242))]
            for entry, obj, handle in cases:
                observed = json.loads(subprocess.check_output([str(binary), hex(entry), hex(handle)], text=True, encoding="utf-8", timeout=30))
                uc = Uc(UC_ARCH_X86, UC_MODE_32)
                uc.mem_map(BASE, 0x370000)
                uc.mem_write(BASE, rebased.get_memory_mapped_image())
                uc.mem_map(0x100000, 0x2000)
                uc.mem_map(0x40000000, 0x4000)
                uc.mem_write(0x2000112C, struct.pack("<I", FREE))
                uc.mem_write(0x200010C4, struct.pack("<I", DELETE))
                uc.mem_write(obj, struct.pack("<II", 0x12345678 if entry == 0x20320A04 else handle, 0x12345678))
                for reg, value in zip(REGISTERS, INITIAL):
                    uc.reg_write(reg, value)
                uc.mem_write(INITIAL[4], struct.pack("<I", RETURN))
                uc.mem_write(FREE, b"\xB8" + struct.pack("<I", bool(handle)) + bytes.fromhex("b9a1a1a1a1baa2a2a2a2c20400"))
                uc.mem_write(DELETE, bytes.fromhex("b8d0d0d0d0b9d1d1d1d1bad2d2d2d2c20400"))
                calls = []

                def imports(machine, address, size, data):
                    if address not in (FREE, DELETE):
                        return
                    arg = struct.unpack("<I", machine.mem_read(machine.reg_read(UC_X86_REG_ESP) + 4, 4))[0]
                    calls.append([address, arg])
                    if address == DELETE:
                        machine.mem_write(arg, b"\0" * 4)

                uc.hook_add(UC_HOOK_CODE, imports)
                uc.emu_start(entry, RETURN, count=100)
                self.assertEqual(uc.reg_read(UC_X86_REG_EIP), RETURN)
                oracle = {"registers": [uc.reg_read(reg) for reg in REGISTERS], "calls": calls,
                          "object": list(struct.unpack("<II", uc.mem_read(obj, 8)))}
                self.assertEqual(observed, oracle, (hex(entry), hex(handle)))
        for translation in sorted({row[3] for row in CALLBACKS}):
            subprocess.run([str(CLANG.with_name("clang.exe")), "--target=aarch64-linux-android29", "-std=gnu11",
                            "-w", "-fno-strict-aliasing", "-fwrapv", "-DRECOMP_X87_WINDOW", "-DRECOMP_GUEST_STATE_SEPARATE",
                            "-Iandroid/runtime", "-Iandroid/host", "-Igenerated", "-fsyntax-only",
                            str(ROOT / "generated/d3dx9_33" / translation)], cwd=ROOT, check=True)
        print("PASS all 3 observed D3DX exit callbacks: 5 actual generated C vs original x86 oracle cases, null/live handle, exact FreeLibrary/DeleteCriticalSection and object reset, return/ESP/callee-saved registers, pinned bytes/dispatch and both ARM64 TUs")


if __name__ == "__main__":
    unittest.main()

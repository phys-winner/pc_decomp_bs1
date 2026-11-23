"""Oracle the checked CRT FNSAVE/FRSTOR boundary against original x86 opcodes."""
import hashlib
import json
import math
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest

import pefile
from capstone import Cs, CS_ARCH_X86, CS_MODE_32
from unicorn import Uc, UC_ARCH_X86, UC_MODE_32
from unicorn.x86_const import (UC_X86_REG_EAX, UC_X86_REG_EFLAGS, UC_X86_REG_FPCW,
                              UC_X86_REG_FPSW, UC_X86_REG_FPTAG, UC_X86_REG_FP0)
from tests.host_environment import CLANG, environment
from tests.test_mobile_shaders import function

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
from lift_game import CheckedLifter


class FpuStateTest(unittest.TestCase):
    def test_original_opcodes_and_lifter(self):
        spec = next(row for row in json.loads((ROOT / "config/modules.json").read_text())["modules"] if row["name"] == "msvcr80")
        raw = Path(spec["source"]).read_bytes()
        self.assertEqual(hashlib.sha256(raw).hexdigest(), spec["sha256"])
        pe = pefile.PE(data=raw, fast_load=True)
        decoder = Cs(CS_ARCH_X86, CS_MODE_32)
        decoder.detail = True
        for address, opcode, restore in ((0x1005469D, "dd7108", 0), (0x100546A9, "dd6108", 1)):
            self.assertEqual(pe.get_data(address - 0x10000000, 3), bytes.fromhex(opcode))
            text = "\n".join(CheckedLifter().lift_instruction(next(decoder.disasm(bytes.fromhex(opcode), address))))
            self.assertIn(f"void_fpu_state((uint32_t)(ecx + 0x8), 0x{address:08X}u, {restore}, 0)", text)
            self.assertNotIn("void_lift_trap", text)
        for opcode in ("66dd30", "66dd20"):
            text = "\n".join(CheckedLifter().lift_instruction(next(decoder.disasm(bytes.fromhex(opcode), 0x10000))))
            self.assertIn(", 1);", text)
        text = "\n".join(CheckedLifter().lift_instruction(next(decoder.disasm(bytes.fromhex("67dd30"), 0x10000))))
        self.assertIn("void_lift_trap", text)

    def test_production_images_reset_restore_and_boundaries(self):
        header = (ROOT / "android/runtime/recomp_types.h").read_text(encoding="utf-8")
        helpers = function(header, "static inline double fp_ld80(") + "\n" + function(header, "static inline void fp_st80(")
        production = (ROOT / "android/host/fpu_state.c").read_text(encoding="utf-8").replace('#include "host.h"', "")
        program = r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <setjmp.h>
#include <io.h>
#include <fcntl.h>
static uint8_t memory[4096];
static double g_st[8];static int64_t g_st_i64[8];
static int g_fp_top,void_fpu_cmp;
static uint16_t g_fpu_cw;
static uint64_t mask;
static jmp_buf trap;
#define GPTR(a) (memory+(a))
static int void_vm_readable(uint32_t address,uint32_t bytes){return address>=128 && (uint64_t)address+bytes<=sizeof memory;}
static int void_vm_writable(uint32_t address,uint32_t bytes){return void_vm_readable(address,bytes);}
static void void_lift_trap(uint32_t address,const char *text){(void)address;(void)text;longjmp(trap,1);}
static void fp_cw_masks(uint16_t cw){mask=cw;}
/* HELPERS */
/* PRODUCTION */
int main(int argc,char **argv){
    assert(argc==3);int depth=atoi(argv[1]),small=atoi(argv[2]);
    double values[]={1.25,-0.0,-3.5,INFINITY,-INFINITY,NAN,9007199254740992.0,0x1p-1074};
    for(int i=0;i<depth;++i)g_st[i]=values[i];
    if(depth>6)g_st_i64[6]=9007199254740993ll;
    g_fp_top=depth;g_fpu_cw=0x0B7F;void_fpu_cmp=0x14500;mask=0xDEADBEEF;
    double before[8];int64_t shadows[8];memcpy(before,g_st,sizeof before);memcpy(shadows,g_st_i64,sizeof shadows);
    memset(memory,0xA5,sizeof memory);
    void_fpu_state(256,0x1005469D,0,small);
    unsigned size=small?94:108;
    assert(g_fp_top==0 && g_fpu_cw==0x037F && void_fpu_cmp==0x10000 && mask==0x037F);
    for(unsigned i=0;i<8;++i){unsigned physical=((0u-(unsigned)depth)+i)&7u;assert(!memcmp(&g_st[physical],&before[i],8));assert(g_st_i64[physical]==shadows[i]);}
    assert(memory[255]==0xA5 && memory[256+size]==0xA5);
    _setmode(_fileno(stdout),_O_BINARY);fwrite(memory+256,1,size,stdout);
    void_fpu_state(256,0x100546A9,1,small);
    assert(g_fp_top==depth && g_fpu_cw==0x0B7F && mask==0x0B7F);
    assert((void_fpu_cmp&0x4700)==0x4500);
    for(int i=0;i<depth;++i){assert(isnan(before[i])?isnan(g_st[i]):!memcmp(&g_st[i],&before[i],8));if(shadows[i])assert(g_st_i64[i]==shadows[i]);}
    uint8_t snapshot[4096];memcpy(snapshot,memory,sizeof memory);int old_depth=g_fp_top;
    if(!setjmp(trap)){void_fpu_state(4096-size+1,0x1234,0,small);assert(0);}
    assert(g_fp_top==old_depth && !memcmp(snapshot,memory,sizeof memory));
    if(!setjmp(trap)){void_fpu_state(4096-size+1,0x1234,1,small);assert(0);}
    assert(g_fp_top==old_depth && !memcmp(snapshot,memory,sizeof memory));
    return 0;
}
'''
        with tempfile.TemporaryDirectory(prefix="bioshock-fpu-state-", dir=ROOT / "build") as temporary:
            folder = Path(temporary)
            probe, obj, binary = folder / "probe.c", folder / "probe.obj", folder / "probe.exe"
            probe.write_text(program.replace("/* HELPERS */", helpers).replace("/* PRODUCTION */", production), encoding="utf-8")
            env, linker = environment("x64")
            for command in ([str(CLANG), "/nologo", "/O2", "/W4", "/std:c11", "/clang:--target=x86_64-pc-windows-msvc", "/c", str(probe), "/Fo" + str(obj)],
                            [str(linker), "/nologo", str(obj), "/OUT:" + str(binary)]):
                result = subprocess.run(command, env=env, capture_output=True, text=True, timeout=60)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            for small in (0, 1):
                for depth in range(9):
                    result = subprocess.run([str(binary), str(depth), str(small)], capture_output=True, timeout=30)
                    self.assertEqual(result.returncode, 0, result.stderr)
                    image = result.stdout
                    environment_bytes = 14 if small else 28
                    status = 0x4500 | (((-depth) & 7) << 11)
                    tags = 0xFFFF
                    top = (status >> 11) & 7
                    for index in range(depth):
                        value = (1.25, -0.0, -3.5, math.inf, -math.inf, math.nan, 2**53, 2**-1074)[index]
                        tag = 1 if value == 0 else 0 if math.isfinite(value) else 2
                        shift = 2 * ((top + index) & 7)
                        tags = (tags & ~(3 << shift)) | tag << shift
                    uc = Uc(UC_ARCH_X86, UC_MODE_32)
                    uc.mem_map(0x10000, 0x3000)
                    opcode = bytes.fromhex("66dd30" if small else "dd30")
                    uc.mem_write(0x10000, opcode)
                    uc.reg_write(UC_X86_REG_EAX, 0x11000)
                    uc.reg_write(UC_X86_REG_EFLAGS, 0xAD7)
                    for index in range(8):
                        mantissa, exponent = struct.unpack_from("<QH", image, environment_bytes + index * 10)
                        uc.reg_write(UC_X86_REG_FP0 + ((top + index) & 7), (mantissa, exponent))
                    uc.reg_write(UC_X86_REG_FPCW, 0x0B7F)
                    uc.reg_write(UC_X86_REG_FPSW, status)
                    uc.reg_write(UC_X86_REG_FPTAG, tags)
                    uc.emu_start(0x10000, 0x10000 + len(opcode), count=1)
                    oracle = bytes(uc.mem_read(0x11000, len(image)))
                    self.assertEqual(image, oracle, (small, depth, image.hex(), oracle.hex()))
                    self.assertEqual(uc.reg_read(UC_X86_REG_EFLAGS), 0xAD7)
                    self.assertEqual(uc.reg_read(UC_X86_REG_FPCW), 0x037F)
                    self.assertEqual(uc.reg_read(UC_X86_REG_FPSW), 0)
                    self.assertEqual(uc.reg_read(UC_X86_REG_FPTAG), 0xFFFF)
                    restore = bytes.fromhex("66dd20" if small else "dd20")
                    uc.mem_write(0x10020, restore)
                    uc.emu_start(0x10020, 0x10020 + len(restore), count=1)
                    self.assertEqual(uc.reg_read(UC_X86_REG_FPCW), 0x0B7F)
                    self.assertEqual(uc.reg_read(UC_X86_REG_FPSW), status)
                    self.assertEqual(uc.reg_read(UC_X86_REG_FPTAG), tags)
                    self.assertEqual(uc.reg_read(UC_X86_REG_EFLAGS), 0xAD7)
        print("PASS production FNSAVE/FRSTOR: 18 94/108-byte x86 oracle images, all stack depths/classes, exact int64, reset/restore and span guards")


if __name__ == "__main__":
    unittest.main()

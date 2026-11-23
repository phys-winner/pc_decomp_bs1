"""Check every multi-result x87 form present in BioShock's generated CRT."""
import math
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest

from capstone import Cs, CS_ARCH_X86, CS_MODE_32
from unicorn import Uc, UC_ARCH_X86, UC_MODE_32
from unicorn.x86_const import UC_X86_REG_EFLAGS
from tests.host_environment import CLANG, environment
from tests import test_touch_controls as touch_checks

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
from lift_game import CheckedLifter


class X87StackOracleTest(unittest.TestCase):
    def compile_and_run(self, lifted, declarations, loop):
        header = (ROOT / "android/runtime/recomp_types.h").read_text(encoding="utf-8")
        push = touch_checks.TouchControlsTest._method(header, "static inline void fp_push_impl(")
        pop = touch_checks.TouchControlsTest._method(header, "static inline double fp_pop_impl(")
        with tempfile.TemporaryDirectory(prefix="bioshock-x87-oracle-", dir=ROOT / "build") as temporary:
            out = Path(temporary)
            source = out / "check.c"
            source.write_text("#include <assert.h>\n#include <math.h>\n#include <stdio.h>\n#include <stdint.h>\nint64_t g_st_i64[8];\n" + push + pop +
                "\n#define fp_push(v) fp_push_impl(_st,&_fp_top,(v))\n#define fp_pop() fp_pop_impl(_st,&_fp_top)\nint main(void){\n" + declarations + "\n" + loop + "\nreturn 0;}\n")
            env, linker = environment("x64")
            commands = ([str(CLANG), "/nologo", "/O2", "/std:c11", "/clang:--target=x86_64-pc-windows-msvc",
                         "/c", str(source), "/Fo" + str(out / "check.obj")],
                        [str(linker), "/nologo", str(out / "check.obj"), "/OUT:" + str(out / "check.exe")],
                        [str(out / "check.exe")])
            for command in commands:
                result = subprocess.run(command, env=env, capture_output=True, text=True)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        return [tuple(map(float, line.split())) for line in result.stdout.splitlines()]

    @staticmethod
    def lift(opcode):
        decoder = Cs(CS_ARCH_X86, CS_MODE_32)
        decoder.detail = True
        return "\n".join(CheckedLifter().lift_instruction(next(decoder.disasm(bytes.fromhex(opcode), 0x100000))))

    def test_fptan_pushes_one_above_tangent(self):
        lifted = self.lift("d9f2")
        angles = (0.0, 1e-6, -1e-6, .01, -.01, math.pi / 180, -math.pi / 180,
                  math.pi / 4, -math.pi / 4, math.pi, 2 * math.pi, 10.0, -10.0)
        declarations = "double angles[]={" + ",".join(repr(a) for a in angles) + "};"
        loop = "for(unsigned i=0;i<sizeof angles/sizeof *angles;++i){double _st[8]={angles[i],9.125};int _fp_top=1;int _fpu_cmp=0;\n" + lifted + \
            "\nassert(_fp_top==2&&_st[2]==9.125);printf(\"%.17g %.17g\\n\",_st[0],_st[1]);}"
        actual = self.compile_and_run(lifted, declarations, loop)
        for angle, pair in zip(angles, actual, strict=True):
            uc = Uc(UC_ARCH_X86, UC_MODE_32)
            uc.mem_map(0x10000, 4096)
            code = bytes.fromhex("dbe3dd05") + struct.pack("<I", 0x10308) + bytes.fromhex("dd05") + struct.pack("<I", 0x10300) + bytes.fromhex("d9f2")
            for address in (0x10400, 0x10408, 0x10410):
                code += bytes.fromhex("dd1d") + struct.pack("<I", address)
            uc.mem_write(0x10000, code)
            uc.mem_write(0x10300, struct.pack("<dd", angle, 9.125))
            uc.reg_write(UC_X86_REG_EFLAGS, 0x202)
            uc.emu_start(0x10000, 0x10000 + len(code), count=20)
            one, tangent, sentinel = struct.unpack("<ddd", uc.mem_read(0x10400, 24))
            self.assertEqual((one, sentinel), (1.0, 9.125))
            self.assertEqual(uc.reg_read(UC_X86_REG_EFLAGS), 0x202)
            self.assertAlmostEqual(pair[0], one, places=12)
            self.assertAlmostEqual(pair[1], tangent, places=12)
            self.assertAlmostEqual(tangent, math.tan(angle), places=12)

    def test_fpatan_uses_old_st1_over_old_st0_and_pops(self):
        lifted = self.lift("d9f3")
        pairs = ((1.0, 1.0), (1.0, -1.0), (-1.0, 1.0), (-1.0, -1.0),
                 (0.0, 1.0), (0.0, -1.0), (1.0, 0.0), (-1.0, 0.0), (0.0, 0.0))
        declarations = "double pairs[][2]={" + ",".join("{" + repr(x) + "," + repr(y) + "}" for x, y in pairs) + "};"
        loop = "for(unsigned i=0;i<sizeof pairs/sizeof *pairs;++i){double _st[8]={pairs[i][0],pairs[i][1],9.125};int _fp_top=2;int _fpu_cmp=0;\n" + lifted + \
            "\nassert(_fp_top==1&&_st[1]==9.125);printf(\"%.17g\\n\",_st[0]);}"
        actual = self.compile_and_run(lifted, declarations, loop)
        for (x, y), (result,) in zip(pairs, actual, strict=True):
            uc = Uc(UC_ARCH_X86, UC_MODE_32)
            uc.mem_map(0x10000, 4096)
            code = bytes.fromhex("dbe3dd05") + struct.pack("<I", 0x10308) + bytes.fromhex("dd05") + struct.pack("<I", 0x10300)
            code += bytes.fromhex("dd05") + struct.pack("<I", 0x10310) + bytes.fromhex("d9f3")
            for address in (0x10400, 0x10408):
                code += bytes.fromhex("dd1d") + struct.pack("<I", address)
            uc.mem_write(0x10000, code)
            uc.mem_write(0x10300, struct.pack("<ddd", y, 9.125, x))
            uc.reg_write(UC_X86_REG_EFLAGS, 0x202)
            uc.emu_start(0x10000, 0x10000 + len(code), count=20)
            expected, sentinel = struct.unpack("<dd", uc.mem_read(0x10400, 16))
            self.assertEqual(sentinel, 9.125)
            self.assertEqual(uc.reg_read(UC_X86_REG_EFLAGS), 0x202)
            self.assertAlmostEqual(result, expected, places=12)
            self.assertAlmostEqual(result, math.atan2(y, x), places=12)


if __name__ == "__main__":
    unittest.main()

"""Actual photo bool ABI and production non-Flash UI input integration."""
import hashlib
import json
from pathlib import Path
import re
import struct
import subprocess
import tempfile
import unittest

import pefile
from unicorn import Uc, UC_ARCH_X86, UC_MODE_32
from unicorn.x86_const import UC_X86_REG_ECX, UC_X86_REG_ESP, UC_X86_REG_EIP

from tests.host_environment import CLANG, environment
from tests import test_touch_controls as touch_checks

ROOT = Path(__file__).resolve().parents[1]


class PhotoGradeContextTest(unittest.TestCase):
    def test_original_bool_variable_offset_and_mask(self):
        source = Path(json.loads((ROOT / "config/inputs.json").read_text())["source_root"])
        raw = (source / "Builds/Release/Bioshock.exe").read_bytes()
        self.assertEqual(hashlib.sha256(raw).hexdigest(), "7f2ee821ad60eaf08322b4bc6ebbf839d7fe010b4d0255ec12b5be4a2966f7f6")
        image = pefile.PE(data=raw)
        vm = Uc(UC_ARCH_X86, UC_MODE_32)
        base = image.OPTIONAL_HEADER.ImageBase
        vm.mem_map(base, image.OPTIONAL_HEADER.SizeOfImage)
        vm.mem_write(base, image.get_memory_mapped_image())
        vm.mem_map(0x100000, 0x20000)
        stack, stop, frame, code, field, pawn, output = (0x100800, 0x101000, 0x102000, 0x103000, 0x104000, 0x105000, 0x110000)

        def words(address, *values):
            vm.mem_write(address, struct.pack("<" + "I" * len(values), *values))

        def word(address):
            return struct.unpack("<I", vm.mem_read(address, 4))[0]

        # Explicit VM-dispatch fixture: the original InstanceVariable decoder
        # and outer BoolVariable execute unchanged, with no instruction hooks.
        words(0x11298098, 0x1095F780, 0)
        count = 0
        for offset in (0x934, 0xA28):
            for bit in range(32):
                mask = 1 << bit
                words(field + 108, offset)
                words(field + 148, mask)
                words(field + 132, mask ^ 0xFFFFFFFF)  # Wrong ABI decoy.
                for value in (0, mask, mask ^ 0xFFFFFFFF, 0xFFFFFFFF):
                    words(pawn + offset, value)
                    vm.mem_write(code, b"\x01" + struct.pack("<I", field))
                    words(frame + 12, code)
                    words(stack, stop, frame, output)
                    words(output, 0xBAD0BAD0)
                    vm.reg_write(UC_X86_REG_ESP, stack)
                    vm.reg_write(UC_X86_REG_ECX, pawn)
                    vm.emu_start(0x1095E0C0, stop, count=1000)
                    self.assertEqual(vm.reg_read(UC_X86_REG_EIP), stop)
                    self.assertEqual(vm.reg_read(UC_X86_REG_ESP), stack + 12)
                    self.assertEqual(word(output), int(bool(value & mask)))
                    self.assertEqual(word(0x111BE2C4), pawn + offset)
                    self.assertEqual(word(pawn + offset), value)
                    count += 1
        print(f"PASS original BioShock BoolVariable + InstanceVariable: {count} packed-bit/offset cases, metadata+0x6C/+0x94 with wrong-ABI decoy, unchanged storage and exact return")

    def test_original_photo_lifecycle_and_bindings(self):
        receipt = json.loads((ROOT / "work/photo-grade-290/source-receipt.json").read_text())
        scripts = {}
        for item in receipt:
            raw = (ROOT / "work/photo-grade-290/source" / (item["owner"] + ".uc")).read_bytes()
            # The extraction writer translated original CRLF once more on
            # Windows; recover the pinned source bytes without editing them.
            original = raw.replace(b"\r\r\n", b"\r\n")
            self.assertTrue(item["source_complete"])
            self.assertEqual(hashlib.sha256(original).hexdigest(), item["source_sha256"])
            scripts[item["owner"]] = original.decode("utf-8").replace("\r\n", "\n")
        player = scripts["ShockPlayer"]
        camera = scripts["ResearchCamera"]
        self.assertRegex(player, r"var\s+private\s+bool\s+IsDisplayingPhoto\s*;")
        clicked = player[player.index("event OnCameraClick("):player.index("event EPhotoGrade GetGradeFromScore(")]
        self.assertIn("IsDisplayingPhoto = true;", clicked)
        self.assertIn("ResetPhotoDisplayUI();", clicked)
        self.assertIn("FinishedPhotoDisplay();", clicked)  # Rejected photos close immediately.
        finished = player[player.index("exec function FinishedPhotoDisplay()"):player.index("native function GPSDebug(")]
        self.assertRegex(finished, r"if\s*\(!IsDisplayingPhoto\)\s*\{\s*return;")
        for statement in ('POPINPUTCONTEXT PhotoGradingUIActive', 'ClearPhotoInfo', 'ShowAll',
                          'IsDisplayingPhoto = false;', 'ResearchPhotos();', 'PhotoDisplayInProgress = false;'):
            self.assertIn(statement, finished)
        self.assertIn('PUSHINPUTCONTEXT PhotoGradingUIActive', camera)
        self.assertIn('ForcePause();', camera)
        self.assertIn("GetPlayingMovie( 'HUD' )", player)
        source = Path(json.loads((ROOT / "config/inputs.json").read_text())["source_root"])
        bindings = (source / "Builds/Release/DefUser.ini").read_text(errors="replace")
        section = bindings.split("[PhotoGradingUIActive]", 1)[1].split("\n[", 1)[0]
        for key in ("LeftMouse", "RightMouse", "Escape", "Enter"):
            self.assertRegex(section, rf"(?m)^{key}=FinishedPhotoDisplay\s*$")

    def test_production_reflection_pointer_input_and_resume(self):
        env, linker = environment("x64")
        out = ROOT / "build/probes/photo-grade-291"
        out.mkdir(parents=True, exist_ok=True)
        host = (ROOT / "android/host/host.h").read_text()
        runtime = (ROOT / "android/runtime/recomp_types.h").read_text()
        regs = host[host.index("typedef struct void_regs {"):host.index("} void_regs;") + len("} void_regs;")]
        end = runtime.index("} xmm_t;") + len("} xmm_t;")
        xmm = runtime[runtime.rindex("typedef union {", 0, end):end]
        slots = re.search(r"#define RECOMP_X87_SLOTS \d+", runtime).group()
        header = xmm + "\n" + regs + "\n" + slots + r'''
#define MEM8(address) (*(uint8_t *)ADDR(address))
extern double g_st_win[RECOMP_X87_SLOTS];
extern int64_t g_st_i64_win[RECOMP_X87_SLOTS];
extern int g_fp_sp;
void void_regs_save(void_regs *out);
void void_regs_load(const void_regs *in);
uint32_t void_galloc(uint32_t bytes);
void void_gfree(uint32_t address);
uint32_t void_call_guest_this(uint32_t address,uint32_t self,int nargs,const uint32_t *args);
'''
        native = (ROOT / "android/host/android_main.c").read_text()
        pointer = touch_checks.TouchControlsTest._method(native, "JNIEXPORT jboolean JNICALL Java_org_bioshock_port_GameActivity_nativePointer")
        with tempfile.TemporaryDirectory(prefix="photo-grade-") as temporary:
            temp = Path(temporary)
            (temp / "bioshock_guest_state_fixture.h").write_text(header)
            (temp / "photo_native_pointer_fixture.h").write_text(pointer)
            obj, executable = temp / "photo.obj", temp / "photo.exe"
            subprocess.run([str(CLANG), "/nologo", "/O2", "/std:c11", "/D_CRT_SECURE_NO_WARNINGS", "/DRECOMP_X87_WINDOW",
                            "/clang:--target=x86_64-pc-windows-msvc", "/I" + temporary, "/c", str(ROOT / "tests/test_photo_grade_context.c"),
                            "/Fo" + str(obj)], cwd=ROOT, env=env, check=True)
            subprocess.run([str(linker), "/nologo", str(obj), "/OUT:" + str(executable)], env=env, check=True)
            process = subprocess.run([str(executable)], env=env, capture_output=True, text=True, timeout=30)
            self.assertEqual(process.returncode, 0, process.stdout + process.stderr)
            (out / "host.log").write_text(process.stdout + process.stderr)
            print(process.stdout, end="")
            flags = ["--target=aarch64-linux-android29", "-O2", "-std=gnu11", "-Wall", "-Wextra", "-Werror",
                     "-fno-strict-aliasing", "-fwrapv", "-DRECOMP_X87_WINDOW", "-DUT3_ANDROID_APP", "-I" + temporary,
                     "-Iandroid/runtime", "-Iandroid/host", "-Igenerated"]
            subprocess.run([str(CLANG.with_name("clang.exe")), *flags, "tests/test_photo_grade_context.c", "-lm",
                            "-o", str(out / "test-photo-grade")], cwd=ROOT, check=True)


if __name__ == "__main__":
    unittest.main()

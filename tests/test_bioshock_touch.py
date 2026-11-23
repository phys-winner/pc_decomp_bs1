"""Run the production BioShock movie classifier against bounded guest metadata."""
from pathlib import Path
import re
import subprocess
import tempfile
import unittest

from tests.host_environment import CLANG, environment

ROOT = Path(__file__).resolve().parents[1]


class BioshockTouchTest(unittest.TestCase):
    def test_original_ui_queue_and_consumed_position(self):
        env, linker = environment("x64")
        out = ROOT / "build/android-tests"
        out.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(prefix="bioshock-ui-queue-") as temporary:
            obj = Path(temporary) / "ui.obj"
            exe = Path(temporary) / "ui.exe"
            subprocess.run([str(CLANG), "/nologo", "/O2", "/std:c11", "/D_CRT_SECURE_NO_WARNINGS",
                            "/clang:--target=x86_64-pc-windows-msvc", "/c", str(ROOT / "tests/test_touch_ui.c"),
                            "/Fo" + str(obj)], cwd=ROOT, env=env, check=True)
            subprocess.run([str(linker), "/nologo", str(obj), "/OUT:" + str(exe)], env=env, check=True)
            subprocess.run([str(exe)], env=env, check=True, timeout=30)
        compiler = CLANG.with_name("clang.exe")
        flags = ["--target=aarch64-linux-android29", "-O2", "-std=gnu11", "-Wall", "-Wextra", "-Werror",
                 "-Wno-unused-function", "-fno-strict-aliasing", "-fwrapv", "-DRECOMP_X87_WINDOW",
                 "-Iandroid/runtime", "-Iandroid/host", "-Igenerated"]
        subprocess.run([str(compiler), *flags, "tests/test_touch_ui.c", "-lm",
                        "-o", str(out / "test-touch-ui")], cwd=ROOT, check=True)
        subprocess.run([str(compiler), *flags, "tests/test_bioshock_touch.c", "-lm",
                        "-o", str(out / "test-bioshock-touch")], cwd=ROOT, check=True)
        subprocess.run([str(compiler), *flags, "-DUT3_ANDROID_APP", "-c", "android/host/input.c",
                        "-o", str(out / "input.o")], cwd=ROOT, check=True)

    def test_visible_input_movies_and_gameplay_epoch(self):
        env, linker = environment("x64")
        with tempfile.TemporaryDirectory(prefix="bioshock-touch-") as temporary:
            out = Path(temporary)
            host = (ROOT / "android/host/host.h").read_text()
            runtime = (ROOT / "android/runtime/recomp_types.h").read_text()
            regs = host[host.index("typedef struct void_regs {"):host.index("} void_regs;") + len("} void_regs;")]
            end = runtime.index("} xmm_t;") + len("} xmm_t;")
            xmm = runtime[runtime.rindex("typedef union {", 0, end):end]
            slots = re.search(r"#define RECOMP_X87_SLOTS \d+", runtime).group()
            (out / "bioshock_guest_state_fixture.h").write_text(xmm + "\n" + regs + "\n" + slots + r'''
#define MEM8(address) (*(uint8_t *)ADDR(address))
extern double g_st_win[RECOMP_X87_SLOTS];
extern int64_t g_st_i64_win[RECOMP_X87_SLOTS];
extern int g_fp_sp;
void void_regs_save(void_regs *out);
void void_regs_load(const void_regs *in);
uint32_t void_galloc(uint32_t bytes);
uint32_t void_gstrdup(const char *text);
void void_gfree(uint32_t address);
uint32_t void_call_guest_this(uint32_t address,uint32_t self,int nargs,const uint32_t *args);
''')
            subprocess.run([str(CLANG), "/nologo", "/O2", "/std:c11", "/D_CRT_SECURE_NO_WARNINGS", "/DRECOMP_X87_WINDOW",
                            "/clang:--target=x86_64-pc-windows-msvc", "/I" + str(out), "/c",
                            str(ROOT / "tests/test_bioshock_touch.c"), "/Fo" + str(out / "test.obj")],
                           cwd=ROOT, env=env, check=True)
            subprocess.run([str(linker), "/nologo", str(out / "test.obj"),
                            "/OUT:" + str(out / "test.exe")], env=env, check=True)
            subprocess.run([str(out / "test.exe")], env=env, check=True)
            compiler = CLANG.with_name("clang.exe")
            flags = ["--target=aarch64-linux-android29", "-O2", "-std=gnu11", "-Wall", "-Wextra", "-Werror",
                     "-fno-strict-aliasing", "-fwrapv", "-DRECOMP_X87_WINDOW",
                     "-Iandroid/runtime", "-Iandroid/host", "-Igenerated"]
            subprocess.run([str(compiler), *flags, "-c", "android/host/bioshock_touch.c",
                            "-o", str(out / "bioshock_touch.o")], cwd=ROOT, check=True)
            subprocess.run([str(compiler), *flags, "tests/test_bioshock_touch.c", "-lm",
                            "-o", str(out / "test-bioshock-touch")], cwd=ROOT, check=True)


if __name__ == "__main__":
    unittest.main()

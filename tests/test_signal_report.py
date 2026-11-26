"""Exercise the allocation-free signal-report formatting and frame bounds helpers."""
from pathlib import Path
import subprocess
import unittest

from tests.host_environment import CLANG, environment

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / "build/tests/signal-report"


class SignalReportTest(unittest.TestCase):
    def test_helpers_format_modules_and_bound_frames(self):
        source = (ROOT / "android/host/core.c").read_text(encoding="utf-8").replace("\r\n", "\n")
        start = source.index("/* signal-report helpers begin */") + len("/* signal-report helpers begin */")
        end = source.index("/* signal-report helpers end */")
        helpers = source[start:end]
        harness = r'''#include <stdint.h>
#include <stddef.h>
#include <limits.h>
#include <assert.h>
#include <string.h>
#include <stdio.h>
/* HELPERS */
int main(void) {
    fault_modules[0] = (fault_module){ 0x1000, 0x2000, "libbioshock.so" };
    fault_module_count = 1;
    char out[128], *p = out;
    fault_module_offset(&p, 0x1234); *p = 0;
    assert(!strcmp(out, "libbioshock.so+0x234"));
    p = out; fault_hex(&p, 0); *p = 0; assert(!strcmp(out, "0x0"));
    p = out; fault_signed(&p, -6); *p = 0; assert(!strcmp(out, "-6"));
    assert(fault_fp_valid(0x1800, 0x1000, 0));
    assert(!fault_fp_valid(0x17f0, 0x1000, 0x1800));
    assert(!fault_fp_valid(0x1801, 0x1000, 0));
    assert(!fault_fp_valid(0x201000, 0x1000, 0));
    puts("PASS signal report: hex, module offsets, and frame bounds");
    return 0;
}'''.replace("/* HELPERS */", helpers)
        OUT.mkdir(parents=True, exist_ok=True)
        probe, obj, binary = OUT / "probe.c", OUT / "probe.obj", OUT / "probe.exe"
        probe.write_text(harness, encoding="utf-8")
        env, linker = environment("x64")
        for command in ([str(CLANG), "/nologo", "/O2", "/W3", "/std:c11", "/clang:--target=x86_64-pc-windows-msvc",
                         "/c", str(probe), "/Fo" + str(obj)],
                        [str(linker), "/nologo", str(obj), "/OUT:" + str(binary)],
                        [str(binary)]):
            result = subprocess.run(command, cwd=ROOT, env=env, capture_output=True, text=True, timeout=60)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("PASS signal report", result.stdout)


if __name__ == "__main__":
    unittest.main()

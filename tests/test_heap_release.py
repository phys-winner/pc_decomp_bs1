"""The guest heaps return memory: tests/heap_check.c compiles android/host/memory.c into itself and runs on a device.

guest_sys_free takes dlmalloc's unmaps of a segment's top and of glued segments allocation by allocation (The Sims
2's rule, wiki note 02), void_heap_purge gives the host pages of free chunks back without touching live blocks, and
HeapDestroy returns every page and reservation. The build is checked always; the run needs BIOSHOCK_TEST_ANDROID.
"""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
SERIAL = os.environ.get("BIOSHOCK_TEST_ANDROID")
SDK = Path(os.environ.get("ANDROID_SDK_ROOT", Path.home() / "AppData/Local/Android/Sdk"))
CLANG = SDK / "ndk/27.2.12479018/toolchains/llvm/prebuilt/windows-x86_64/bin/clang.exe"


class HeapReleaseTest(unittest.TestCase):
    def build(self, folder):
        binary = folder / "heap-check"
        built = subprocess.run([str(CLANG), "--target=aarch64-linux-android29", "-O2", "-std=gnu11", "-Wall",
                                "-Wno-unused-function", "-Wno-unused-variable", "-Wno-unused-but-set-variable",
                                "-I" + str(ROOT / "android"), "-I" + str(ROOT / "android/host"),
                                str(ROOT / "tests/heap_check.c"), "-o", str(binary)],
                               capture_output=True, text=True, timeout=180)
        self.assertEqual(built.returncode, 0, built.stdout + built.stderr)
        return binary

    @unittest.skipUnless(CLANG.exists(), "NDK 27.2 clang")
    def test_heap_check_builds(self):
        with tempfile.TemporaryDirectory(prefix="bioshock-heap-", dir=ROOT / "build") as temporary:
            self.build(Path(temporary))

    @unittest.skipUnless(SERIAL and CLANG.exists(), "Set BIOSHOCK_TEST_ANDROID to the explicitly selected device")
    def test_heaps_return_memory_on_device(self):
        adb = [str(SDK / "platform-tools/adb.exe"), "-s", SERIAL]
        with tempfile.TemporaryDirectory(prefix="bioshock-heap-", dir=ROOT / "build") as temporary:
            binary = self.build(Path(temporary))
            remote = "/data/local/tmp/bioshock-heap-check"
            subprocess.run([*adb, "push", str(binary), remote], check=True, capture_output=True, timeout=60)
            result = subprocess.run([*adb, "shell", f"chmod 755 {remote} && {remote}; echo EXIT=$?"],
                                    capture_output=True, text=True, timeout=600)
            subprocess.run([*adb, "shell", "rm", "-f", remote], capture_output=True, timeout=30)
        output = result.stdout + result.stderr
        print(output[-4000:])
        self.assertIn("EXIT=0", output)
        self.assertIn("PASS guest heaps", output)


if __name__ == "__main__":
    unittest.main()

"""Exercise the GLES compressed-slice packer from r10_gles.c."""
from pathlib import Path
import subprocess
import unittest

from tests.host_environment import CLANG, environment
from tests.test_mobile_shaders import function

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / "build/tests/gles-compressed-upload"


class GlesCompressedUploadTest(unittest.TestCase):
    def test_packs_all_compressed_volume_slices_and_skips_row_padding(self):
        source = (ROOT / "android/render10/r10_gles.c").read_text(encoding="utf-8")
        packer = function(source, "static uint8_t *pack_compressed_slices(")
        harness = r'''
#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
@PACKER@
int main(void) {
    const uint8_t input[] = {
        1,2,3,4,0,0, 5,6,7,8,0,0,
        9,10,11,12,0,0, 13,14,15,16,0,0,
        21,22,23,24,0,0, 25,26,27,28,0,0,
        29,30,31,32,0,0, 33,34,35,36,0,0
    };
    uint8_t *packed = pack_compressed_slices(input, 6, 24, 2, 2, 2, 2);
    const uint8_t expected[] = {1,2,3,4,5,6,7,8,21,22,23,24,25,26,27,28};
    assert(packed && !memcmp(packed, expected, sizeof expected));
    free(packed);
    return 0;
}
'''.replace("@PACKER@", packer)
        OUT.mkdir(parents=True, exist_ok=True)
        c_file = OUT / "test.c"
        obj_file = OUT / "test.obj"
        exe_file = OUT / "test.exe"
        c_file.write_text(harness, encoding="utf-8")
        env, linker = environment("x64")
        subprocess.run([str(CLANG), "/nologo", "/O2", "/std:c11", "/c", str(c_file), "/Fo" + str(obj_file)],
                       cwd=ROOT, env=env, check=True)
        subprocess.run([str(linker), "/nologo", str(obj_file), "/OUT:" + str(exe_file)], env=env, check=True)
        result = subprocess.run([str(exe_file)], env=env, capture_output=True, text=True, timeout=30)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("(GLsizei)slices", source)


if __name__ == "__main__":
    unittest.main()

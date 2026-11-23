"""Validate the diagnostic probe's legacy and explicit ELF owner offsets."""
from pathlib import Path
import subprocess
import tempfile
import unittest

from tests.host_environment import CLANG, environment
from tests.test_mobile_shaders import function

ROOT = Path(__file__).resolve().parents[1]


class StateProbeArgsTest(unittest.TestCase):
    def test_explicit_and_legacy_machine_offsets(self):
        source = (ROOT / "work/read_bioshock_state.c").read_text(encoding="utf-8")
        program = r'''
#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
/* PRODUCTION */
int main(void){
    uintptr_t offset;
    assert(thread_machine_offset(NULL,&offset) && offset==0x522E300);
    assert(thread_machine_offset("74a8738",&offset) && offset==0x74A8738);
    assert(thread_machine_offset("0x74A8738",&offset) && offset==0x74A8738);
    const char *bad[]={"","0","-1","74a8738x","100000000","FFFFFFFFFFFFFFFFF"};
    for(unsigned i=0;i<sizeof bad/sizeof *bad;++i){offset=123;assert(!thread_machine_offset(bad[i],&offset) && offset==123);}
    return 0;
}
'''.replace("/* PRODUCTION */", function(source, "static int thread_machine_offset("))
        env, linker = environment("x64")
        with tempfile.TemporaryDirectory(prefix="bioshock-probe-args-") as temporary:
            folder = Path(temporary)
            (folder / "check.c").write_text(program, encoding="utf-8")
            subprocess.run([str(CLANG), "/nologo", "/O2", "/std:c11", "/clang:--target=x86_64-pc-windows-msvc",
                            "/c", str(folder / "check.c"), "/Fo" + str(folder / "check.obj")], env=env, check=True)
            subprocess.run([str(linker), "/nologo", str(folder / "check.obj"), "/OUT:" + str(folder / "check.exe")], env=env, check=True)
            subprocess.run([str(folder / "check.exe")], env=env, check=True, timeout=30)


if __name__ == "__main__":
    unittest.main()

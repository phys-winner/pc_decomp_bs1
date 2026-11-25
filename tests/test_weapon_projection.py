"""First-person framing is independent of the world's Horizontal FOV Lock."""
from pathlib import Path
import subprocess
import tempfile
import unittest

from tests.host_environment import CLANG, environment
from tests import test_touch_controls

ROOT = Path(__file__).resolve().parents[1]


class WeaponProjectionTest(unittest.TestCase):
    def test_authored_framing_and_zoom_survive_widescreen(self):
        source = (ROOT / "android/host/bioshock_render10.c").read_text()
        method = test_touch_controls.TouchControlsTest._method(source, "void void_bioshock_weapon_projection(")
        program = r'''
#include <stdint.h>
#include <math.h>
#include <assert.h>
#include <stdio.h>
static union { uint32_t u[4096]; float f[4096]; } memory;
#define MEM32(a) memory.u[((a)-0x10000)/4]
#define MEMF(a) memory.f[((a)-0x10000)/4]
static int render_member(uint32_t a,uint32_t bytes) { return a>=0x10000 && a+bytes<=0x14000; }
/* METHOD */
int main(void) {
    const uint32_t node=0x10000,viewport=0x10400,config=0x10800,args=0x10C00;
    MEM32(node+16)=viewport;MEM32(viewport+132)=config;
    for(int lock=0;lock<2;++lock)for(int fov=20;fov<=100;fov+=5)
        for(int w=4;w<=24;++w){
            float aspect=w/3.f,half=fov*.00872664625997f;
            float incoming=lock?half:atanf(tanf(half)*aspect/(4.f/3.f));
            MEM32(config+144)=lock;MEMF(args)=MEMF(args+4)=incoming;
            MEMF(args+8)=1;MEMF(args+12)=aspect;
            void_bioshock_weapon_projection(node,args);
            assert(fabsf(MEMF(args)-half)<.000002f && MEMF(args)==MEMF(args+4));
            assert(MEMF(args+8)==1 && MEMF(args+12)==aspect);
        }
    MEM32(config+144)=0;MEMF(args)=NAN;MEMF(args+12)=2;
    void_bioshock_weapon_projection(node,args);assert(isnan(MEMF(args)));
    MEMF(args)=.5f;MEMF(args+12)=0;
    void_bioshock_weapon_projection(node,args);assert(MEMF(args)==.5f);
    MEM32(node+16)=0;void_bioshock_weapon_projection(node,args);assert(MEMF(args)==.5f);
    puts("PASS authored weapon framing across 714 FOV/aspect/lock cases, including zoom and invalid inputs");
}
'''.replace("/* METHOD */", method)
        env, linker = environment("x64")
        with tempfile.TemporaryDirectory(prefix="bioshock-weapon-projection-") as temporary:
            folder = Path(temporary)
            path = folder / "projection.c"
            path.write_text(program)
            obj, exe = folder / "projection.obj", folder / "projection.exe"
            subprocess.run([str(CLANG), "/nologo", "/O2", "/std:c11", "/c", str(path),
                            "/Fo" + str(obj)], env=env, check=True)
            subprocess.run([str(linker), "/nologo", str(obj), "/OUT:" + str(exe)], env=env, check=True)
            subprocess.run([str(exe)], env=env, check=True)

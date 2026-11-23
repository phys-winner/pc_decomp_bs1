"""Diagnostic pose lookup follows inherited, case-insensitive Unreal FNames."""
from pathlib import Path
import subprocess
import tempfile
import unittest

from tests.host_environment import CLANG, environment
from tests.test_mobile_shaders import function

ROOT = Path(__file__).resolve().parents[1]


class ReadBioshockStateTest(unittest.TestCase):
    def test_inherited_case_insensitive_pose_fields(self):
        source = (ROOT / "work/read_bioshock_state.c").read_text(encoding="utf-8")
        actual = "\n".join(function(source, name) for name in (
            "static int property_name_equal(", "static uint32_t property_offset(", "static void pose_fields("))
        program = r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
typedef int pid_t;
enum {PC=0x10000,PAWN=0x20000,PC_CLASS=0x30000,CONTROLLER_CLASS=0x40000,ACTOR_CLASS=0x50000,PAWN_CLASS=0x60000,
      PAWN_FIELD=0x70000,LOCATION_FIELD=0x71000,ROTATION_FIELD=0x72000,DESIRED_FIELD=0x73000,DUMMY_FIELDS=0x74000};
static uint32_t memory[0x80000/4];
static uint32_t word(pid_t pid,uint32_t address){assert(pid==1&&!(address&3)&&address<sizeof memory);return memory[address/4];}
static void put(uint32_t address,uint32_t value){assert(!(address&3)&&address<sizeof memory);memory[address/4]=value;}
static void object_name(pid_t pid,uint32_t object,char out[96]){
    assert(pid==1);
    const char *name=object==PAWN_FIELD?"pAwN":object==LOCATION_FIELD?"location":object==ROTATION_FIELD?"rotation":object==DESIRED_FIELD?"desiredROTATION":"?";
    strcpy(out,name);
}
static void class_name(pid_t pid,uint32_t object,char out[96]){assert(pid==1);strcpy(out,object==PC?"ShockPlayerController":"ShockPlayer");}
/* ACTUAL */
int main(void){
    put(PC+44,PC_CLASS);put(PC_CLASS+56,CONTROLLER_CLASS);put(CONTROLLER_CLASS+56,ACTOR_CLASS);
    put(PAWN+44,PAWN_CLASS);put(PAWN_CLASS+56,ACTOR_CLASS);put(CONTROLLER_CLASS+84,PAWN_FIELD);
    put(PAWN_FIELD+108,1044);put(PAWN_FIELD+60,PAWN_FIELD); /* A bounded field cycle must not hide Super fields. */
    /* Real Actor metadata has hundreds of functions before its pose fields.
     * Searching only the first256 must fail this production-function check. */
    put(ACTOR_CLASS+84,DUMMY_FIELDS);
    for(unsigned i=0;i<300;++i)put(DUMMY_FIELDS+i*128+60,i==299?LOCATION_FIELD:DUMMY_FIELDS+(i+1)*128);
    put(LOCATION_FIELD+60,ROTATION_FIELD);put(ROTATION_FIELD+60,DESIRED_FIELD);
    put(LOCATION_FIELD+108,0x17c);put(ROTATION_FIELD+108,0x198);put(DESIRED_FIELD+108,0x1b4);
    put(PC+1044,PAWN);assert(property_offset(1,PC,"Pawn")==1044);
    assert(property_offset(1,PC,"Location")==0x17c&&property_offset(1,PAWN,"Location")==0x17c);
    assert(property_offset(1,PC,"Rotation")==0x198&&property_offset(1,PAWN,"DesiredRotation")==0x1b4);
    assert(!property_offset(1,PC,"ViewRotation")&&!property_offset(1,0,"Pawn"));
    assert(!property_name_equal("LocationX","location")&&!property_name_equal("location","LocationX"));
    put(PC+0x17c,0x3f800000);put(PC+0x180,0x40000000);put(PC+0x184,0x40400000);
    put(PC+0x198,(uint32_t)-4096);put(PC+0x19c,65500);
    pose_fields(1,PC,"controller");pose_fields(1,PAWN,"pawn");
    put(ACTOR_CLASS+56,ACTOR_CLASS);assert(!property_offset(1,PC,"Unknown"));
    puts("PASS actual diagnostic inherited/case-insensitive property lookup beyond300 fields, bounded cycles, float locations and signed rotators");
    return 0;
}
'''
        env, linker = environment("x64")
        with tempfile.TemporaryDirectory(prefix="bioshock-pose-reader-") as temporary:
            folder = Path(temporary)
            probe, obj, exe = folder / "probe.c", folder / "probe.obj", folder / "probe.exe"
            probe.write_text(program.replace("/* ACTUAL */", actual), encoding="utf-8")
            for command in ([str(CLANG), "/nologo", "/O2", "/std:c11", "/D_CRT_SECURE_NO_WARNINGS",
                             "/clang:--target=x86_64-pc-windows-msvc", "/c", str(probe), "/Fo" + str(obj)],
                            [str(linker), "/nologo", str(obj), "/OUT:" + str(exe)]):
                result = subprocess.run(command, env=env, capture_output=True, text=True, timeout=30)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            result = subprocess.run([str(exe)], env=env, capture_output=True, text=True, timeout=30)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIn("controller.Location offset=380 data=1.000000(3F800000)", result.stdout)
            self.assertIn("controller.Rotation offset=408 data=-4096(FFFFF000) 65500(0000FFDC)", result.stdout)
            self.assertIn("pawn.DesiredRotation offset=436", result.stdout)
            print(result.stdout)


if __name__ == "__main__":
    unittest.main()

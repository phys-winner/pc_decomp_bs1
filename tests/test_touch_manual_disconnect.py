"""Prove the original reconnect latch and the narrowly scoped manual-off reset."""
from pathlib import Path
import hashlib
import json
import struct
import subprocess
import tempfile
import unittest

import pefile
from unicorn import Uc, UC_ARCH_X86, UC_MODE_32, UC_HOOK_CODE
from unicorn.x86_const import UC_X86_REG_EAX, UC_X86_REG_ESI, UC_X86_REG_EBP, UC_X86_REG_ESP, UC_X86_REG_EIP

from tests.host_environment import CLANG, environment
from tests.test_mobile_shaders import function

ROOT = Path(__file__).resolve().parents[1]


class ManualTouchDisconnectTest(unittest.TestCase):
    def test_get_state_race_and_original_warning_pipeline(self):
        source = (ROOT / "android/host/bioshock_touch.c").read_text(encoding="utf-8")
        xinput = (ROOT / "android/host/xinput.c").read_text(encoding="utf-8")
        program = r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
static int owned=1,enabled=1,physical,writable=1,writes,connected=1;
static uint32_t remembered=1;
static uint8_t output[16];
static uint32_t arguments[2]={0,4096};
#define ARG(i) arguments[i]
#define GPTR(a) (assert((a)==4096),output)
#define WINAPI_ORDINAL(library,name,ordinal,count) static uint32_t name(void)
static int void_machine_is_owned(void){return owned;}
static int void_bioshock_touch_enabled(void){return enabled;}
static int physical_pad(void){return physical;}
static int (*void_input_pad_present)(void)=physical_pad;
static int void_vm_writable(uint32_t address,uint32_t size){
    if(address==4096){assert(size==16);return 1;}
    assert(address==0x11171e38&&size==4);return writable;
}
static uint32_t *checked(uint32_t address){assert(owned&&address==0x11171e38);++writes;return &remembered;}
#define MEM32(a) (*checked(a))
static void void_set_last_error(uint32_t error){assert(!error);}
static int void_input_touch_pad(uint32_t index,uint8_t state[16]){
    if(index||!connected)return 0;memset(state,0x12,16);return 1;
}
void void_bioshock_touch_xinput_state(uint32_t index,int present) __attribute__((weak));
/* ACTUAL */
int main(void){
    assert(XInputGetState()==0&&remembered==1&&!writes&&output[0]==0x12);
    /* JNI can disable after the last game-context poll, before the very next XInput poll. */
    enabled=0;connected=0;assert(XInputGetState()==1167&&!remembered&&writes==1&&output[0]==0x12);
    int manual=remembered;
    remembered=1;assert(XInputGetState()==1167&&!remembered&&writes==2);int rearmed=remembered;
    enabled=1;physical=1;connected=1;assert(XInputGetState()==0);
    remembered=1;enabled=0;assert(XInputGetState()==0&&remembered==1&&writes==2);
    physical=0;connected=0;assert(XInputGetState()==1167&&remembered==1&&writes==2);int unplug=remembered;
    enabled=1;connected=1;assert(XInputGetState()==0);connected=0;
    assert(XInputGetState()==1167&&remembered==1&&writes==2);int unexpected=remembered;
    enabled=0;owned=0;assert(XInputGetState()==1167&&remembered==1&&writes==2);
    owned=1;writable=0;assert(XInputGetState()==1167&&remembered==1&&writes==2);writable=1;
    arguments[0]=1;assert(XInputGetState()==1167&&remembered==1&&writes==2);arguments[0]=0;
    printf("{\"manual\":%u,\"rearmed\":%u,\"physical_unplug\":%u,\"unexpected_virtual_loss\":%u}\n",manual,rearmed,unplug,unexpected);
}
'''.replace("/* ACTUAL */", function(source, "void void_bioshock_touch_xinput_state(") + "\n" +
            function(xinput, "static uint32_t xinput_disconnected(") + "\n" +
            function(xinput, 'WINAPI_ORDINAL("xinput1_3.dll", XInputGetState,'))
        setter = function(source, "void void_bioshock_touch_options(")
        self.assertNotIn("MEM32", setter)
        env, linker = environment("x64")
        with tempfile.TemporaryDirectory(prefix="manual-touch-off-", dir=ROOT / "build") as temporary:
            stage = Path(temporary); path = stage / "probe.c"; path.write_text(program, encoding="utf-8")
            for command in ([str(CLANG), "/nologo", "/O2", "/W4", "/WX", "/std:c11", "/clang:--target=x86_64-pc-windows-msvc",
                             "/c", str(path), "/Fo" + str(stage / "probe.obj")],
                            [str(linker), "/nologo", str(stage / "probe.obj"), "/OUT:" + str(stage / "probe.exe")],
                            [str(stage / "probe.exe")]):
                result = subprocess.run(command, cwd=ROOT, env=env, capture_output=True, text=True, timeout=60)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            results = json.loads(result.stdout)
        raw = (ROOT / "generated/input/Bioshock.exe").read_bytes()
        self.assertEqual(hashlib.sha256(raw).hexdigest(), "7f2ee821ad60eaf08322b4bc6ebbf839d7fe010b4d0255ec12b5be4a2966f7f6")
        pe = pefile.PE(data=raw, fast_load=True); base = pe.OPTIONAL_HEADER.ImageBase
        for case, remembered in results.items():
            expected_warning = case in ("physical_unplug", "unexpected_virtual_loss")
            with self.subTest(case=case, remembered=remembered):
                uc = Uc(UC_ARCH_X86, UC_MODE_32)
                uc.mem_map(base, pe.OPTIONAL_HEADER.SizeOfImage)
                uc.mem_write(base, pe.get_memory_mapped_image())
                uc.mem_map(0x200000, 0x10000); uc.mem_map(0x50000000, 0x1000)
                def put(address, value): uc.mem_write(address, struct.pack("<I", value))
                put(0x111299F0, 1); put(0x11171E38, remembered)
                put(0x200018, 0x201000); put(0x201000, 0x202000)
                put(0x2010D4, 0); put(0x20210C, 0x50000000)
                uc.reg_write(UC_X86_REG_ESI, 0x200000)
                uc.reg_write(UC_X86_REG_EBP, 0x208000)
                uc.reg_write(UC_X86_REG_ESP, 0x20F000)
                warning = []
                def hook(vm, pc, size, unused):
                    if pc in (0x10E49594, 0x50000000):
                        sp = vm.reg_read(UC_X86_REG_ESP)
                        ret = struct.unpack("<I", vm.mem_read(sp, 4))[0]
                        vm.reg_write(UC_X86_REG_EAX, 1167 if pc == 0x10E49594 else 1)
                        vm.reg_write(UC_X86_REG_ESP, sp + (12 if pc == 0x10E49594 else 4))
                        vm.reg_write(UC_X86_REG_EIP, ret)
                    elif pc == 0x1092B5E0:
                        warning.append(True); vm.emu_stop()
                    elif pc == 0x10DA33F0:
                        vm.emu_stop()
                uc.hook_add(UC_HOOK_CODE, hook)
                uc.emu_start(0x10DA32FE, 0x10DA33F7, count=300)
                self.assertEqual(bool(warning), expected_warning)
                self.assertEqual(struct.unpack("<I", uc.mem_read(0x111299F0, 4))[0], 0)


if __name__ == "__main__":
    unittest.main()

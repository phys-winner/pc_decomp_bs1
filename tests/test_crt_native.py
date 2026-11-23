"""Compare pinned original x86 CRT memory effects/ABI with native wrappers."""
import hashlib
import json
from pathlib import Path
import struct
import subprocess
import unittest

import pefile
from unicorn import Uc, UC_ARCH_X86, UC_MODE_32
from unicorn.x86_const import (UC_X86_REG_EAX, UC_X86_REG_EBP, UC_X86_REG_EBX, UC_X86_REG_EDI,
                              UC_X86_REG_EFLAGS, UC_X86_REG_ESI, UC_X86_REG_ESP)

from tests.host_environment import CLANG, environment

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT/"build/tests/crt-native"
PIN = "0da6d4937077afb7f6e1659d645df95a4918b6fd951b3758f4a176b08c4dca98"
ARENA, STOP, STACK = 0x30000000,0x400000,0x308000
INITIAL = bytes((i*73+(i>>4)*17+0x35)&255 for i in range(16384))


def cases():
    for owner in (0x10014fb0,0x10015320):
        for n in range(41):
            for align in range(4):
                for delta in (-128,-3,-2,-1,0,1,2,3,128):
                    yield owner,n,align,delta,0
        for n in (255,256,257,511,512,513,1023,1024,2047,2048,2049):
            for align in (0,1,2,3,7,15):
                for delta in sorted({0,1,-1,16,-16,65,-65,n-1,1-n,n,-n,n+1,-n-1}):
                    for sse in (0,1):
                        yield owner,n,align,delta,sse
    for n in (*range(41),255,256,257,1023,1024,2048):
        for align in range(16):
            for value in (0,1,255,256,-1):
                for sse in (0,1):
                    yield 0x10014a20,n,align,value,sse


PREFIX = r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef void (*void_fn)(void);
static uint8_t arena[16384],stack[256];
static uint32_t g_eax,g_ecx,g_edx,g_ebx,g_ebp,g_esi,g_edi,g_esp;
static int fallback_calls,missing,added,disabled;
static const char *execution_mode,*legacy_mode;
static void *pointer(uint32_t address) {
    if(address>=0x30000000u&&address<0x30004000u)return arena+(address-0x30000000u);
    assert(address>=0x308000u&&address<0x308100u);return stack+(address-0x308000u);
}
#define GPTR(address) pointer((uint32_t)(address))
#define MEM32(address) (*(uint32_t *)GPTR(address))
#define ARG(i) MEM32(g_esp+4u+(i)*4u)
static void original(void) {++fallback_calls;g_eax=0xbad0u;g_esp+=4;}
static void_fn installed[3];
static void_fn void_dispatch_find(uint32_t va) {(void)va;return missing?NULL:original;}
static void void_dispatch_add(uint32_t va,void_fn fn) {
    unsigned i=va==0x10014fb0u?0:va==0x10015320u?1:2;
    assert(va==0x10014fb0u||va==0x10015320u||va==0x10014a20u);installed[i]=fn;++added;
}
static void void_log(const char *text,...) {(void)text;}
static const char *fixture_getenv(const char *name) {
    if(!strcmp(name,"BIOSHOCK_NATIVE_CRT"))return disabled?"0":NULL;
    if(!strcmp(name,"BIOSHOCK_EXECUTION_MODE"))return execution_mode;
    assert(!strcmp(name,"UT3_EXECUTION_MODE"));return legacy_mode;
}
#define getenv fixture_getenv
'''
HARNESS = r'''
static void args(uint32_t destination,uint32_t source,uint32_t count) {
    g_esp=0x308000;MEM32(g_esp)=0xdead0000;
    MEM32(g_esp+4)=destination;MEM32(g_esp+8)=source;MEM32(g_esp+12)=count;
    g_ebx=0xb1234567;g_ebp=0x71234567;g_esi=0x51234567;g_edi=0x61234567;
    g_ecx=0xc1234567;g_edx=0xd1234567;
}
int main(void) {
    missing=1;void_crt_native_init();assert(!added);missing=0;void_crt_native_init();assert(added==3);
    assert(crt_range(0xffffffffu,1)&&!crt_range(0xffffffffu,2));
    for(unsigned i=0;i<2;++i) {
        args(0xfffffff0u,0x30000000,32);crt_mode=1;installed[i]();assert(fallback_calls==1);fallback_calls=0;
        args(0x30000000,0xfffffff0u,32);installed[i]();assert(fallback_calls==1);fallback_calls=0;
    }
    args(0xfffffff0u,0,32);crt_mode=1;installed[2]();assert(fallback_calls==1);fallback_calls=0;
    for(unsigned i=0;i<3;++i) {
        args(0,0,0);crt_mode=-1;disabled=1;installed[i]();assert(fallback_calls==1&&g_eax==0xbad0u);fallback_calls=0;disabled=0;
        args(0,0,0);crt_mode=-1;execution_mode="exact";installed[i]();assert(fallback_calls==1);fallback_calls=0;execution_mode=NULL;
        args(0,0,0);crt_mode=-1;legacy_mode="exact";installed[i]();assert(fallback_calls==1);fallback_calls=0;legacy_mode=NULL;
        args(0xffffffffu,0xffffffffu,0);crt_mode=-1;installed[i]();assert(!fallback_calls&&g_eax==0xffffffffu&&g_esp==0x308004u);
    }
    uint8_t initial[16384],expected[16384];
    for(unsigned i=0;i<sizeof initial;++i)initial[i]=(uint8_t)(i*73+(i>>4)*17+0x35);
    for(unsigned i=0;i<sizeof vectors/sizeof *vectors;++i) {
        const int64_t *r=vectors[i];uint32_t source=8192u+(uint32_t)r[2];
        uint32_t destination=r[0]==0x10014a20?source:(uint32_t)((int64_t)source+r[3]);
        uint32_t n=(uint32_t)r[1];memcpy(arena,initial,sizeof arena);memcpy(expected,initial,sizeof expected);
        for(uint32_t j=0;j<n;++j)expected[destination+j]=r[0]==0x10014a20?(uint8_t)r[3]:initial[source+j];
        args(0x30000000u+destination,r[0]==0x10014a20?(uint32_t)r[3]:0x30000000u+source,n);
        installed[r[0]==0x10014fb0?0:r[0]==0x10015320?1:2]();
        assert(!memcmp(arena,expected,sizeof arena)&&!fallback_calls&&g_eax==0x30000000u+destination&&g_esp==0x308004u);
        assert(g_ebx==0xb1234567&&g_ebp==0x71234567&&g_esi==0x51234567&&g_edi==0x61234567);
        assert(MEM32(0x308000)==0xdead0000);
    }
    printf("PASS native CRT: %zu pinned-original x86 oracle vectors, overlapping copy/set/alignment/SSE, whole arenas, EAX/ESP/callee-saved ABI, zero count, wrap fallback and Exact/disabled modes\n",sizeof vectors/sizeof *vectors);
    return 0;
}
'''


class NativeCRT(unittest.TestCase):
    def test_original_oracle_and_native(self):
        raw=(ROOT/"generated/input/msvcr80.dll").read_bytes()
        self.assertEqual(hashlib.sha256(raw).hexdigest(),PIN)
        pe=pefile.PE(data=raw,fast_load=True)
        base=pe.OPTIONAL_HEADER.ImageBase
        vm=Uc(UC_ARCH_X86,UC_MODE_32)
        vm.mem_map(base,(pe.OPTIONAL_HEADER.SizeOfImage+4095)&-4096)
        vm.mem_write(base,pe.get_memory_mapped_image())
        vm.mem_map(ARENA,len(INITIAL));vm.mem_map(0x300000,0x10000);vm.mem_map(STOP,4096)
        vectors=list(cases())
        for row in vectors:
            owner,n,align,extra,sse=row
            source=8192+align
            destination=source if owner==0x10014a20 else source+extra
            expected=bytearray(INITIAL)
            expected[destination:destination+n]=bytes([extra&255])*n if owner==0x10014a20 else INITIAL[source:source+n]
            vm.mem_write(ARENA,INITIAL);vm.mem_write(0x10095948,struct.pack("<I",sse))
            vm.mem_write(STACK,struct.pack("<4I",STOP,ARENA+destination,(extra&0xffffffff) if owner==0x10014a20 else ARENA+source,n))
            for reg,value in ((UC_X86_REG_ESP,STACK),(UC_X86_REG_EBP,0x71234567),(UC_X86_REG_EBX,0xb1234567),
                              (UC_X86_REG_ESI,0x51234567),(UC_X86_REG_EDI,0x61234567),(UC_X86_REG_EFLAGS,0x246)):
                vm.reg_write(reg,value)
            vm.emu_start(owner,STOP,count=100000)
            self.assertEqual(vm.mem_read(ARENA,len(INITIAL)),expected,row)
            self.assertEqual(vm.reg_read(UC_X86_REG_EAX),ARENA+destination,row)
            self.assertEqual(vm.reg_read(UC_X86_REG_ESP),STACK+4,row)
            for reg,value in ((UC_X86_REG_EBP,0x71234567),(UC_X86_REG_EBX,0xb1234567),(UC_X86_REG_ESI,0x51234567),(UC_X86_REG_EDI,0x61234567)):
                self.assertEqual(vm.reg_read(reg),value,row)
            self.assertFalse(vm.reg_read(UC_X86_REG_EFLAGS)&0x400,row)
        OUT.mkdir(parents=True,exist_ok=True)
        actual=(ROOT/"android/host/crt_native.c").read_text().replace('#include "host.h"',"")
        table="static const int64_t vectors[][5]={\n"+"".join("{"+",".join(map(str,r))+"},\n" for r in vectors)+"};\n"
        probe=OUT/"probe.c";probe.write_text(PREFIX+actual+table+HARNESS)
        env,linker=environment("x64")
        obj,binary=OUT/"probe.obj",OUT/"probe.exe"
        for command in ([str(CLANG),"/nologo","/O2","/W4","/WX","/std:c11","/clang:--target=x86_64-pc-windows-msvc","/c",str(probe),"/Fo"+str(obj)],
                        [str(linker),"/nologo",str(obj),"/OUT:"+str(binary)],[str(binary)],
                        [str(CLANG.with_name("clang.exe")),"--target=aarch64-linux-android29","-O2","-std=gnu11",str(probe),"-Wl,-z,max-page-size=16384","-o",str(OUT/"test-crt-native")]):
            result=subprocess.run(command,cwd=ROOT,env=env,capture_output=True,text=True,timeout=60)
            self.assertEqual(result.returncode,0,result.stdout+result.stderr)
        (OUT/"receipt.json").write_text(json.dumps(dict(result="PASS",original_sha256=PIN,vectors=len(vectors),
            sources_sha256={p:hashlib.sha256((ROOT/p).read_bytes()).hexdigest() for p in ("android/host/crt_native.c","tests/test_crt_native.py")}),indent=2)+"\n")


if __name__=="__main__":
    unittest.main()

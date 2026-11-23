"""Native block decompression: original x86 oracle, checked seam and ARM64 ABI/fallbacks."""
import hashlib
import os
from pathlib import Path
import random
import struct
import subprocess
import sys
import unittest
import zlib

import pefile
from unicorn import Uc, UC_ARCH_X86, UC_MODE_32, UC_HOOK_CODE
from unicorn.x86_const import UC_X86_REG_EAX, UC_X86_REG_EBX, UC_X86_REG_ESI, UC_X86_REG_EDI, UC_X86_REG_EBP, UC_X86_REG_ESP, UC_X86_REG_EIP

from tests.host_environment import CLANG

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / "build/tests/bioshock-inflate"
ARENA, STACK, STOP, HEAP = 0x20000000, 0x202F0000, 0x400000, 0x20400000


def vectors():
    rng = random.Random(1907)
    for n in (1, 31, 128, 32768, 33096, 65536):
        for kind in range(3):
            data = bytes(rng.randrange(256) for _ in range(n)) if kind == 0 else (b"Rapture\0" * n)[:n] if kind == 1 else bytes(n)
            packed = zlib.compress(data, 1 if kind == 0 else 9)
            for tail in (b"", b"unused trailing bytes"):
                yield packed + tail, n + 19, data, 1
            yield packed[:-1], n + 19, b"", 0
            yield packed, n - 1, b"", 0
            broken = packed[:-1] + bytes([packed[-1] ^ 255])
            yield broken, n + 19, b"", 0


HARNESS = r'''
#define _GNU_SOURCE
#include "host.h"
#include <assert.h>
#include <sys/mman.h>
uint32_t g_eax,g_ecx,g_edx,g_esp,g_ebx,g_esi,g_edi,g_ebp,g_flag_k,g_flag_a,g_flag_b,g_flag_cf;
static int owned=1,begins,ends;
static uint32_t saved[12];
static void registers(uint32_t *r,int store) {
    uint32_t *p[]={&g_eax,&g_ecx,&g_edx,&g_esp,&g_ebx,&g_esi,&g_edi,&g_ebp,&g_flag_k,&g_flag_a,&g_flag_b,&g_flag_cf};
    for(int i=0;i<12;++i){if(store)r[i]=*p[i];else *p[i]=r[i];}
}
int void_vm_readable(uint32_t a,uint32_t n){return a>=0x100000u&&a<=0x900000u&&n<=0x900000u-a;}
int void_vm_writable(uint32_t a,uint32_t n){return void_vm_readable(a,n);}
void void_log(const char *s,...){(void)s;}
void void_blocking_begin(void){assert(owned);owned=0;++begins;registers(saved,1);g_esp=0xdeadbeef;g_eax=7;g_edi=8;}
void void_blocking_end(void){assert(!owned);owned=1;++ends;registers(saved,0);}
int void_bioshock_uncompress(void);
static uint32_t word(FILE *f){uint32_t n;assert(fread(&n,4,1,f)==1);return n;}
int main(int argc,char **argv) {
    assert(argc==2);
    assert(mmap((void*)(VOID_GUEST_BASE+0x100000),0x800000,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS|MAP_FIXED_NOREPLACE,-1,0)==(void*)(VOID_GUEST_BASE+0x100000));
    FILE *f=fopen(argv[1],"rb");assert(f);uint32_t count=word(f);
    for(uint32_t i=0;i<count;++i){
        uint32_t size=word(f),capacity=word(f),n=word(f),success=word(f);
        assert(size<0x100000&&capacity<0x100000&&n<0x100000);
        memset(GPTR(0x100000),0xA5,0x800000);
        assert(fread(GPTR(0x200000),1,size,f)==size);
        uint8_t *expected=malloc(n?n:1);assert(expected&&fread(expected,1,n,f)==n);
        g_esp=0x800000;g_eax=99;g_ecx=10;g_edx=11;g_ebx=12;g_esi=13;g_edi=14;g_ebp=15;
        g_flag_k=FK_NONE;g_flag_a=16;g_flag_b=17;g_flag_cf=1;
        MEM32(g_esp)=0x400000;MEM32(g_esp+4)=0x400000;MEM32(g_esp+8)=0x700000;
        MEM32(g_esp+12)=0x200000;MEM32(g_esp+16)=size;MEM32(g_esp+20)=0x1092A1D0;MEM32(g_esp+24)=0x1092A200;
        MEM32(0x700000)=capacity;
        int got=void_bioshock_uncompress();assert(got==(int)success&&owned&&begins==ends);
        assert(g_ebx==12&&g_esi==13&&g_edi==14&&g_ebp==15&&g_ecx==10&&g_edx==11);
        if(success){
            assert(g_eax==0&&g_esp==0x800004&&MEM32(0x700000)==n);
            assert(!memcmp(GPTR(0x400000),expected,n)&&MEM8(0x400000+n)==0xA5);
            assert(g_flag_k==FK_ADD&&g_flag_a==0x800000-0x38&&g_flag_b==0x38&&!g_flag_cf);
        }else{
            assert(g_eax==99&&g_esp==0x800000&&MEM32(0x700000)==capacity);
            for(unsigned j=0;j<capacity;++j)assert(MEM8(0x400000+j)==0xA5);
            assert(g_flag_k==FK_NONE&&g_flag_a==16&&g_flag_b==17&&g_flag_cf==1);
        }
        if(success){
            g_esp=0x800000;g_eax=99;MEM32(0x700000)=capacity;
            MEM32(g_esp+20)=0x12345678;int b=begins;assert(!void_bioshock_uncompress()&&begins==b);
            MEM32(g_esp+20)=0x1092A1D0;MEM32(g_esp+4)=0x200000;
            assert(!void_bioshock_uncompress()&&begins==b);
            MEM32(g_esp+4)=0x400000;MEM32(g_esp+8)=0x400001;MEM32(0x400001)=capacity;
            assert(!void_bioshock_uncompress()&&begins==b);
            MEM32(g_esp+8)=0x700000;MEM32(g_esp+12)=UINT32_MAX-1;
            assert(!void_bioshock_uncompress()&&begins==b&&g_eax==99&&g_esp==0x800000);
        }
        free(expected);
    }
    assert(fgetc(f)==EOF);fclose(f);
    printf("PASS %u original-oracle vectors: bytes/length, cdecl/saved registers/flags, guest release/restore; truncated/corrupt/short output fallback, custom allocators/aliases/invalid ranges declined\n",count);
    return 0;
}
'''


class NativeInflateTest(unittest.TestCase):
    def test_original_and_arm64(self):
        pe = pefile.PE(str(ROOT / "generated/input/Bioshock.exe"))
        base = pe.OPTIONAL_HEADER.ImageBase
        vm = Uc(UC_ARCH_X86, UC_MODE_32)
        vm.mem_map(base, (pe.OPTIONAL_HEADER.SizeOfImage + 4095) & -4096)
        vm.mem_write(base, pe.get_memory_mapped_image())
        vm.mem_map(ARENA, 0x1000000); vm.mem_map(STOP, 0x10000)
        heap = HEAP
        stubs = {"memcpy": STOP + 0x1000, "memmove": STOP + 0x1100, "memset": STOP + 0x1200}
        for dll in pe.DIRECTORY_ENTRY_IMPORT:
            for item in dll.imports:
                name = item.name.decode() if item.name else ""
                if name in stubs: vm.mem_write(item.address, struct.pack("<I", stubs[name]))

        def step(machine, pc, size, _):
            nonlocal heap
            sp = machine.reg_read(UC_X86_REG_ESP)
            def word(a): return struct.unpack("<I", machine.mem_read(a, 4))[0]
            if pc == 0x1092A1D0:
                count = word(sp+8)*word(sp+12)
                result = heap; heap += (count+15)&-16
                self.assertLess(heap, ARENA+0x1000000)
                machine.mem_write(result, bytes(count))
            elif pc == 0x1092A200: result = 0
            elif pc in stubs.values():
                dst, src, count = word(sp+4), word(sp+8), word(sp+12)
                if count: machine.mem_write(dst, bytes([src&255])*count if pc == stubs["memset"] else bytes(machine.mem_read(src,count)))
                result = dst
            else: return
            machine.reg_write(UC_X86_REG_EAX,result)
            machine.reg_write(UC_X86_REG_ESP,sp+4)
            machine.reg_write(UC_X86_REG_EIP,word(sp))
        vm.hook_add(UC_HOOK_CODE,step)
        cases = list(vectors())
        for packed, capacity, expected, success in cases:
            if not success: continue
            heap = HEAP
            dst, length, src = ARENA+0x100000, ARENA+0x200000, ARENA
            vm.mem_write(src,packed); vm.mem_write(dst,b"\xA5"*(capacity+16)); vm.mem_write(length,struct.pack("<I",capacity))
            vm.mem_write(STACK,struct.pack("<7I",STOP,dst,length,src,len(packed),0x1092A1D0,0x1092A200))
            saved = ((UC_X86_REG_EBX,12),(UC_X86_REG_ESI,13),(UC_X86_REG_EDI,14),(UC_X86_REG_EBP,15))
            for reg, value in (*saved,(UC_X86_REG_ESP,STACK)): vm.reg_write(reg,value)
            vm.emu_start(0x10922AC0,STOP,count=10000000)
            self.assertEqual(vm.reg_read(UC_X86_REG_EAX),0)
            self.assertEqual(vm.reg_read(UC_X86_REG_ESP),STACK+4)
            for reg,value in saved: self.assertEqual(vm.reg_read(reg),value)
            self.assertEqual(bytes(vm.mem_read(length,4)),struct.pack("<I",len(expected)))
            self.assertEqual(bytes(vm.mem_read(dst,len(expected)+1)),expected+b"\xA5")
        OUT.mkdir(parents=True,exist_ok=True)
        fixture = bytearray(struct.pack("<I",len(cases)))
        for packed,capacity,expected,success in cases:
            fixture += struct.pack("<4I",len(packed),capacity,len(expected),success)+packed+expected
        (OUT/"vectors.bin").write_bytes(fixture)
        (OUT/"probe.c").write_text(HARNESS,encoding="utf-8")
        binary = OUT/"test-bioshock-inflate"
        subprocess.run([str(CLANG.with_name("clang.exe")),"--target=aarch64-linux-android29","-O2","-std=gnu11",
                        "-Wall","-Werror","-Iandroid/runtime","-Iandroid/host","-Igenerated",str(OUT/"probe.c"),
                        "android/host/bioshock_inflate.c","-lz","-Wl,-z,max-page-size=16384","-o",str(binary)],cwd=ROOT,check=True)
        serial = os.environ.get("BIOSHOCK_TEST_ANDROID")
        if serial:
            adb = ["adb","-s",serial]
            for path in (binary,OUT/"vectors.bin"): subprocess.run([*adb,"push",str(path),"/data/local/tmp/"+path.name],check=True,capture_output=True)
            subprocess.run([*adb,"shell","chmod","755","/data/local/tmp/"+binary.name],check=True)
            subprocess.run([*adb,"shell","/data/local/tmp/"+binary.name,"/data/local/tmp/vectors.bin"],check=True)
        print(f"PASS original x86 zlib oracle: {sum(x[3] for x in cases)} valid blocks; ARM64 fixture built ({len(cases)} vectors)")

    def test_checked_lifter(self):
        subprocess.run([sys.executable,"-B","-c","from tests.test_bioshock_inflate import NativeInflateTest; NativeInflateTest('test_checked_lifter')._check_lifter()"],cwd=ROOT,check=True)

    def _check_lifter(self):
        sys.path.insert(0,str(ROOT/"tools"))
        from lift_game import CheckedLifter
        from disasm.disasm32 import Instruction
        from capstone import Cs, CS_ARCH_X86, CS_MODE_32
        decoder = Cs(CS_ARCH_X86,CS_MODE_32); decoder.detail = True
        def decode(code,address=0x10922AC0):
            i=next(decoder.disasm(bytes.fromhex(code),address))
            return Instruction(i.address,i.size,i.mnemonic,i.op_str,i.bytes,i.operands)
        pe=pefile.PE(str(ROOT/"generated/input/Bioshock.exe"),fast_load=True)
        self.assertEqual(pe.get_data(0x10922AC0-pe.OPTIONAL_HEADER.ImageBase,3),bytes.fromhex("83ec38"))
        emission=CheckedLifter().lift_instruction(decode("83ec38"))
        self.assertIn("void_bioshock_uncompress",emission[0])
        source=(ROOT/"generated/bioshock/functions_0013.c").read_text(encoding="utf-8")
        self.assertEqual(source.count("\n".join("    "+line for line in emission)),1)
        with self.assertRaisesRegex(ValueError,"inflate entry"): CheckedLifter().lift_instruction(decode("83ec3c"))
        self.assertNotIn("void_bioshock_uncompress","\n".join(CheckedLifter().lift_instruction(decode("83ec38",0x10922AC1))))


if __name__ == "__main__":
    unittest.main()

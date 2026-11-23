"""Run the production module lifecycle with dependent DLL and reentrant exit callbacks."""
from pathlib import Path
import subprocess
import tempfile
import unittest

from tests.host_environment import CLANG, environment
from tests.test_mobile_shaders import function

ROOT = Path(__file__).resolve().parents[1]


class ModuleLifecycleTest(unittest.TestCase):
    def test_dependency_order_reload_and_reentrant_free(self):
        source = (ROOT / "android/host/pe.c").read_text(encoding="utf-8")
        declarations = source[source.index("#define DLL_PROCESS_DETACH_"):source.index("static pthread_mutex_t loader_lock")]
        production = "\n".join(function(source, signature) for signature in (
            "static void_module *find_by_handle(uint32_t handle)\n{",
            "static uint32_t dll_event(void_module *m, uint32_t reason, uint32_t reserved)\n{",
            "void void_module_prepare_call(uint32_t va)\n{",
            "static uint32_t module_acquire(void_module *m)\n{",
            "static int module_release(void_module *m, uint32_t reserved, int release_reference)\n{",
            "int void_module_free(uint32_t handle)\n{",
            "void void_modules_detach_all(uint32_t reserved)\n{",
        ))
        program = r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef struct {uint32_t words[32];} void_regs;
typedef struct void_module void_module;
static unsigned char memory[0x10000],pristine[5][256];
static uint32_t g_fs_base=0x100;
static void_regs live_regs;
#define GPTR(address) (memory+(address))
#define MEM32(address) (*(uint32_t *)(memory+(address)))
#define ERROR_INVALID_HANDLE_ 6
static void void_regs_save(void_regs *out){memcpy(out,&live_regs,sizeof live_regs);}
static void void_regs_load(const void_regs *in){memcpy(&live_regs,in,sizeof live_regs);}
static void void_log(const char *format,...){(void)format;}
static void void_set_last_error(uint32_t error){assert(error==ERROR_INVALID_HANDLE_);}
static _Noreturn void void_fatal(const char *format,...){(void)format;abort();}
static _Noreturn void void_lift_trap(uint32_t address,const char *message){(void)address;(void)message;abort();}
static void void_tls_callbacks(uint32_t base,uint32_t reason);
static uint32_t void_call_guest(uint32_t entry,int nargs,const uint32_t *args,int pop);
/* DECLARATIONS */
static int module_release(void_module *m,uint32_t reserved,int release_reference);
static uint32_t module_acquire(void_module *m);
/* PRODUCTION */

enum {PLUGIN,CRT,D3DX,DYNAMIC,PSEUDO};
static unsigned detaches[32],detach_count,tls_attaches,tls_detaches;
static int terminating,reentrant,fail_attach=-1;
static void void_tls_callbacks(uint32_t base,uint32_t reason){
    assert(find_by_handle(base));if(reason==DLL_PROCESS_ATTACH_)++tls_attaches;else ++tls_detaches;
}
static uint32_t void_call_guest(uint32_t entry,int nargs,const uint32_t *args,int pop){
    assert(nargs==3 && pop==1);void_module *m=find_by_handle(args[0]);assert(m && entry==m->entry);
    unsigned index=(unsigned)(m-modules);assert(MEM32(g_fs_base)==0xAABBCCDD);
    memset(&live_regs,0xA5,sizeof live_regs);
    if(args[1]==DLL_PROCESS_ATTACH_){
        assert(m->state==ST_INITIALIZING);
        if((int)index==fail_attach)return 0;
    }else{
        assert(args[1]==DLL_PROCESS_DETACH_ && m->state==ST_DETACHING);
        assert(args[2]==(terminating && !(reentrant && index==DYNAMIC)?1u:0u));
        assert(detach_count<32);detaches[detach_count++]=index;
        assert(!void_module_free(m->base)); // Reentrant self-release must not recurse or retire held dependencies.
        if(index!=CRT){
            assert(modules[CRT].state==ST_READY);
            unsigned before=modules[CRT].attach_events;
            void_module_prepare_call(modules[CRT].base+32);
            assert(modules[CRT].state==ST_READY && modules[CRT].attach_events==before);
        }
        if(index==D3DX && reentrant){
            assert(modules[DYNAMIC].state==ST_READY);
            assert(void_module_free(modules[DYNAMIC].base));
            assert(modules[DYNAMIC].state==ST_DETACHED && modules[CRT].state==ST_READY);
        }
    }
    return 1;
}
static void setup(void){
    memset(modules,0,sizeof modules);memset(memory,0,sizeof memory);memset(pristine,0x33,sizeof pristine);
    module_count=5;attach_serial=0;detach_count=tls_attaches=tls_detaches=0;terminating=reentrant=0;fail_attach=-1;
    MEM32(g_fs_base)=0xAABBCCDD;
    for(int i=0;i<5;++i){
        void_module *m=&modules[i];snprintf(m->name,sizeof m->name,"module-%d",i);
        m->kind=i==PSEUDO?MOD_PSEUDO:MOD_LIFTED_DLL;m->state=i==PSEUDO?ST_READY:ST_MAPPED;
        m->base=0x1000+(uint32_t)i*0x1000;m->size=sizeof pristine[i];m->entry=m->base+16;m->pristine=pristine[i];
    }
    // Deliberately non-topological config order; the real CRT imports its own msvcrt alias.
    modules[CRT].deps[0]=CRT;modules[CRT].dep_count=1;
    modules[D3DX].deps[0]=CRT;modules[D3DX].dep_count=1;
    modules[DYNAMIC].deps[0]=CRT;modules[DYNAMIC].dep_count=1;
    modules[PLUGIN].deps[0]=D3DX;modules[PLUGIN].dep_count=1;
}
static void finished(void){
    for(int i=0;i<4;++i){assert(modules[i].state==(modules[i].attach_events?ST_DETACHED:ST_MAPPED) && !modules[i].refs && !modules[i].holds_deps && !modules[i].attach_order);}
    assert(modules[PSEUDO].state==ST_READY);
    unsigned calls=detach_count;void_modules_detach_all(1);assert(detach_count==calls);
}
int main(void){
    setup();assert(module_acquire(&modules[DYNAMIC]));assert(module_acquire(&modules[PLUGIN]));
    assert(modules[CRT].attach_order<modules[DYNAMIC].attach_order && modules[DYNAMIC].attach_order<modules[D3DX].attach_order && modules[D3DX].attach_order<modules[PLUGIN].attach_order);
    terminating=reentrant=1;void_modules_detach_all(1);
    assert(detach_count==4 && detaches[0]==PLUGIN && detaches[1]==D3DX && detaches[2]==DYNAMIC && detaches[3]==CRT);
    for(int i=0;i<4;++i)assert(modules[i].attach_events==1 && modules[i].detach_events==1);
    assert(tls_attaches==4 && tls_detaches==4);finished();

    setup();assert(module_acquire(&modules[D3DX]));uint64_t first=modules[D3DX].attach_order;
    assert(void_module_free(modules[D3DX].base));assert(modules[D3DX].state==ST_DETACHED && !modules[D3DX].attach_order);
    memset(GPTR(modules[D3DX].base),0xA5,modules[D3DX].size);
    void_regs expected;memset(&expected,0x3C,sizeof expected);live_regs=expected;
    void_module_prepare_call(modules[D3DX].base+32);
    assert(!memcmp(&live_regs,&expected,sizeof expected));
    assert(!memcmp(GPTR(modules[D3DX].base),pristine[D3DX],modules[D3DX].size));
    assert(modules[D3DX].attach_order>first && modules[D3DX].attach_events==2 && modules[CRT].attach_events==1);
    assert(module_acquire(&modules[DYNAMIC]));terminating=1;void_modules_detach_all(1);
    assert(detach_count==4 && detaches[0]==D3DX && detaches[1]==DYNAMIC && detaches[2]==D3DX && detaches[3]==CRT);
    assert(modules[D3DX].detach_events==2 && modules[CRT].attach_events==1);finished();

    setup();fail_attach=D3DX;assert(!module_acquire(&modules[D3DX]));assert(!modules[D3DX].attach_order);
    terminating=1;void_modules_detach_all(1);assert(detach_count==1 && detaches[0]==CRT);
    puts("PASS production DLL lifecycle: actual reverse attach order, CRT self-import, dependent-to-CRT call without reattach, reentrant FreeLibrary, reload state, failed attach, repeated detach");
    return 0;
}
'''.replace("/* DECLARATIONS */", declarations).replace("/* PRODUCTION */", production)
        with tempfile.TemporaryDirectory(prefix="bioshock-module-lifecycle-") as temporary:
            folder = Path(temporary)
            probe = folder / "check.c"
            probe.write_text(program, encoding="utf-8")
            env, linker = environment("x64")
            subprocess.run([str(CLANG), "/nologo", "/O2", "/std:c11", "/D_CRT_SECURE_NO_WARNINGS",
                            "/clang:--target=x86_64-pc-windows-msvc", "/c", str(probe), "/Fo" + str(folder / "check.obj")],
                           env=env, check=True)
            subprocess.run([str(linker), "/nologo", str(folder / "check.obj"), "/OUT:" + str(folder / "check.exe")], env=env, check=True)
            subprocess.run([str(folder / "check.exe")], env=env, check=True, timeout=30)
        subprocess.run([str(CLANG.with_name("clang.exe")), "--target=aarch64-linux-android29", "-std=gnu11",
                        "-Wall", "-Wextra", "-Werror", "-Wno-unused-function", "-fno-strict-aliasing", "-fwrapv",
                        "-DRECOMP_X87_WINDOW", "-DUT3_ANDROID_APP", "-Iandroid/runtime", "-Iandroid/host", "-Igenerated",
                        "-fsyntax-only", "android/host/pe.c"], cwd=ROOT, check=True)


if __name__ == "__main__":
    unittest.main()

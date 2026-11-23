"""UT3 call-site cache against the production dispatch replacement policy."""
from pathlib import Path
import subprocess
import unittest

from tests.host_environment import CLANG, environment

ROOT = Path(__file__).resolve().parents[1]


def method(source, signature):
    """The C function of `source` that starts at `signature`, braces balanced."""
    start = source.index(signature)
    opening = source.index("{", start)
    depth, end = 1, opening + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end]


class CallsiteCacheTest(unittest.TestCase):
    def test_production_resolution_and_invalidation(self):
        core = (ROOT / "android/host/core.c").read_text(encoding="utf-8")
        header = (ROOT / "android/runtime/recomp_types.h").read_text(encoding="utf-8")
        begin = header.index("typedef struct recomp_icache {")
        end = header.index("/* The dummy return address", begin)
        types = header[begin:end]
        dispatch = "\n".join(method(core, signature) for signature in (
            "static inline uint32_t hash_va(", "static void table_grow(",
            "void void_dispatch_add(", "void_fn void_dispatch_find("))
        source = (PREAMBLE + types + DISPATCH_PREAMBLE + dispatch + RESOLVER +
                  (ROOT / "android/host/callsite_cache.inc").read_text(encoding="utf-8") + CHECK)
        out = ROOT / "build/tests/callsite-cache"
        out.mkdir(parents=True, exist_ok=True)
        probe, obj, binary = out / "probe.c", out / "probe.obj", out / "probe.exe"
        probe.write_text(source, encoding="utf-8")
        env, linker = environment("x64")
        for command in ([str(CLANG), "/nologo", "/O2", "/W4", "/WX", "/std:c11",
                         "/D_CRT_SECURE_NO_WARNINGS", "/clang:--target=x86_64-pc-windows-msvc",
                         "/c", str(probe), "/Fo" + str(obj)],
                        [str(linker), "/nologo", str(obj), "/OUT:" + str(binary)], [str(binary)]):
            run = subprocess.run(command, cwd=ROOT, env=env, capture_output=True, text=True, timeout=60)
            self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
        self.assertEqual(header.count("recomp_func_t _fn = RECOMP_SITE_RESOLVE(_va);"), 6)


PREAMBLE = r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <setjmp.h>
typedef void (*void_fn)(void);
typedef void_fn recomp_func_t;
static int fail_allocation;
static void *fixture_calloc(size_t n,size_t size) { return fail_allocation ? NULL : calloc(n,size); }
static void *fixture_realloc(void *p,size_t size) { return fail_allocation ? NULL : realloc(p,size); }
#define calloc fixture_calloc
#define realloc fixture_realloc
#define void_fatal(...) abort()
recomp_func_t void_resolve(uint32_t va);
'''
DISPATCH_PREAMBLE = r'''
typedef struct { uint32_t va;void_fn fn; } slot_t;
static slot_t *table;
static uint32_t table_mask,table_used;
static struct { uint32_t va;void_fn fn; } resolve_cache[1u<<14];
static uint32_t resolve_slot(uint32_t va) { return (va*0x9E3779B1u)>>(32-14); }
static void icache_flush(void);
void void_dispatch_add(uint32_t va,void_fn fn);
'''
RESOLVER = r'''
#define TARGETS 1024u
#define VA(i) (0xF0000000u+16u*(uint32_t)(i))
static int count_thunks;
static uint64_t slow_calls,diagnostics,calls;
static jmp_buf trap;
static int trapping;
static int resolve_diagnostic_va(uint32_t va) { return va==VA(999); }
recomp_func_t void_resolve(uint32_t va) {
    ++slow_calls;if(resolve_diagnostic_va(va))++diagnostics;
    void_fn fn=void_dispatch_find(va);
    if(!fn) { assert(trapping);longjmp(trap,1); }
    return fn;
}
'''
CHECK = r'''
static void_fn expected[TARGETS];
static uint32_t rng=12345;
static uint32_t next(void) { rng^=rng<<13;rng^=rng>>17;rng^=rng<<5;return rng; }
static void reregister(uint32_t i,uint32_t generation) {
    expected[i]=(void_fn)(uintptr_t)(0x10000000u+64u*i+generation);
    void_dispatch_add(VA(i),expected[i]);
}
static void call(recomp_icache_t *site,uint32_t i) {
    uint32_t va=VA(i);++calls;
    recomp_func_t fn=site->key && site->key==~va ? site->fn : void_resolve_site(site,va);
    assert(fn==expected[i]);
}
static recomp_icache_t many[70000];
static void invalid(uint32_t va) { (void)RECOMP_SITE_RESOLVE(va);assert(0); }
int main(void) {
    for(uint32_t i=0;i<TARGETS;++i)reregister(i,0);
    static recomp_icache_t mono,two,five,six,mega,phases;
    for(unsigned round=0;round<3;++round) {
        for(int k=0;k<4000;++k)call(&mono,7);
        for(int k=0;k<4000;++k)call(&two,10+(k&1));
        for(int k=0;k<4000;++k)call(&five,20+k%5);
        for(int k=0;k<4000;++k)call(&six,30+k%6);
        for(int k=0;k<20000;++k)call(&mega,100+next()%300);
        for(int k=0;k<4000;++k)call(&phases,500+k/1000);
        reregister(7,round+1);reregister(11,round+1);reregister(22,round+1);
        reregister(33,round+1);reregister(150+round,round+1);reregister(501,round+1);
        call(&mono,7);call(&two,11);call(&five,22);call(&six,33);call(&mega,150+round);call(&phases,501);
    }
    assert(site_big_used && site_ext_used);
    for(uint32_t s=0;s<70000;++s){call(&many[s],600+s%7);call(&many[s],610+s%5);}
    for(int pass=0;pass<3;++pass)
        for(uint32_t s=0;s<70000;++s)call(&many[s],(s+pass)&1 ? 600+s%7 : 610+s%5);
    reregister(600,9);reregister(610,9);
    for(uint32_t s=0;s<70000;++s){call(&many[s],600+s%7);call(&many[s],610+s%5);}
    static recomp_icache_t diag,counted,allocation;
    uint64_t before=diagnostics;
    for(unsigned i=0;i<30;++i)call(&diag,999);
    assert(diagnostics==before+30 && !diag.key && !diag.fills);
    count_thunks=1;before=slow_calls;
    for(unsigned i=0;i<30;++i)call(&counted,7);
    assert(slow_calls==before+30 && !counted.key && !counted.fills);count_thunks=0;
    icache_capacity=icache_count;fail_allocation=1;
    call(&allocation,7);assert(!allocation.key && !allocation.fills);fail_allocation=0;
    trapping=1;
    if(!setjmp(trap))invalid(0);
    if(!setjmp(trap))invalid(UINT32_MAX);
    if(!setjmp(trap)) { (void)void_resolve_site(&mega,UINT32_MAX);assert(0); }
    printf("PASS production call-site cache: %llu resolutions, invalidation/promotion/pool exhaustion, diagnostics/counting, allocation failure and invalid targets\n",(unsigned long long)calls);
    return 0;
}
'''


if __name__ == "__main__":
    unittest.main()

"""x87 stack slots as C locals (tools/x87_locals.py) versus the window model.

For BioShock's hottest x87 bodies (game math, Havok, FMOD/MPEG, priority
sorting) plus their direct callees, two copies are compiled from the current
generated sources: the window model exactly as generated (renamed *_w) and the
rewritten body the production build uses (checked build, RECOMP_PROTOCOL_CHECK).
On the user-selected ARM64 device both run on identical random guest states:
pointer-rich guest memory with ordinary and special floats, the executable's
real .rdata/.data images, random registers, stack arguments, flags and FPU
control word, and random values below the entry depth for bodies that consume
x87 arguments. A run that leaves the tested set (an unlisted callee, an
indirect call) or faults is skipped for both copies before comparison.

Compared: every byte of the guest window and image data, EAX..EBP, ESP, the
published flags, the x87 compare state and control word, MXCSR, XMM, MMX, the
window position and every live window slot with its integer shadow, and zero
protocol violations. Only words that are NaN in both runs may differ in their
payload (the compiler may commute a NaN-propagating operation). A second run
draws no NaN test inputs; the image's own data still holds NaN patterns (-1
sentinels read as floats), so the same rule applies there.
"""
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
SERIAL = os.environ.get("BIOSHOCK_TEST_ANDROID")   # the explicitly selected device; no default (shared phones)
sys.path.insert(0, str(ROOT / "tools"))

TARGETS = ["sub_10A10220", "sub_109591E0", "sub_10F32E30", "sub_10F33930", "sub_10BF2960", "sub_10BF2ED0",
           "sub_10EBA2C0", "sub_10E1E8D0", "sub_10E20E00", "sub_10E21F60", "sub_10914130", "sub_10B66DC0",
           "sub_10F37470", "sub_10981BA0", "sub_10D6B7D0", "sub_10ED80D0", "sub_10EE94F0", "sub_10E986A1",
           "sub_10E9C0DA", "sub_10F33100", "sub_10B69790",
           # Bodies the trap-line and deeper call-result search opened up (Havok's solver first).
           "sub_10EE49E0", "sub_10ED3900", "sub_10A8B5D0", "sub_10EFECF0", "sub_10A17D80", "sub_10F0E490",
           "sub_10DBFB80"]
# Globals a body dereferences that the image leaves zero (runtime-initialized):
# point them into the random guest window so the body runs.
POINTER_GLOBALS = {"sub_10EBA2C0": [0x11308FC8]}
# Stack arguments (1-based dword index above the return address) that are
# small integers: the MPEG synthesis offset and the output stride.
SMALL_ARGS = {"sub_10EBA2C0": (2, 3)}

FUNC_RE = re.compile(rb"^void (sub_[0-9A-F]{8})\(void\) \{\r?$", re.M)
CALL_RE = re.compile(rb"RECOMP_CALL\((sub_[0-9A-F]{8})\)")


def index_functions():
    import x87_locals
    where = {}
    for module in x87_locals.manifest():
        for source in module["sources"]:
            text = source.read_bytes()
            for match in FUNC_RE.finditer(text):
                where[match[1].decode()] = source
    return where


def body_of(text, name):
    start = text.index(b"void %s(void) {" % name.encode())
    end = re.compile(rb"\n\}\r?\n").search(text, start).end()
    return text[start:end]


HARNESS = r'''
#define _GNU_SOURCE
#include "recomp_types.h"
#include <setjmp.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/time.h>
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "CHECK failed line %d: %s\n", __LINE__, #x); exit(3); } } while (0)
uint32_t g_eax, g_ecx, g_edx, g_esp, g_ebx, g_esi, g_edi, g_ebp, g_flag_k, g_flag_a, g_flag_b, g_flag_cf;
uint32_t g_cur_func, g_backedges, g_fs_base, g_mxcsr = 0x1F80;
uint64_t g_mm[8], g_fp_round_mask = ~0ull, g_fp_store_mask = ~0ull;
xmm_t g_xmm[8];
double g_st_win[RECOMP_X87_SLOTS];
int64_t g_st_i64_win[RECOMP_X87_SLOTS];
int g_fp_sp = RECOMP_X87_SLOTS / 2, g_fp_top, void_fpu_cmp = 1;
uint16_t g_fpu_cw = 0x027F, g_seg_cs, g_seg_ds, g_seg_es, g_seg_fs, g_seg_gs, g_seg_ss;
void (*recomp_yield_hook)(void), (*recomp_spin_hook)(void);
static sigjmp_buf skip_jump;
static volatile int in_body, violations;
static void skip_trial(void) { if (in_body) siglongjmp(skip_jump, 1); abort(); }
void void_lift_trap(uint32_t address, const char *text) { (void)address; (void)text; skip_trial(); }
void void_bioshock_audio_return(uint32_t caller, uint32_t value) { (void)caller; (void)value; }
void recomp_x87_violation(uint32_t va, int found, int expected) {
    fprintf(stderr, "x87 violation %08X net %d expected %d\n", va, found, expected); ++violations; }
static void outside(void) { skip_trial(); }
recomp_func_t void_resolve(uint32_t va);
recomp_func_t void_resolve_site(recomp_icache_t *site, uint32_t va) { (void)site; return void_resolve(va); }
recomp_func_t void_resolve(uint32_t va) { (void)va; return outside; }
static void on_fault(int sig, siginfo_t *info, void *context) { (void)sig; (void)info; (void)context; skip_trial(); }
static void arm(long microseconds) {
    struct itimerval timer = { { 0, 0 }, { microseconds / 1000000, microseconds % 1000000 } };
    setitimer(ITIMER_REAL, &timer, NULL); }
@STUBS@
typedef void (*body_t)(void);
typedef struct { const char *name; body_t window, locals; int c, p; uint32_t pointer_global; int bystander; uint32_t small_args; } target_t;
@TARGETS@

#define LO 0x00100000u
#define HI 0x00200000u
#define STACK 0x001F0000u
#define IMAGE_LO @IMAGE_LO@u
#define IMAGE_HI @IMAGE_HI@u
static uint64_t rng = 88172645463325252ull;
static uint32_t next(void) { rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17; return (uint32_t)rng; }
static int no_nan;
static uint32_t special_bits(void) {
    switch (next() % 12) {
    case 0: return 0; case 1: return 0x80000000u;
    case 2: return next() & 0x807FFFFFu;                                   /* denormal */
    case 3: return 0x7F800000u | (next() & 0x80000000u);                   /* infinity */
    case 4: return no_nan ? 0x3FC00000u : 0x7FC00000u | (next() & 0x803FFFFFu);
    case 5: return no_nan ? 0xBF400000u : 0x7F800001u | (next() & 0x803FFFFEu);
    default: { float f = ((float)(int32_t)next() / 2147483648.0f) * 1000.0f; uint32_t b; memcpy(&b, &f, 4); return b; }
    }
}
static uint32_t random_word(int heavy) {
    uint32_t pick = next() % 10;
    if (pick < 4) return LO + ((next() % (HI - LO - 0x1000)) & ~3u);          /* a pointer into the window */
    if (pick < 5) return next() % 64;                                       /* a small integer */
    if (heavy && pick < 7) return special_bits();
    float f = ((float)(int32_t)next() / 2147483648.0f) * (pick == 9 ? 4096.0f : 4.0f);
    uint32_t b; memcpy(&b, &f, 4); return b;
}
static double random_double(int heavy) {
    if (heavy && next() % 4 == 0) { uint32_t b = special_bits(); float f; memcpy(&f, &b, 4); return (double)f; }
    return ((double)(int32_t)next() / 2147483648.0) * 100.0;
}
typedef struct { uint32_t r[8], k, a, b, cf, mxcsr; int cmp, sp, top; uint16_t cw; xmm_t xmm[8]; uint64_t mm[8]; } regs_t;
static regs_t snap(void) {
    regs_t s = { { g_eax, g_ecx, g_edx, g_ebx, g_esp, g_ebp, g_esi, g_edi }, g_flag_k, g_flag_a, g_flag_b, g_flag_cf,
                 g_mxcsr, void_fpu_cmp, g_fp_sp, g_fp_top, g_fpu_cw };
    memcpy(s.xmm, g_xmm, sizeof s.xmm); memcpy(s.mm, g_mm, sizeof s.mm); return s; }
static void load(const regs_t *s) {
    g_eax = s->r[0]; g_ecx = s->r[1]; g_edx = s->r[2]; g_ebx = s->r[3]; g_esp = s->r[4]; g_ebp = s->r[5]; g_esi = s->r[6]; g_edi = s->r[7];
    g_flag_k = s->k; g_flag_a = s->a; g_flag_b = s->b; g_flag_cf = s->cf; g_mxcsr = s->mxcsr; void_fpu_cmp = s->cmp;
    g_fp_sp = s->sp; g_fp_top = s->top; g_fpu_cw = s->cw; memcpy(g_xmm, s->xmm, sizeof g_xmm); memcpy(g_mm, s->mm, sizeof g_mm); }
static int nan32(uint32_t v) { return (v & 0x7F800000u) == 0x7F800000u && (v & 0x007FFFFFu); }
static int nan64(uint64_t v) { return (v & 0x7FF0000000000000ull) == 0x7FF0000000000000ull && (v & 0x000FFFFFFFFFFFFFull); }

int main(int argc, char **argv) {
    unsigned trials = argc > 1 ? (unsigned)atoi(argv[1]) : 300;
    no_nan = argc > 2 && !strcmp(argv[2], "nonan");
    CHECK(mmap((void *)(VOID_GUEST_BASE + LO), HI - LO, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0) == (void *)(VOID_GUEST_BASE + LO));
    CHECK(mmap((void *)(VOID_GUEST_BASE + IMAGE_LO), IMAGE_HI - IMAGE_LO, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0) == (void *)(VOID_GUEST_BASE + IMAGE_LO));
    FILE *image = fopen(argv[3], "rb");
    CHECK(image && fread(GPTR(IMAGE_LO), 1, IMAGE_HI - IMAGE_LO, image) == IMAGE_HI - IMAGE_LO);
    fclose(image);
    size_t window = HI - LO, data = IMAGE_HI - IMAGE_LO;
    uint8_t *pristine = malloc(data), *start_window = malloc(window), *start_data = malloc(data);
    uint8_t *want_window = malloc(window), *want_data = malloc(data);
    CHECK(pristine && start_window && start_data && want_window && want_data);
    memcpy(pristine, GPTR(IMAGE_LO), data);
    struct sigaction action = { 0 };
    action.sa_sigaction = on_fault; action.sa_flags = SA_SIGINFO | SA_NODEFER;
    sigaction(SIGSEGV, &action, NULL); sigaction(SIGBUS, &action, NULL); sigaction(SIGALRM, &action, NULL);
    unsigned total_compared = 0, nan_words = 0, weak = 0;
    for (unsigned t = 0; t < sizeof targets / sizeof targets[0]; ++t) {
        const target_t *target = &targets[t];
        unsigned compared = 0, skipped = 0, nan_before = nan_words;
        for (unsigned trial = 0; trial < trials; ++trial) {
            int heavy = trial % 4 == 0;
            memcpy(GPTR(IMAGE_LO), pristine, data);
            for (uint32_t va = LO; va < HI; va += 4) MEM32(va) = random_word(heavy);
            if (target->pointer_global) MEM32(target->pointer_global) = LO + ((next() % (HI - LO - 0x10000)) & ~15u);
            regs_t start;
            memset(&start, 0, sizeof start);
            /* Registers and stack arguments are mostly valid pointers. */
            for (int r = 0; r < 8; ++r) start.r[r] = next() % 100 < 85 ? LO + ((next() % (HI - LO - 0x20000)) & ~3u) : random_word(heavy);
            start.r[4] = STACK - 0x100;                             /* esp */
            for (uint32_t i = 1; i <= 16; ++i) MEM32(start.r[4] + 4 * i) = next() % 100 < 85 ? LO + ((next() % (HI - LO - 0x20000)) & ~3u) : random_word(heavy);
            for (uint32_t i = 1; i <= 16; ++i) if (target->small_args & (1u << i)) MEM32(start.r[4] + 4 * i) = 1 + next() % 15;
            MEM32(start.r[4]) = RECOMP_RETADDR;
            start.k = next() % 8; start.a = next(); start.b = next(); start.cf = next() & 1;
            start.mxcsr = 0x1F80; start.cmp = (int)(next() % 3) - 1; start.cw = trial % 3 ? 0x027F : 0x037F;
            start.sp = RECOMP_X87_SLOTS / 2; start.top = 0;
            for (int x = 0; x < 8; ++x) for (int l = 0; l < 4; ++l) start.xmm[x].u32[l] = random_word(heavy);
            for (int m = 0; m < 8; ++m) start.mm[m] = ((uint64_t)random_word(heavy) << 32) | random_word(heavy);
            /* x87 arguments: c values below the entry depth, plus one bystander. */
            double args[4]; int64_t shadows[4];
            for (int a = 0; a < 4; ++a) { args[a] = random_double(heavy); shadows[a] = a == 0 && trial % 5 == 0 ? (int64_t)next() : 0; }
            memset(g_st_win, 0, sizeof g_st_win); memset(g_st_i64_win, 0, sizeof g_st_i64_win);
            load(&start);
            /* A value of the caller below the arguments, when the body's depth leaves room
             * for it on the eight registers (MSVC calls with an empty stack). */
            for (int a = target->bystander ? 0 : 1; a <= target->c; ++a) { fp_push(args[a]); g_st_i64[0] = shadows[a]; }
            start = snap();
            double start_st[RECOMP_X87_SLOTS]; int64_t start_i64[RECOMP_X87_SLOTS];
            memcpy(start_st, g_st_win, sizeof start_st); memcpy(start_i64, g_st_i64_win, sizeof start_i64);
            memcpy(start_window, GPTR(LO), window); memcpy(start_data, GPTR(IMAGE_LO), data);
            in_body = 1;
            if (sigsetjmp(skip_jump, 1)) { arm(0); in_body = 0; ++skipped; continue; }
            /* A loop count drawn from random memory can run for minutes: a body that
             * does not return within the budget is skipped like a fault. */
            arm(250000);
            target->window();
            arm(0);
            in_body = 0;
            regs_t want = snap();
            double want_st[RECOMP_X87_SLOTS]; int64_t want_i64[RECOMP_X87_SLOTS];
            memcpy(want_st, g_st_win, sizeof want_st); memcpy(want_i64, g_st_i64_win, sizeof want_i64);
            memcpy(want_window, GPTR(LO), window); memcpy(want_data, GPTR(IMAGE_LO), data);
            memcpy(GPTR(LO), start_window, window); memcpy(GPTR(IMAGE_LO), start_data, data);
            memcpy(g_st_win, start_st, sizeof start_st); memcpy(g_st_i64_win, start_i64, sizeof start_i64);
            load(&start);
            in_body = 1;
            if (sigsetjmp(skip_jump, 1)) { fprintf(stderr, "%s trial %u: locals faulted or left the set, window did not\n", target->name, trial); return 5; }
            arm(1000000);
            target->locals();
            arm(0);
            in_body = 0;
            regs_t got = snap();
            for (int r = 0; r < 8; ++r)
                if (want.r[r] != got.r[r] && nan32(want.r[r]) && nan32(got.r[r])) { ++nan_words; got.r[r] = want.r[r]; }
            if (memcmp(want.r, got.r, sizeof want.r) || want.k != got.k || want.a != got.a || want.b != got.b || want.cf != got.cf ||
                want.mxcsr != got.mxcsr || want.cmp != got.cmp || want.sp != got.sp || want.top != got.top || want.cw != got.cw ||
                memcmp(want.mm, got.mm, sizeof want.mm)) {
                fprintf(stderr, "%s trial %u: registers differ: eax %08X/%08X ecx %08X/%08X edx %08X/%08X esp %08X/%08X k %u/%u a %08X/%08X b %08X/%08X cmp %d/%d sp %d/%d top %d/%d\n",
                        target->name, trial, want.r[0], got.r[0], want.r[1], got.r[1], want.r[2], got.r[2], want.r[4], got.r[4],
                        want.k, got.k, want.a, got.a, want.b, got.b, want.cmp, got.cmp, want.sp, got.sp, want.top, got.top);
                return 6;
            }
            for (int x = 0; x < 8; ++x) for (int l = 0; l < 4; ++l) {
                uint32_t a = want.xmm[x].u32[l], b = got.xmm[x].u32[l];
                if (a != b && !(nan32(a) && nan32(b))) { fprintf(stderr, "%s trial %u: xmm%d.%d %08X/%08X\n", target->name, trial, x, l, a, b); return 7; }
                if (a != b) ++nan_words;
            }
            /* Live window slots: from the position up to the bystander pushed first. */
            int live = want.top;
            for (int s = 0; s < live && s < 8; ++s) {
                uint64_t a, b; memcpy(&a, &want_st[want.sp + s], 8); memcpy(&b, &g_st_win[got.sp + s], 8);
                if ((a != b && !(nan64(a) && nan64(b))) || want_i64[want.sp + s] != g_st_i64_win[got.sp + s]) {
                    fprintf(stderr, "%s trial %u: st(%d) %016llX/%016llX shadow %lld/%lld\n", target->name, trial, s,
                            (unsigned long long)a, (unsigned long long)b, (long long)want_i64[want.sp + s], (long long)g_st_i64_win[got.sp + s]);
                    return 8;
                }
                if (a != b) ++nan_words;
            }
            for (int pass = 0; pass < 2; ++pass) {
                const uint8_t *want_bytes = pass ? want_data : want_window;
                const uint8_t *got_bytes = pass ? (const uint8_t *)GPTR(IMAGE_LO) : (const uint8_t *)GPTR(LO);
                size_t size = pass ? data : window;
                if (!memcmp(want_bytes, got_bytes, size)) continue;
                for (size_t at = 0; at < size; at += 4) {
                    uint32_t a, b; memcpy(&a, want_bytes + at, 4); memcpy(&b, got_bytes + at, 4);
                    if (a == b) continue;
                    uint64_t a8 = 0, b8 = 0;
                    if (at >= 4) { memcpy(&a8, want_bytes + at - 4, 8); memcpy(&b8, got_bytes + at - 4, 8); }
                    uint64_t a9 = 0, b9 = 0;
                    if (at + 8 <= size) { memcpy(&a9, want_bytes + at, 8); memcpy(&b9, got_bytes + at, 8); }
                    if ((nan32(a) && nan32(b)) || (nan64(a8) && nan64(b8)) || (nan64(a9) && nan64(b9))) {
                        if (no_nan && nan_words < 6)
                            fprintf(stderr, "%s trial %u: NaN in both at %08X: window %08X locals %08X\n", target->name, trial,
                                    (uint32_t)((pass ? IMAGE_LO : LO) + at), a, b);
                        ++nan_words;
                        continue;
                    }
                    fprintf(stderr, "%s trial %u: memory differs at %08X: window %08X locals %08X\n",
                            target->name, trial, (uint32_t)((pass ? IMAGE_LO : LO) + at), a, b);
                    return 9;
                }
            }
            ++compared;
        }
        printf("%s c=%d p=%d: %u compared, %u skipped, %u NaN-in-both words\n", target->name, target->c, target->p,
               compared, skipped, nan_words - nan_before);
        if (compared < trials / 10) ++weak;
        total_compared += compared;
    }
    CHECK(violations == 0);
    /* NaN-free test inputs do not make the image NaN-free (-1 sentinels are NaN
     * as floats), so a NaN may still meet another NaN in either model: only the
     * payload of a word NaN in both may differ, in both modes. */
    printf("PASS x87 locals%s: %u trials identical to the window model except %u words NaN in both; %u targets with under 10%% comparable trials\n",
           no_nan ? " (no NaN test inputs)" : "", total_compared, nan_words, weak);
    return 0;
}
'''


class X87LocalsTest(unittest.TestCase):
    def test_hot_bodies_rewritten(self):
        import x87_locals
        plan = x87_locals.Plan()
        where = index_functions()
        rewritten = 0
        for name in TARGETS:
            text = where[name].read_bytes()
            body = body_of(text, name) + b"\n"
            stats = x87_locals.Plan.__new__(x87_locals.Plan)
            stats.info, stats.stats = plan.info, x87_locals.Counter()
            out = x87_locals.apply_x87(body, stats, only={name})
            rewritten += out != body
            self.assertIn(b"X87_CHECK_ENTER", out if out != body else b"X87_CHECK_ENTER")
        self.assertGreaterEqual(rewritten, len(TARGETS) - 2)

    @unittest.skipUnless(SERIAL, "Set BIOSHOCK_TEST_ANDROID to the explicitly selected device")
    def test_locals_match_window_on_device(self):
        import pefile
        import x87_locals
        plan = x87_locals.Plan()
        where = index_functions()
        closure, frontier = [], list(TARGETS)
        while frontier and len(closure) < 160:
            name = frontier.pop(0)
            if name in closure or name not in where:
                continue
            closure.append(name)
            body = body_of(where[name].read_bytes(), name)
            if len(closure) <= 120:
                frontier += [callee.decode() for callee in CALL_RE.findall(body)]
        window_parts, locals_parts = [], []
        defined = set(closure)
        rename = re.compile(rb"\b(" + b"|".join(n.encode() for n in closure) + rb")\b")
        for name in closure:
            body = body_of(where[name].read_bytes(), name) + b"\n"
            window_parts.append(rename.sub(lambda m: m[1] + b"_w", body))
            stats = x87_locals.Plan.__new__(x87_locals.Plan)
            stats.info, stats.stats = plan.info, x87_locals.Counter()
            locals_parts.append(x87_locals.apply_x87(body, stats, only={name}))
        callees = {c.decode() for part in window_parts + locals_parts for c in CALL_RE.findall(part)}
        external = sorted(c for c in callees if c not in defined and not c.endswith("_w"))
        declarations = "".join(f"void {n}(void);\nvoid {n}_w(void);\n" for n in closure)
        lifted = ('#include "functions.h"\n' + declarations + "\n" +
                  b"\n".join(window_parts).decode() + "\n" + b"\n".join(locals_parts).decode())
        stubs = "".join(f"void {n}(void) {{ outside(); }}\nvoid {n}_w(void) {{ outside(); }}\n" for n in external)
        stubs += "".join(f"void {n}(void);\nvoid {n}_w(void);\n" for n in TARGETS)
        info = plan.info
        rows = []
        for name in TARGETS:
            c = info.get(name, {}).get("c", 0)
            p = info.get(name, {}).get("p", 0)
            pointer = POINTER_GLOBALS.get(name, [0])[0]
            depths = info.get(name, {}).get("depth", {}).values()
            # Only for a leaf: a callee stacks its own depth on top of the caller's.
            body = body_of(where[name].read_bytes(), name)
            leaf = not CALL_RE.search(body) and b"RECOMP_ICALL" not in body
            room = leaf and name in info and 1 + c + max(depths, default=0) <= 8
            small = sum(1 << i for i in SMALL_ARGS.get(name, ()))
            rows.append(f'{{ "{name}", {name}_w, {name}, {c}, {p}, 0x{pointer:08X}u, {int(room)}, 0x{small:X}u }}')
        targets = "static const target_t targets[] = {\n    " + ",\n    ".join(rows) + "\n};\n"
        pe = pefile.PE(str(ROOT / "generated/input/Bioshock.exe"))
        base = pe.OPTIONAL_HEADER.ImageBase
        data_sections = [s for s in pe.sections if s.Name.rstrip(b"\0") in (b".rdata", b".data")]
        lo = base + min(s.VirtualAddress for s in data_sections)
        hi = base + max(s.VirtualAddress + s.Misc_VirtualSize for s in data_sections)
        hi = (hi + 0xFFFF) & ~0xFFFF
        image_bytes = pe.get_memory_mapped_image()[lo - base:hi - base]
        image_bytes += b"\0" * ((hi - lo) - len(image_bytes))
        harness = (HARNESS.replace("@STUBS@", stubs).replace("@TARGETS@", targets)
                   .replace("@IMAGE_LO@", f"0x{lo:08X}").replace("@IMAGE_HI@", f"0x{hi:08X}"))
        sdk = Path(os.environ.get("ANDROID_SDK_ROOT", Path.home() / "AppData/Local/Android/Sdk"))
        clang = sdk / "ndk/27.2.12479018/toolchains/llvm/prebuilt/windows-x86_64/bin/clang.exe"
        adb = [str(sdk / "platform-tools/adb.exe"), "-s", SERIAL]
        common = ["--target=aarch64-linux-android29", "-O2", "-fno-strict-aliasing", "-fwrapv", "-ffp-contract=off",
                  "-fno-math-errno", "-DRECOMP_X87_WINDOW", "-DRECOMP_CALL_DIAG", "-Iandroid/runtime", "-Iandroid/host",
                  "-Igenerated", "-Igenerated/bioshock"]
        generated_flags = common + ["-w", "-DRECOMP_GENERATED_CODE", "-DRECOMP_GUEST_STATE_SEPARATE", "-DRECOMP_PROTOCOL_CHECK",
                                    "-mllvm", "-basic-aa-separate-storage", "-include", "android/runtime/guest_state_prefix.h"]
        with tempfile.TemporaryDirectory(prefix="bioshock-x87-", dir=ROOT / "build") as temporary:
            folder = Path(temporary)
            (folder / "lifted.c").write_text(lifted, encoding="utf-8")
            (folder / "harness.c").write_text(harness, encoding="utf-8")
            (folder / "image.bin").write_bytes(image_bytes)
            objects = []
            for name, flags in (("lifted.c", generated_flags), ("harness.c", common + ["-std=gnu11", "-w"])):
                built = subprocess.run([str(clang), *flags, "-c", str(folder / name), "-o", str(folder / (name + ".o"))],
                                       cwd=ROOT, capture_output=True, text=True, timeout=900)
                self.assertEqual(built.returncode, 0, built.stdout[-4000:] + built.stderr[-4000:])
                objects.append(str(folder / (name + ".o")))
            binary = folder / "test-x87"
            linked = subprocess.run([str(clang), "--target=aarch64-linux-android29", *objects, "-o", str(binary), "-lm"],
                                    cwd=ROOT, capture_output=True, text=True, timeout=300)
            self.assertEqual(linked.returncode, 0, linked.stdout[-4000:] + linked.stderr[-4000:])
            remote = "/data/local/tmp/bioshock-test-x87"
            subprocess.run([*adb, "push", str(binary), remote], check=True, capture_output=True, timeout=120)
            subprocess.run([*adb, "push", str(folder / "image.bin"), remote + ".image"], check=True, capture_output=True, timeout=300)
            result = subprocess.run([*adb, "shell", f"chmod 755 {remote} && {remote} 300 x {remote}.image && "
                                     f"{remote} 300 nonan {remote}.image; echo EXIT=$?"],
                                    capture_output=True, text=True, timeout=3000)
            subprocess.run([*adb, "shell", "rm", "-f", remote, remote + ".image"], capture_output=True, timeout=30)
        output = result.stdout + result.stderr
        print(output)
        self.assertIn("EXIT=0", output)
        self.assertIn("PASS x87 locals:", output)
        self.assertIn("(no NaN test inputs)", output)
        self.assertEqual(output.count("PASS x87 locals"), 2)


if __name__ == "__main__":
    unittest.main()

"""Native CPU skinning (0x10B99DC0) versus the original lifted body, bit for bit.

1. The canonical lifter override at 0x10B99DC0 is the only change: the first
   instruction asks android/host/bioshock_skinning.c to do the whole body and
   falls through to the unchanged lifted body when it declines; the function's
   bytes are pinned and other encodings are refused.
2. On the user-selected ARM64 device, the ORIGINAL lifted sub_10B99DC0 (the
   override line replaced by the generator's ordinary emission) and the
   PRODUCTION lifted sub_10B99DC0 run on identical random guest states: rigid
   and soft vertex counts 0..48, every byte value in the packed vectors,
   weights and bone indices, random matrices, zeros of both signs, denormals,
   infinities, NaNs and magnitudes past 2^31 after packing, random unpack/pack
   vectors and weight constants besides the game's, random MXCSR, and
   output/input/stack overlaps (the native code must decline those). Every
   byte of guest memory outside the callee's dead stack frame, EAX..EDI, ESP,
   the published flags, MXCSR and the x87 stack position must be identical.
   Only a position word that is NaN in both may differ in its payload; with
   NaN-free inputs ("nonan") there must be no such word at all.
"""
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

from tests.test_mobile_shaders import function

ROOT = Path(__file__).resolve().parents[1]
SERIAL = os.environ.get("BIOSHOCK_TEST_ANDROID")
OVERRIDE = ("    { extern int void_bioshock_skin_vertices(void); RECOMP_REGS_OUT(); "
            "if (void_bioshock_skin_vertices()) { RECOMP_REGS_IN(); { RECOMP_REGS_OUT(); return; } } } "
            "PUSH32(esp, ebp); /* 0x10B99DC0: push ebp; CPU skinning, native body (bioshock_skinning.c) */\n")
ORIGINAL = "    PUSH32(esp, ebp); /* 0x10B99DC0: push ebp */\n"
# 0x10B99DC0..0x10B9A44F, the whole function the native code reproduces.
BYTES_SHA256 = "8b709c5f430bfe37429fdcb486eb591b89e8fed1bc04e12884f7e0b79971bfd2"

HARNESS = r'''
#define _GNU_SOURCE
#include "recomp_types.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "CHECK failed line %d: %s\n", __LINE__, #x); exit(3); } } while (0)
uint32_t g_eax, g_ecx, g_edx, g_esp, g_ebx, g_esi, g_edi, g_ebp, g_flag_k, g_flag_a, g_flag_b, g_flag_cf;
uint32_t g_cur_func, g_backedges, g_fs_base, g_mxcsr;
uint64_t g_mm[8], g_fp_round_mask = ~0ull, g_fp_store_mask = ~0ull;
xmm_t g_xmm[8];
double g_st_win[RECOMP_X87_SLOTS];
int64_t g_st_i64_win[RECOMP_X87_SLOTS];
int g_fp_sp = 128, g_fp_top, void_fpu_cmp = 1;
uint16_t g_fpu_cw = 0x027F, g_seg_cs, g_seg_ds, g_seg_es, g_seg_fs, g_seg_gs, g_seg_ss;
void (*recomp_yield_hook)(void), (*recomp_spin_hook)(void);
void void_lift_trap(uint32_t address, const char *text) { fprintf(stderr, "trap %08X %s\n", address, text); exit(4); }
void sub_10B99DC0(void);
void sub_10B99DC0_original(void);

#define LO 0x00100000u
#define HI 0x00900000u
#define BONES 0x00200000u
#define RIGID 0x00300000u
#define SOFT 0x00400000u
#define OUT 0x00500000u
#define STACK_TOP 0x00800000u
static const uint32_t pages[][2] = { { 0x10FD6000u, 0x10FD7000u }, { 0x10FEB000u, 0x10FEC000u }, { 0x112BE000u, 0x112BF000u } };
static uint64_t rng = 88172645463325252ull;
static uint32_t next(void) { rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17; return (uint32_t)rng; }
static int no_nan_inputs;
static float bits_f(uint32_t b) { float f; memcpy(&f, &b, 4); return f; }
static float special(void) {
    switch (next() % 16) {
    case 0: return 0.0f;
    case 1: return -0.0f;
    case 2: return bits_f(next() & 0x807FFFFFu);                                   /* denormal */
    case 3: return bits_f(0x7F800000u | (next() & 0x80000000u));                   /* infinity */
    case 4: if (no_nan_inputs) return 1.5f; return bits_f(0x7FC00000u | (next() & 0x803FFFFFu));   /* qNaN */
    case 5: if (no_nan_inputs) return -0.75f; return bits_f(0x7F800001u | (next() & 0x803FFFFEu)); /* sNaN */
    case 6: return (float)((int32_t)next()) * 1e-38f;
    case 7: return (float)((int32_t)next()) * 64.0f;                                /* packs past 2^31 */
    default: return ((float)(int32_t)next() / 2147483648.0f) * 4.0f;
    }
}
static float plain(void) { return ((float)(int32_t)next() / 2147483648.0f) * 2.0f; }
static void put_f(uint32_t va, float v) { memcpy(GPTR(va), &v, 4); }
typedef struct { uint32_t eax, ecx, edx, ebx, esi, edi, ebp, esp, k, a, b, cf, mxcsr; int sp, top; } regs_t;
static regs_t snap(void) { return (regs_t){ g_eax, g_ecx, g_edx, g_ebx, g_esi, g_edi, g_ebp, g_esp, g_flag_k, g_flag_a, g_flag_b, g_flag_cf, g_mxcsr, g_fp_sp, g_fp_top }; }
static void load(const regs_t *r) { g_eax = r->eax; g_ecx = r->ecx; g_edx = r->edx; g_ebx = r->ebx; g_esi = r->esi; g_edi = r->edi; g_ebp = r->ebp; g_esp = r->esp; g_flag_k = r->k; g_flag_a = r->a; g_flag_b = r->b; g_flag_cf = r->cf; g_mxcsr = r->mxcsr; g_fp_sp = r->sp; g_fp_top = r->top; }

/* Snapshot of every mapped guest byte: the main window and the constant pages. */
static size_t total_size(void) { size_t n = HI - LO; for (unsigned i = 0; i < 3; ++i) n += pages[i][1] - pages[i][0]; return n; }
static void save(uint8_t *to) { memcpy(to, GPTR(LO), HI - LO); to += HI - LO;
    for (unsigned i = 0; i < 3; ++i) { memcpy(to, GPTR(pages[i][0]), pages[i][1] - pages[i][0]); to += pages[i][1] - pages[i][0]; } }
static void restore(const uint8_t *from) { memcpy(GPTR(LO), from, HI - LO); from += HI - LO;
    for (unsigned i = 0; i < 3; ++i) { memcpy(GPTR(pages[i][0]), from, pages[i][1] - pages[i][0]); from += pages[i][1] - pages[i][0]; } }

int main(int argc, char **argv) {
    unsigned trials = argc > 1 ? (unsigned)atoi(argv[1]) : 3000;
    no_nan_inputs = argc > 2 && !strcmp(argv[2], "nonan");
    void *mapped = mmap((void *)(VOID_GUEST_BASE + LO), HI - LO, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
    CHECK(mapped == (void *)(VOID_GUEST_BASE + LO));
    for (unsigned i = 0; i < 3; ++i) {
        void *page = mmap((void *)(VOID_GUEST_BASE + pages[i][0]), pages[i][1] - pages[i][0], PROT_READ | PROT_WRITE,
                          MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
        CHECK(page == (void *)(VOID_GUEST_BASE + pages[i][0]));
    }
    size_t size = total_size();
    uint8_t *before = malloc(size), *lifted = malloc(size), *native = malloc(size);
    CHECK(before && lifted && native);
    unsigned compared = 0, declined = 0, specials_seen = 0, nan_payloads = 0, game_constants = 0, vertices = 0;
    for (unsigned trial = 0; trial < trials; ++trial) {
        memset(GPTR(LO), 0xA5, HI - LO);
        for (unsigned i = 0; i < 3; ++i) memset(GPTR(pages[i][0]), 0x5A, pages[i][1] - pages[i][0]);
        int heavy = trial % 4 == 0;
        /* The game's runtime vectors (0x10F90650/90/D0) and weight constant, or random ones. */
        put_f(0x10FD6520u, 1.0f);
        if (trial % 5) {
            for (unsigned l = 0; l < 4; ++l) {
                put_f(0x112BE110u + 4u * l, l < 3 ? 127.5f : 0.0f);
                put_f(0x112BE120u + 4u * l, l < 3 ? bits_f(0x3C008081u) : 0.0f);
                put_f(0x112BE130u + 4u * l, l < 3 ? 1.0f : 0.0f);
            }
            uint64_t c = 0x3F70101020000000ull; memcpy(GPTR(0x10FEB9C8u), &c, 8);
            ++game_constants;
        } else {
            for (unsigned l = 0; l < 12; ++l) put_f(0x112BE110u + 4u * l, heavy ? special() : plain() * 200.0f);
            double c = heavy ? (double)special() : (double)plain() / 100.0; memcpy(GPTR(0x10FEB9C8u), &c, 8);
        }
        for (uint32_t i = 0; i < 256u * 16u; ++i) put_f(BONES + 4u * i, heavy && next() % 8 == 0 ? special() : plain());
        uint32_t rigid_count = trial % 7 == 0 ? 0 : next() % 49u, soft_count = trial % 11 == 0 ? 0 : next() % 49u;
        for (uint32_t v = 0; v < rigid_count; ++v) {
            uint32_t va = RIGID + 0x24u * v;
            for (uint32_t w = 0; w < 9; ++w) MEM32(va + 4u * w) = next();
            for (uint32_t w = 0; w < 3; ++w) put_f(va + 4u * w, heavy ? special() : plain() * 100.0f);
        }
        for (uint32_t v = 0; v < soft_count; ++v) {
            uint32_t va = SOFT + 0x28u * v;
            for (uint32_t w = 0; w < 10; ++w) MEM32(va + 4u * w) = next();
            for (uint32_t w = 0; w < 3; ++w) put_f(va + 4u * w, heavy ? special() : plain() * 100.0f);
        }
        uint32_t rigid = RIGID, soft = SOFT, bones = BONES, out = OUT;
        switch (next() % 12) {      /* overlaps the native code must decline */
        case 0: out = RIGID + 0x24u * (next() % 8u); break;
        case 1: out = SOFT + 4u * (next() % 16u); break;
        case 2: out = BONES + 0x3F00u; break;
        /* In the callee's own frame, below its arguments (over them the
         * original re-reads a clobbered bones pointer and faults). */
        case 3: out = STACK_TOP - 0x100u - 0x10u - 0x20u * (rigid_count + soft_count + next() % 4u); break;
        case 4: rigid = STACK_TOP - 0x100u - 0x200u; break;
        default: break;
        }
        if (rigid == STACK_TOP - 0x100u - 0x200u)
            for (uint32_t v = 0; v < rigid_count && v < 14; ++v) for (uint32_t w = 0; w < 9; ++w) MEM32(rigid + 0x24u * v + 4u * w) = next() & 0x3F7F7F7Fu;
        if (rigid == STACK_TOP - 0x100u - 0x200u && rigid_count > 14) rigid_count = 14;
        uint32_t esp = STACK_TOP - 0x100;
        MEM32(esp) = RECOMP_RETADDR; MEM32(esp + 4) = rigid; MEM32(esp + 8) = rigid_count; MEM32(esp + 12) = soft;
        MEM32(esp + 16) = soft_count; MEM32(esp + 20) = bones; MEM32(esp + 24) = out;
        uint32_t mxcsr = trial % 3 ? 0x1F80u : (0x1F80u | (next() & 0x6000u) | (next() & 0x3Fu));
        regs_t start = { next(), next(), next(), next(), next(), next(), next(), esp, next() % 8, next(), next(), next() & 1, mxcsr, 128, 0 };
        save(before);
        load(&start); sub_10B99DC0_original(); regs_t want = snap();
        save(lifted);
        restore(before);
        g_backedges = 0;
        load(&start); sub_10B99DC0(); regs_t got = snap();
        save(native);
        /* The lifted loops count back edges; the native body has none. */
        int fell_back = g_backedges != 0;
        int overlap = out != OUT || rigid != RIGID;
        if (!overlap) CHECK(!fell_back);
        if (fell_back) CHECK(overlap);
        if (memcmp(&want, &got, sizeof want)) {
            fprintf(stderr, "trial %u registers differ: eax %08X/%08X ecx %08X/%08X edx %08X/%08X ebx %08X/%08X esp %08X/%08X k %u/%u a %08X/%08X b %08X/%08X cf %u/%u mxcsr %08X/%08X sp %d/%d\n",
                    trial, want.eax, got.eax, want.ecx, got.ecx, want.edx, got.edx, want.ebx, got.ebx, want.esp, got.esp, want.k, got.k,
                    want.a, got.a, want.b, got.b, want.cf, got.cf, want.mxcsr, got.mxcsr, want.sp, got.sp);
            return 5;
        }
        /* An output written over the callee's own frame also clobbers what it
         * restores; both runs are the lifted body then. */
        if (!fell_back)
            CHECK(got.esp == esp + 0x1C && got.ebx == start.ebx && got.esi == start.esi && got.edi == start.edi && got.ebp == start.ebp && got.mxcsr == mxcsr);
        /* The callee's frame below the caller's ESP is dead after `ret 18h`. */
        uint32_t dead_lo = STACK_TOP - 0x1000, dead_hi = esp + 0x1C;
        for (size_t at = 0; at < size; at += 4) {
            uint32_t va = at < HI - LO ? LO + (uint32_t)at : 0;
            if (va && va >= dead_lo && va < dead_hi) continue;
            if (memcmp(lifted + at, native + at, 4)) {
                uint32_t a, b; memcpy(&a, lifted + at, 4); memcpy(&b, native + at, 4);
                /* With two NaN operands ARM64 returns the first operand's NaN
                 * and the compiler may commute fmul/fadd: only the payload or
                 * sign of a position that is NaN in both may differ. Packed
                 * bytes, copied dwords and everything else are identical. */
                int position = !fell_back && va >= out && va < out + 0x20u * (rigid_count + soft_count) && ((va - out) & 0x1Fu) < 12u;
                if (position && (a & 0x7F800000u) == 0x7F800000u && (a & 0x007FFFFFu) &&
                    (b & 0x7F800000u) == 0x7F800000u && (b & 0x007FFFFFu)) { ++nan_payloads; continue; }
                fprintf(stderr, "trial %u memory differs at %08X (offset %zx): lifted %08X native %08X (rigid %u soft %u out %08X)\n",
                        trial, va, at, a, b, rigid_count, soft_count, out);
                return 6;
            }
        }
        if (fell_back) ++declined;
        if (heavy) ++specials_seen;
        vertices += rigid_count + soft_count;
        ++compared;
    }
    if (no_nan_inputs) CHECK(nan_payloads == 0);
    printf("PASS native CPU skinning%s:", no_nan_inputs ? " (no NaN inputs: zero exceptions)" : "");
    printf(" %u trials, %u vertices bit-identical to the original lifted body except %u position words NaN in both"
           " (%u overlapping layouts declined to the lifted body, %u with special values, %u with the game's constants)\n",
           compared, vertices, nan_payloads, declined, specials_seen, game_constants);
    return 0;
}
'''


class SkinningNativeTest(unittest.TestCase):
    def test_checked_override_and_original_bytes(self):
        subprocess.run([sys.executable, "-B", "-c",
                        "from tests.test_skinning_native import SkinningNativeTest; "
                        "SkinningNativeTest('test_checked_override_and_original_bytes')._check_lifter()"],
                       cwd=ROOT, check=True, timeout=60)

    def _check_lifter(self):
        sys.path.insert(0, str(ROOT / "tools"))
        from lift_game import CheckedLifter
        from disasm.disasm32 import Instruction
        import pefile
        from capstone import Cs, CS_ARCH_X86, CS_MODE_32
        config = json.loads((ROOT / "config/inputs.json").read_text(encoding="utf-8"))
        raw = (Path(config["source_root"]) / "Builds/Release/Bioshock.exe").read_bytes()
        self.assertEqual(hashlib.sha256(raw).hexdigest(), "7f2ee821ad60eaf08322b4bc6ebbf839d7fe010b4d0255ec12b5be4a2966f7f6")
        pe = pefile.PE(data=raw)
        code = pe.get_data(0x10B99DC0 - pe.OPTIONAL_HEADER.ImageBase, 0x10B9A44F - 0x10B99DC0)
        self.assertEqual(hashlib.sha256(code).hexdigest(), BYTES_SHA256)
        decoder = Cs(CS_ARCH_X86, CS_MODE_32)
        decoder.detail = True
        def instruction(decoded):
            return Instruction(decoded.address, decoded.size, decoded.mnemonic, decoded.op_str, decoded.bytes, decoded.operands)
        listing = list(decoder.disasm(code, 0x10B99DC0))
        # Self-contained: no call, and the only return is the final `ret 18h`.
        self.assertFalse([d for d in listing if d.mnemonic == "call"])
        self.assertEqual([(d.address, d.op_str) for d in listing if d.mnemonic == "ret"], [(0x10B9A44C, "0x18")])
        emission = CheckedLifter().lift_instruction(instruction(listing[0]))
        self.assertEqual(["    " + line + "\n" for line in emission], [OVERRIDE])
        source = (ROOT / "generated/bioshock/functions_0113.c").read_text(encoding="utf-8")
        self.assertEqual(source.count(OVERRIDE), 1)
        self.assertEqual(source.count("void_bioshock_skin_vertices"), 2)
        for changed in ("54", "53"):
            with self.subTest(changed=changed), self.assertRaisesRegex(ValueError, "CPU skinning entry"):
                CheckedLifter().lift_instruction(instruction(next(decoder.disasm(bytes.fromhex(changed), 0x10B99DC0))))
        other = CheckedLifter().lift_instruction(instruction(next(decoder.disasm(bytes.fromhex("55"), 0x10B99DC1))))
        self.assertNotIn("skin", "\n".join(other))

    @unittest.skipUnless(SERIAL, "Set BIOSHOCK_TEST_ANDROID to the explicitly selected device")
    def test_native_matches_lifted_on_device(self):
        source = (ROOT / "generated/bioshock/functions_0113.c").read_text(encoding="utf-8")
        production = function(source, "void sub_10B99DC0(void)")
        self.assertEqual(production.count(OVERRIDE), 1)
        original = production.replace(OVERRIDE, ORIGINAL).replace("void sub_10B99DC0(void)", "void sub_10B99DC0_original(void)")
        lifted = "\n\n".join(['#include "functions.h"', "void sub_10B99DC0_original(void);", production, original])
        sdk = Path(os.environ.get("ANDROID_SDK_ROOT", Path.home() / "AppData/Local/Android/Sdk"))
        clang = sdk / "ndk/27.2.12479018/toolchains/llvm/prebuilt/windows-x86_64/bin/clang.exe"
        adb = [str(sdk / "platform-tools/adb.exe"), "-s", SERIAL]
        common = ["--target=aarch64-linux-android29", "-O2", "-fno-strict-aliasing", "-fwrapv", "-ffp-contract=off",
                  "-fno-math-errno", "-DRECOMP_X87_WINDOW", "-DRECOMP_CALL_DIAG", "-Iandroid/runtime", "-Iandroid/host",
                  "-Igenerated", "-Igenerated/bioshock"]
        generated_flags = common + ["-w", "-DRECOMP_GENERATED_CODE", "-DRECOMP_GUEST_STATE_SEPARATE", "-mllvm",
                                    "-basic-aa-separate-storage", "-include", "android/runtime/guest_state_prefix.h"]
        with tempfile.TemporaryDirectory(prefix="bioshock-skinning-", dir=ROOT / "build") as temporary:
            folder = Path(temporary)
            (folder / "lifted.c").write_text(lifted, encoding="utf-8")
            (folder / "harness.c").write_text(HARNESS, encoding="utf-8")
            objects = []
            for name, flags in (("lifted.c", generated_flags), ("harness.c", common + ["-std=gnu11", "-w"]),
                                (str(ROOT / "android/host/bioshock_skinning.c"), common + ["-std=gnu11", "-Wall", "-Werror"])):
                obj = folder / (Path(name).stem + ".o")
                built = subprocess.run([str(clang), *flags, "-c", str(folder / name) if not Path(name).is_absolute() else name,
                                        "-o", str(obj)], cwd=ROOT, capture_output=True, text=True, timeout=300)
                self.assertEqual(built.returncode, 0, built.stdout + built.stderr)
                objects.append(str(obj))
            binary = folder / "test-skinning"
            linked = subprocess.run([str(clang), "--target=aarch64-linux-android29", *objects, "-o", str(binary), "-lm"],
                                    cwd=ROOT, capture_output=True, text=True, timeout=120)
            self.assertEqual(linked.returncode, 0, linked.stdout + linked.stderr)
            remote = "/data/local/tmp/bioshock-test-skinning"
            subprocess.run([*adb, "push", str(binary), remote], check=True, capture_output=True, timeout=60)
            result = subprocess.run([*adb, "shell", f"chmod 755 {remote} && {remote} 4000 && {remote} 4000 nonan; echo EXIT=$?"],
                                    capture_output=True, text=True, timeout=600)
            subprocess.run([*adb, "shell", "rm", "-f", remote], capture_output=True, timeout=30)
        output = result.stdout + result.stderr
        print(output)
        self.assertIn("EXIT=0", output)
        self.assertIn("PASS native CPU skinning", output)
        self.assertIn("(no NaN inputs: zero exceptions)", output)


if __name__ == "__main__":
    unittest.main()

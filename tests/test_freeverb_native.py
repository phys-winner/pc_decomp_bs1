"""Native FMOD Freeverb block versus the original lifted functions, bit for bit.

1. The canonical lifter override at 0x10E946D8 is the only change: the first
   instruction becomes a call of android/host/bioshock_audio_dsp.c, the bytes
   of all three original functions are pinned, and other encodings are refused.
2. On the user-selected ARM64 device, the ORIGINAL lifted sub_10E946D8 (the
   override line replaced by the generator's ordinary emission) and the
   PRODUCTION lifted sub_10E946D8 (with the native body) run on identical
   random guest states: random gains/feedbacks/damps, buffer sizes and indices
   including wrap and out-of-size indices, zeros of both signs, denormals,
   infinities and NaNs, input/output aliasing, strides 1..3 and counts <= 0.
   Every byte of guest memory outside the callee's dead stack frame, EAX..EDI,
   ESP, the published flags and the x87 stack position must be identical.
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
from tests.host_environment import CLANG, environment

ROOT = Path(__file__).resolve().parents[1]
SERIAL = os.environ.get("BIOSHOCK_TEST_ANDROID")
OVERRIDE = ("    { extern void void_bioshock_freeverb_block(void); RECOMP_REGS_OUT(); "
            "void_bioshock_freeverb_block(); RECOMP_REGS_IN(); { RECOMP_REGS_OUT(); return; } } "
            "/* 0x10E946D8: FMOD Freeverb block, native body (bioshock_audio_dsp.c) */\n")
ORIGINAL = "    PUSH32(esp, ebp); /* 0x10E946D8: push ebp */\n"
# 0x10E94630..0x10E947EB: comb, all-pass and block, the bytes the native code reproduces.
BYTES_SHA256 = "3026c43cbfd785788d17d051a6fabbdf023f3b5990d2ce853bd7d8de8587f216"

HARNESS = r'''
#define _GNU_SOURCE
#include "recomp_types.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "CHECK failed line %d: %s\n", __LINE__, #x); exit(3); } } while (0)
uint32_t g_eax, g_ecx, g_edx, g_esp, g_ebx, g_esi, g_edi, g_ebp, g_flag_k, g_flag_a, g_flag_b, g_flag_cf;
uint32_t g_cur_func, g_backedges, g_fs_base;
uint64_t g_mm[8], g_fp_round_mask = ~0ull, g_fp_store_mask = ~0ull;
double g_st_win[RECOMP_X87_SLOTS];
int64_t g_st_i64_win[RECOMP_X87_SLOTS];
int g_fp_sp = 128, g_fp_top, void_fpu_cmp = 1;
uint16_t g_fpu_cw = 0x027F, g_seg_cs, g_seg_ds, g_seg_es, g_seg_fs, g_seg_gs, g_seg_ss;
void (*recomp_yield_hook)(void), (*recomp_spin_hook)(void);
void void_bioshock_audio_return(uint32_t caller, uint32_t value) { (void)caller; (void)value; }
void void_lift_trap(uint32_t address, const char *text) { fprintf(stderr, "trap %08X %s\n", address, text); exit(4); }
static int blocking_depth;
void void_blocking_begin(void) { CHECK(blocking_depth == 0); ++blocking_depth; }
void void_blocking_end(void) { CHECK(blocking_depth == 1); --blocking_depth; }
void void_guest_offload_begin(void) {}
void void_guest_offload_end(void) {}
void sub_10E946D8(void);
void sub_10E946D8_original(void);

#define LO 0x00100000u
#define HI 0x00900000u
#define OBJ 0x00200000u
#define BUFS 0x00300000u
#define SAMPLES 0x00600000u
#define STACK_TOP 0x00800000u
static uint64_t rng = 88172645463325252ull;
static uint32_t next(void) { rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17; return (uint32_t)rng; }
static int no_nan_inputs;
static float special(void) {
    switch (next() % 16) {
    case 0: return 0.0f;
    case 1: return -0.0f;
    case 2: { uint32_t b = next() & 0x807FFFFFu; float f; memcpy(&f, &b, 4); return f; }      /* denormal */
    case 3: { uint32_t b = 0x7F800000u | (next() & 0x80000000u); float f; memcpy(&f, &b, 4); return f; }
    case 4: if (no_nan_inputs) return 1.5f; { uint32_t b = 0x7FC00000u | (next() & 0x803FFFFFu); float f; memcpy(&f, &b, 4); return f; }  /* qNaN */
    case 5: if (no_nan_inputs) return -0.75f; { uint32_t b = 0x7F800001u | (next() & 0x803FFFFEu); float f; memcpy(&f, &b, 4); return f; }  /* sNaN */
    case 6: return (float)((int32_t)next()) * 1e-38f;
    default: return ((float)(int32_t)next() / 2147483648.0f) * 2.0f;
    }
}
static float fraction(void) { return (float)(next() % 1000000u) / 1000000.0f; }
static void put_f(uint32_t va, float v) { memcpy(GPTR(va), &v, 4); }
typedef struct { uint32_t eax, ecx, edx, ebx, esi, edi, ebp, esp, k, a, b, cf; int sp, top; } regs_t;
static regs_t snap(void) { return (regs_t){ g_eax, g_ecx, g_edx, g_ebx, g_esi, g_edi, g_ebp, g_esp, g_flag_k, g_flag_a, g_flag_b, g_flag_cf, g_fp_sp, g_fp_top }; }
static void load(const regs_t *r) { g_eax = r->eax; g_ecx = r->ecx; g_edx = r->edx; g_ebx = r->ebx; g_esi = r->esi; g_edi = r->edi; g_ebp = r->ebp; g_esp = r->esp; g_flag_k = r->k; g_flag_a = r->a; g_flag_b = r->b; g_flag_cf = r->cf; g_fp_sp = r->sp; g_fp_top = r->top; }

int main(int argc, char **argv) {
    unsigned trials = argc > 1 ? (unsigned)atoi(argv[1]) : 3000;
    no_nan_inputs = argc > 2 && !strcmp(argv[2], "nonan");
    void *mapped = mmap((void *)(VOID_GUEST_BASE + LO), HI - LO, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
    CHECK(mapped == (void *)(VOID_GUEST_BASE + LO));
    uint8_t *before = malloc(HI - LO), *lifted = malloc(HI - LO);
    CHECK(before && lifted);
    unsigned compared = 0, early = 0, aliased = 0, specials_seen = 0, nan_payloads = 0;
    for (unsigned trial = 0; trial < trials; ++trial) {
        memset(GPTR(LO), 0xA5, HI - LO);
        int heavy = trial % 4 == 0;
        put_f(OBJ + 0x0, heavy ? special() : fraction());
        put_f(OBJ + 0x18, heavy ? special() : fraction());
        put_f(OBJ + 0x1C, heavy ? special() : fraction());
        put_f(OBJ + 0x20, heavy ? special() : fraction());
        uint32_t heap = BUFS;
        for (unsigned f = 0; f < 16; ++f) {
            uint32_t c = OBJ + (f < 8 ? 0x2C + f * 0x1C : 0x10C + (f - 8) * 0x1C);
            int32_t size = 1 + (int32_t)(next() % (trial % 3 ? 48u : 6u));
            int32_t index = (int32_t)(next() % (uint32_t)(size + (next() % 8 == 0 ? 3 : 0)));
            put_f(c + 0x0, heavy ? special() : fraction()); put_f(c + 0x4, special());
            put_f(c + 0x8, heavy ? special() : fraction()); put_f(c + 0xC, heavy ? special() : fraction());
            MEM32(c + 0x10) = heap; MEM32(c + 0x14) = (uint32_t)size; MEM32(c + 0x18) = (uint32_t)index;
            for (int32_t i = 0; i < size + 4; ++i) put_f(heap + 4u * (uint32_t)i, special());
            heap += 4u * (uint32_t)(size + 8);
        }
        for (unsigned f = 0; f < 8; ++f) {
            uint32_t a = OBJ + (f < 4 ? 0x1EC + f * 0x10 : 0x22C + (f - 4) * 0x10);
            int32_t size = 1 + (int32_t)(next() % (trial % 3 ? 32u : 5u));
            int32_t index = (int32_t)(next() % (uint32_t)(size + (next() % 8 == 0 ? 3 : 0)));
            put_f(a + 0x0, heavy ? special() : fraction());
            MEM32(a + 0x4) = heap; MEM32(a + 0x8) = (uint32_t)size; MEM32(a + 0xC) = (uint32_t)index;
            for (int32_t i = 0; i < size + 4; ++i) put_f(heap + 4u * (uint32_t)i, special());
            heap += 4u * (uint32_t)(size + 8);
        }
        CHECK(heap < SAMPLES);
        int32_t count = (int32_t)(next() % 160u) - (trial % 50 == 0 ? 165 : 0);
        uint32_t stride = 1 + next() % 3;
        uint32_t in_l = SAMPLES, in_r = SAMPLES + 0x10000, out_l = SAMPLES + 0x20000, out_r = SAMPLES + 0x30000;
        switch (next() % 6) { case 0: out_l = in_l; out_r = in_r; aliased++; break; case 1: out_l = in_r; aliased++; break; case 2: out_r = in_l; aliased++; break; default: break; }
        for (uint32_t i = 0; i < 0x40000u / 4u; ++i) put_f(SAMPLES + 4u * i, heavy ? special() : ((float)(int32_t)next() / 2147483648.0f));
        uint32_t esp = STACK_TOP - 0x100;
        MEM32(esp) = RECOMP_RETADDR; MEM32(esp + 4) = in_l; MEM32(esp + 8) = in_r; MEM32(esp + 12) = out_l;
        MEM32(esp + 16) = out_r; MEM32(esp + 20) = (uint32_t)count; MEM32(esp + 24) = stride;
        regs_t start = { next(), OBJ, next(), next(), next(), next(), next(), esp, next() % 8, next(), next(), next() & 1, 128, 0 };
        memcpy(before, GPTR(LO), HI - LO);
        load(&start); sub_10E946D8_original(); regs_t want = snap();
        memcpy(lifted, GPTR(LO), HI - LO);
        memcpy(GPTR(LO), before, HI - LO);
        load(&start); sub_10E946D8(); regs_t got = snap();
        CHECK(blocking_depth == 0);
        if (memcmp(&want, &got, sizeof want)) {
            fprintf(stderr, "trial %u registers differ: eax %08X/%08X ecx %08X/%08X edx %08X/%08X esp %08X/%08X k %u/%u a %08X/%08X b %08X/%08X cf %u/%u sp %d/%d\n",
                    trial, want.eax, got.eax, want.ecx, got.ecx, want.edx, got.edx, want.esp, got.esp, want.k, got.k, want.a, got.a, want.b, got.b, want.cf, got.cf, want.sp, got.sp);
            return 5;
        }
        CHECK(got.esp == esp + 0x1C && got.ebx == start.ebx && got.esi == start.esi && got.edi == start.edi && got.ebp == start.ebp);
        /* The callee's frame below the caller's ESP is dead after `ret 18h`. */
        uint32_t dead_lo = STACK_TOP - 0x1000, dead_hi = esp + 0x1C;
        for (uint32_t va = LO; va < HI; va += 4) {
            if (va >= dead_lo && va < dead_hi) continue;
            if (memcmp(lifted + (va - LO), GPTR(va), 4)) {
                uint32_t a, b; memcpy(&a, lifted + (va - LO), 4); memcpy(&b, GPTR(va), 4);
                /* With two NaN operands ARM64 returns the first operand's NaN
                 * and the compiler may commute fadd/fmul, so only the payload
                 * or sign of a result that is NaN in both builds may differ
                 * (x87 picks yet another one). Everything else is identical. */
                if ((a & 0x7F800000u) == 0x7F800000u && (a & 0x007FFFFFu) &&
                    (b & 0x7F800000u) == 0x7F800000u && (b & 0x007FFFFFu)) { ++nan_payloads; continue; }
                fprintf(stderr, "trial %u memory differs at %08X: lifted %08X native %08X (count %d stride %u)\n", trial, va, a, b, count, stride);
                return 6;
            }
        }
        if (count <= 0) ++early;
        if (heavy) ++specials_seen;
        ++compared;
    }
    if (no_nan_inputs) CHECK(nan_payloads == 0);
    printf("PASS native Freeverb block%s:", no_nan_inputs ? " (no NaN inputs: zero exceptions)" : "");
    printf(" %u trials bit-identical to the original lifted functions except %u words NaN in both (%u count<=0, %u aliased, %u with special values)\n",
           compared, nan_payloads, early, aliased, specials_seen);
    return 0;
}
'''


class FreeverbNativeTest(unittest.TestCase):
    def test_mixer_block_keeps_machine_until_pcm_ready(self):
        source = (ROOT / "android/host/bioshock_audio_dsp.c").read_text(encoding="utf-8")
        program = r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
static uint32_t memory[32];
#define MEM32(a) memory[(a)/4]
#define FK_CMP 1
static uint32_t g_ecx,g_esp,g_eax,g_edx,g_flag_k,g_flag_a,g_flag_b,g_flag_cf;
static int machine_owned=1,computes;
static void void_blocking_begin(void){machine_owned=0;}
static void void_blocking_end(void){machine_owned=1;}
static void void_guest_offload_begin(void){}
static void void_guest_offload_end(void){}
static uint32_t void_bioshock_freeverb_compute(uint32_t self,uint32_t l,uint32_t r,
        uint32_t ol,uint32_t or_,int32_t count,uint32_t stride) {
    /* Releasing once per reverb bus used to put every bus behind a new frame slice. */
    assert(machine_owned);
    assert(self==0x20000&&l==10&&r==20&&ol==30&&or_==40&&count==1024&&stride==2);
    ++computes;return 0x30000;
}
/* ACTUAL */
int main(void){
    MEM32(4)=10;MEM32(8)=20;MEM32(12)=30;MEM32(16)=40;MEM32(20)=1024;MEM32(24)=2;
    for(int bus=0;bus<8;++bus){
        g_ecx=0x20000;g_esp=0;void_bioshock_freeverb_block();
        assert(machine_owned&&computes==bus+1&&g_esp==28&&g_eax==8&&g_ecx==0x2025c&&g_edx==0x30000);
        assert(g_flag_k==FK_CMP&&!g_flag_a&&!g_flag_b&&!g_flag_cf);
    }
    for(int count=-1;count<=0;++count){
        MEM32(20)=(uint32_t)count;g_eax=11;g_ecx=12;g_edx=13;g_esp=0;
        void_bioshock_freeverb_block();
        assert(machine_owned&&computes==8&&g_esp==28&&g_eax==11&&g_ecx==12&&g_edx==13);
        assert(g_flag_a==(uint32_t)count&&!g_flag_b&&!g_flag_cf);
    }
    puts("PASS native Freeverb scheduling: all reverb buses produce PCM without a guest handover; ABI and empty blocks preserved");
}
'''.replace("/* ACTUAL */", function(source, "void void_bioshock_freeverb_block("))
        with tempfile.TemporaryDirectory(prefix="freeverb-scheduling-", dir=ROOT / "build") as temporary:
            stage = Path(temporary)
            path = stage / "probe.c"
            path.write_text(program, encoding="utf-8")
            env, linker = environment("x64")
            for command in ([str(CLANG), "/nologo", "/O2", "/W4", "/WX", "/clang:-Wno-unused-function",
                             "/std:c11", "/clang:--target=x86_64-pc-windows-msvc", "/c", str(path), "/Fo" + str(stage / "probe.obj")],
                            [str(linker), "/nologo", str(stage / "probe.obj"), "/OUT:" + str(stage / "probe.exe")],
                            [str(stage / "probe.exe")]):
                result = subprocess.run(command, cwd=ROOT, env=env, capture_output=True, text=True, timeout=60)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_checked_override_and_original_bytes(self):
        subprocess.run([sys.executable, "-B", "-c",
                        "from tests.test_freeverb_native import FreeverbNativeTest; "
                        "FreeverbNativeTest('test_checked_override_and_original_bytes')._check_lifter()"],
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
        code = pe.get_data(0x10E94630 - pe.OPTIONAL_HEADER.ImageBase, 0x10E947EB - 0x10E94630)
        self.assertEqual(hashlib.sha256(code).hexdigest(), BYTES_SHA256)
        decoder = Cs(CS_ARCH_X86, CS_MODE_32)
        decoder.detail = True
        def instruction(decoded):
            return Instruction(decoded.address, decoded.size, decoded.mnemonic, decoded.op_str, decoded.bytes, decoded.operands)
        first = instruction(next(decoder.disasm(code[0x10E946D8 - 0x10E94630:], 0x10E946D8)))
        emission = CheckedLifter().lift_instruction(first)
        self.assertEqual(["    " + line + "\n" for line in emission], [OVERRIDE])
        source = (ROOT / "generated/bioshock/functions_0246.c").read_text(encoding="utf-8")
        self.assertEqual(source.count(OVERRIDE), 1)
        self.assertEqual(source.count("void_bioshock_freeverb_block"), 2)
        for changed in ("54", "53"):
            with self.subTest(changed=changed), self.assertRaisesRegex(ValueError, "Freeverb block entry"):
                CheckedLifter().lift_instruction(instruction(next(decoder.disasm(bytes.fromhex(changed), 0x10E946D8))))
        other = CheckedLifter().lift_instruction(instruction(next(decoder.disasm(bytes.fromhex("55"), 0x10E946D9))))
        self.assertNotIn("freeverb", "\n".join(other))

    @unittest.skipUnless(SERIAL, "Set BIOSHOCK_TEST_ANDROID to the explicitly selected device")
    def test_native_matches_lifted_on_device(self):
        source = (ROOT / "generated/bioshock/functions_0246.c").read_text(encoding="utf-8")
        production = function(source, "void sub_10E946D8(void)")
        self.assertEqual(production.count(OVERRIDE), 1)
        original = production.replace(OVERRIDE, ORIGINAL).replace("void sub_10E946D8(void)", "void sub_10E946D8_original(void)")
        lifted = "\n\n".join(['#include "functions.h"', "void sub_10E946D8_original(void);",
                              function(source, "void sub_10E94630(void)"), function(source, "void sub_10E9468E(void)"),
                              production, original])
        sdk = Path(os.environ.get("ANDROID_SDK_ROOT", Path.home() / "AppData/Local/Android/Sdk"))
        clang = sdk / "ndk/27.2.12479018/toolchains/llvm/prebuilt/windows-x86_64/bin/clang.exe"
        adb = [str(sdk / "platform-tools/adb.exe"), "-s", SERIAL]
        common = ["--target=aarch64-linux-android29", "-O2", "-fno-strict-aliasing", "-fwrapv", "-ffp-contract=off",
                  "-fno-math-errno", "-DRECOMP_X87_WINDOW", "-DRECOMP_CALL_DIAG", "-Iandroid/runtime", "-Iandroid/host",
                  "-Igenerated", "-Igenerated/bioshock"]
        generated_flags = common + ["-w", "-DRECOMP_GENERATED_CODE", "-DRECOMP_GUEST_STATE_SEPARATE", "-mllvm",
                                    "-basic-aa-separate-storage", "-include", "android/runtime/guest_state_prefix.h"]
        with tempfile.TemporaryDirectory(prefix="bioshock-freeverb-", dir=ROOT / "build") as temporary:
            folder = Path(temporary)
            (folder / "lifted.c").write_text(lifted, encoding="utf-8")
            (folder / "harness.c").write_text(HARNESS, encoding="utf-8")
            objects = []
            for name, flags in (("lifted.c", generated_flags), ("harness.c", common + ["-std=gnu11", "-w"]),
                                (str(ROOT / "android/host/bioshock_audio_dsp.c"), common + ["-std=gnu11", "-Wall", "-Werror"])):
                obj = folder / (Path(name).stem + ".o")
                built = subprocess.run([str(clang), *flags, "-c", str(folder / name) if not Path(name).is_absolute() else name,
                                        "-o", str(obj)], cwd=ROOT, capture_output=True, text=True, timeout=300)
                self.assertEqual(built.returncode, 0, built.stdout + built.stderr)
                objects.append(str(obj))
            binary = folder / "test-freeverb"
            linked = subprocess.run([str(clang), "--target=aarch64-linux-android29", *objects, "-o", str(binary), "-lm"],
                                    cwd=ROOT, capture_output=True, text=True, timeout=120)
            self.assertEqual(linked.returncode, 0, linked.stdout + linked.stderr)
            remote = "/data/local/tmp/bioshock-test-freeverb"
            subprocess.run([*adb, "push", str(binary), remote], check=True, capture_output=True, timeout=60)
            result = subprocess.run([*adb, "shell", f"chmod 755 {remote} && {remote} 4000 && {remote} 4000 nonan; echo EXIT=$?"],
                                    capture_output=True, text=True, timeout=300)
            subprocess.run([*adb, "shell", "rm", "-f", remote], capture_output=True, timeout=30)
        output = result.stdout + result.stderr
        print(output)
        self.assertIn("EXIT=0", output)
        self.assertIn("PASS native Freeverb block", output)


if __name__ == "__main__":
    unittest.main()

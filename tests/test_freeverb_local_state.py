"""The native FMOD Freeverb block with the filters' state in locals versus through guest memory, bit for bit
(android/host/bioshock_audio_dsp.c).

void_bioshock_freeverb_compute keeps the filters' parameters, filterstores and indices in locals for the block
(freeverb_by_sample_local) when freeverb_state_apart finds every index inside its buffer and no buffer and no stream
overlapping the reverb object; otherwise it reads and writes them in guest memory every sample (freeverb_by_sample,
which tests/test_freeverb_native.py compares with the lifted functions on a device). Both keep the order of the
samples, the filters and every buffer and stream access. Here both run on the PC on the same random guest states:
ordinary ones, ones that share memory where the order keeps them exact (buffers over each other, a stream into a
buffer, outputs ahead of inputs, stride 0: the local state is allowed), and ones that must go through guest memory
(a buffer, an output or an input over the object, an index outside its buffer, a size <= 0). Every byte of the guest
window and the returned EDX must match; only words that are NaN in both may differ in payload (the compiler may
commute fadd/fmul operands), and none may in the trials without NaN inputs.
"""
from pathlib import Path
import subprocess
import tempfile
import unittest

from tests.host_environment import CLANG, environment

ROOT = Path(__file__).resolve().parents[1]

HOST_H = r'''
#pragma once
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#define WINDOW (8u << 20)
extern uint8_t guest_window[];
#define GPTR(va) ((void *)(guest_window + ((uint32_t)(va) & (WINDOW - 1u))))
#define MEM32(va) (*(uint32_t *)GPTR(va))
#define FK_CMP 1
extern uint32_t g_eax, g_ecx, g_edx, g_esp, g_flag_k, g_flag_a, g_flag_b, g_flag_cf;
'''

PROBE = r'''
#include "host.h"
#include <stdio.h>
uint8_t guest_window[WINDOW];
uint32_t g_eax, g_ecx, g_edx, g_esp, g_flag_k, g_flag_a, g_flag_b, g_flag_cf;
#include "bioshock_audio_dsp.c"

#define OBJ 0x00200000u
#define BUFS 0x00300000u
#define SAMPLES 0x00600000u
static uint64_t rng = 0x9E3779B97F4A7C15ull;
static uint32_t next(void) { rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17; return (uint32_t)rng; }
static int no_nan;
static float special(void) {
    switch (next() % 16) {
    case 0: return 0.0f;
    case 1: return -0.0f;
    case 2: { uint32_t b = next() & 0x807FFFFFu; float f; memcpy(&f, &b, 4); return f; }
    case 3: { uint32_t b = 0x7F800000u | (next() & 0x80000000u); float f; memcpy(&f, &b, 4); return f; }
    case 4: if (no_nan) return 1.5f; { uint32_t b = 0x7FC00000u | (next() & 0x803FFFFFu); float f; memcpy(&f, &b, 4); return f; }
    case 5: return (float)((int32_t)next()) * 1e-38f;
    default: return ((float)(int32_t)next() / 2147483648.0f) * 2.0f;
    }
}
static float fraction(void) { return (float)(next() % 1000000u) / 1000000.0f; }
static void put_f(uint32_t va, float v) { memcpy(GPTR(va), &v, 4); }
static uint8_t before[WINDOW], expected[WINDOW];

int main(void) {
    unsigned trials = 9000, by_local = 0, fell_back = 0, shared_but_local = 0, nan_words = 0, nan_words_without_nan_inputs = 0;
    unsigned fallback_kind[10] = { 0 };
    for (unsigned trial = 0; trial < trials; ++trial) {
        no_nan = trial >= 6000;   /* the last 3000: no NaN inputs, so no NaN may differ */
        memset(guest_window, 0xA5, WINDOW);
        int heavy = trial % 4 == 0, adversary = (int)(next() % 20);  /* 0..8: one way to share memory or leave a buffer, else none */
        put_f(OBJ + 0x0, heavy ? special() : fraction());
        put_f(OBJ + 0x18, heavy ? special() : fraction());
        put_f(OBJ + 0x1C, heavy ? special() : fraction());
        put_f(OBJ + 0x20, heavy ? special() : fraction());
        uint32_t heap = BUFS, first_buffer = 0;
        for (unsigned f = 0; f < 24; ++f) {
            uint32_t at = f < 8 ? OBJ + 0x2C + f * 0x1C : f < 16 ? OBJ + 0x10C + (f - 8) * 0x1C
                        : f < 20 ? OBJ + 0x1EC + (f - 16) * 0x10 : OBJ + 0x22C + (f - 20) * 0x10;
            int comb = f < 16;
            int32_t size = 1 + (int32_t)(next() % (trial % 3 ? 1200u : 7u));
            int32_t index = (int32_t)(next() % (uint32_t)size);
            if (adversary == 3 && f == 5) index = size + (int32_t)(next() % 3);             /* outside its buffer */
            if (adversary == 4 && f == 9) size = -(int32_t)(next() % 2);                     /* size <= 0 */
            uint32_t buffer = heap;
            if (adversary == 0 && f == 17) buffer = first_buffer;                            /* comb 0's buffer */
            if (adversary == 1 && f == 3) buffer = OBJ + 0x10;                               /* inside the object */
            if (!first_buffer) first_buffer = buffer;
            if (comb) {
                put_f(at + 0x0, heavy ? special() : fraction()); put_f(at + 0x4, special());
                put_f(at + 0x8, heavy ? special() : fraction()); put_f(at + 0xC, heavy ? special() : fraction());
                MEM32(at + 0x10) = buffer; MEM32(at + 0x14) = (uint32_t)size; MEM32(at + 0x18) = (uint32_t)index;
            } else {
                put_f(at + 0x0, heavy ? special() : fraction());
                MEM32(at + 0x4) = buffer; MEM32(at + 0x8) = (uint32_t)size; MEM32(at + 0xC) = (uint32_t)index;
            }
            int32_t words = size > 0 ? size : 1;
            if (buffer == heap) { for (int32_t i = 0; i < words + 4; ++i) put_f(heap + 4u * (uint32_t)i, special()); heap += 4u * (uint32_t)(words + 8); }
        }
        if (heap >= SAMPLES) { fprintf(stderr, "heap overflow\n"); return 2; }
        int32_t count = 1 + (int32_t)(next() % 700u);
        uint32_t stride = 1 + next() % 3, step = stride * 4;
        uint32_t in_l = SAMPLES, in_r = SAMPLES + 0x10000, out_l = SAMPLES + 0x20000, out_r = SAMPLES + 0x30000;
        switch (next() % 6) { case 0: out_l = in_l; out_r = in_r; break; case 1: out_l = in_r; break; case 2: out_r = in_l; break;
                              case 3: in_r = in_l + 4; out_l = in_l; out_r = in_r; break; default: break; }
        if (adversary == 2) out_l = OBJ + 4u * (next() % (FREEVERB_OBJECT / 4u));            /* an output into the object */
        if (adversary == 5) stride = 0;                                                      /* every sample in one place */
        if (adversary == 6) out_r = first_buffer;                                            /* a stream into a buffer */
        if (adversary == 7) in_r = OBJ + 4u * (next() % (FREEVERB_OBJECT / 4u));             /* an input over the object */
        if (adversary == 8) out_l = in_l + step * (1 + next() % 3);                          /* outputs ahead of inputs */
        for (uint32_t i = 0; i < 0x40000u / 4u; ++i) put_f(SAMPLES + 4u * i, heavy ? special() : ((float)(int32_t)next() / 2147483648.0f));
        int apart = freeverb_state_apart(OBJ, in_l, in_r, out_l, out_r, count, stride);
        if (apart) ++by_local; else { ++fell_back; ++fallback_kind[adversary < 9 ? adversary : 9]; }
        /* These reach the object (1, 2, 7) or leave a buffer (3, 4); 0, 5, 6 and 8 share memory elsewhere, in order. */
        if (apart && (adversary == 1 || adversary == 2 || adversary == 3 || adversary == 4 || adversary == 7)) {
            fprintf(stderr, "trial %u: adversary %d was taken as apart (count %d)\n", trial, adversary, count);
            return 3;
        }
        if (apart && adversary <= 8) ++shared_but_local;
        memcpy(before, guest_window, WINDOW);
        uint32_t want = freeverb_by_sample(OBJ, in_l, in_r, out_l, out_r, count, stride);
        memcpy(expected, guest_window, WINDOW);
        memcpy(guest_window, before, WINDOW);
        uint32_t got = void_bioshock_freeverb_compute(OBJ, in_l, in_r, out_l, out_r, count, stride);
        if (want != got) { fprintf(stderr, "trial %u: EDX %08X / %08X\n", trial, want, got); return 4; }
        for (uint32_t va = 0; va < WINDOW; va += 4) {
            uint32_t a, b;
            memcpy(&a, expected + va, 4); memcpy(&b, guest_window + va, 4);
            if (a == b) continue;
            if ((a & 0x7F800000u) == 0x7F800000u && (a & 0x007FFFFFu) && (b & 0x7F800000u) == 0x7F800000u && (b & 0x007FFFFFu)) { ++nan_words; nan_words_without_nan_inputs += no_nan; continue; }
            fprintf(stderr, "trial %u: %08X differs %08X / %08X (apart %d, adversary %d, count %d, stride %u)\n", trial, va, a, b,
                    apart, adversary, count, stride);
            return 5;
        }
    }
    if (nan_words_without_nan_inputs) { fprintf(stderr, "%u NaN words differ without NaN inputs\n", nan_words_without_nan_inputs); return 7; }
    if (by_local < trials / 3 || fell_back < trials / 10 || shared_but_local < trials / 20) {
        fprintf(stderr, "coverage: %u with local state (%u of them sharing memory), %u through guest memory\n", by_local, shared_but_local, fell_back);
        return 6;
    }
    printf("PASS %u trials: %u with the state in locals (%u of them with buffers or streams sharing memory), %u through guest memory "
           "(by adversary %u %u %u %u %u %u %u %u %u, none %u), %u NaN words in both (none without NaN inputs)\n",
           trials, by_local, shared_but_local, fell_back, fallback_kind[0], fallback_kind[1], fallback_kind[2], fallback_kind[3],
           fallback_kind[4], fallback_kind[5], fallback_kind[6], fallback_kind[7], fallback_kind[8], fallback_kind[9], nan_words);
    return 0;
}
'''


class FreeverbLocalStateTest(unittest.TestCase):
    def test_local_state_matches_guest_memory_state(self):
        source = (ROOT / "android/host/bioshock_audio_dsp.c").read_text(encoding="utf-8")
        with tempfile.TemporaryDirectory(prefix="freeverb-local-", dir=ROOT / "build") as temporary:
            stage = Path(temporary)
            (stage / "host.h").write_text(HOST_H, encoding="utf-8")
            (stage / "bioshock_audio_dsp.c").write_text(source, encoding="utf-8")
            (stage / "probe.c").write_text(PROBE, encoding="utf-8")
            env, linker = environment("x64")
            for command in ([str(CLANG), "/nologo", "/O2", "/W3", "/std:c11", "/clang:--target=x86_64-pc-windows-msvc",
                             "/clang:-ffp-contract=off", "/clang:-fno-strict-aliasing", "/clang:-fwrapv",
                             "/clang:-Wno-unused-function", "/c", str(stage / "probe.c"), "/Fo" + str(stage / "probe.obj")],
                            [str(linker), "/nologo", str(stage / "probe.obj"), "/OUT:" + str(stage / "probe.exe")],
                            [str(stage / "probe.exe")]):
                result = subprocess.run(command, cwd=stage, env=env, capture_output=True, text=True, timeout=300)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            print(result.stdout.strip())


if __name__ == "__main__":
    unittest.main()

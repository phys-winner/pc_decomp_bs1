/* FMOD Ex software Freeverb as native code.
 *
 * BioShock's FMOD mixer runs a stereo Freeverb block (0x10E946D8) with eight
 * comb (0x10E94630) and four all-pass (0x10E9468E) filters per channel, per
 * sample. Lifted, that was ~1.15 million guest calls per second and 27 % of
 * all guest-machine time on the RMX3851 (perf_optimization_295.md, machine
 * statistics). freeverb_compute is the same arithmetic in the same order as
 * the generated C of those three functions (generated/bioshock/functions_0246.c):
 * double products and sums, (float) at every `fstp dword`, a value whose
 * exponent bits are zero (zero or denormal) replaced by +0.0f, signed index
 * wrap, every guest load and store in the original order (so aliasing
 * behaves alike). tests/test_freeverb_native.py compares it bit for bit with
 * the lifted functions on the device.
 *
 * It touches exactly the guest memory the original touches apart from the
 * callee's own dead stack frame: the reverb object, its filter buffers and the
 * four sample streams. Keep this short native block inside the guest machine:
 * releasing it for every reverb bus makes the 21 ms PCM producer wait behind
 * another game-thread slice per bus, even when the DSP itself is inexpensive.
 *
 * With the filters' state in locals (freeverb_by_sample_local): when no write
 * of the block (a filter buffer's slots, the output streams) can land in the
 * reverb object and every filter's index lies inside its buffer
 * (freeverb_state_apart), the filters' parameters, filterstores and indices
 * stay in locals for the block and are written back once; the samples, the
 * filters and every buffer and stream access keep the original order, so the
 * bits are the same. the test tablet (Dimensity 8300, 2026-11-22): 102 -> 84 us per
 * 1024-sample block on the prime core. Running each filter over the whole
 * block instead was slower (133 us): every comb's filterstore is a serial
 * chain, and the sample order overlaps sixteen of them.
 */
#include "host.h"

static inline float guest_f32(uint32_t va) { float v; memcpy(&v, GPTR(va), sizeof v); return v; }
static inline void guest_set_f32(uint32_t va, float v) { memcpy(GPTR(va), &v, sizeof v); }
static inline uint32_t guest_u32(uint32_t va) { uint32_t v; memcpy(&v, GPTR(va), sizeof v); return v; }
static inline void guest_set_u32(uint32_t va, uint32_t v) { memcpy(GPTR(va), &v, sizeof v); }
static inline int exponent_zero(float v) { uint32_t bits; memcpy(&bits, &v, sizeof bits); return !(bits & 0x7F800000u); }

/* 0x10E94630: [0] feedback, [4] filterstore, [8] damp1, [C] damp2, [10] buffer, [14] size, [18] index. */
static inline float freeverb_comb(uint32_t comb, float input)
{
    uint32_t slot = guest_u32(comb + 0x10) + guest_u32(comb + 0x18) * 4u;
    float output = guest_f32(slot);
    if (exponent_zero(output)) output = 0.0f;
    double damped = (double)guest_f32(comb + 0xC) * (double)output;
    double kept = (double)guest_f32(comb + 0x8) * (double)guest_f32(comb + 0x4);
    guest_set_f32(comb + 0x4, (float)(damped + kept));
    if (exponent_zero(guest_f32(comb + 0x4))) guest_set_f32(comb + 0x4, 0.0f);
    guest_set_f32(slot, (float)((double)guest_f32(comb) * (double)guest_f32(comb + 0x4) + (double)input));
    guest_set_u32(comb + 0x18, guest_u32(comb + 0x18) + 1u);
    if (!((int32_t)guest_u32(comb + 0x18) < (int32_t)guest_u32(comb + 0x14))) guest_set_u32(comb + 0x18, 0);
    return output;
}

/* 0x10E9468E: [0] feedback, [4] buffer, [8] size, [C] index. Returns the
 * filter output; *buffer_out is the EDX the original leaves (the buffer). */
static inline float freeverb_allpass(uint32_t allpass, float input, uint32_t *buffer_out)
{
    uint32_t index = guest_u32(allpass + 0xC), buffer = guest_u32(allpass + 0x4);
    uint32_t slot = buffer + index * 4u;
    *buffer_out = buffer;
    float delayed = guest_f32(slot);
    if (exponent_zero(delayed)) delayed = 0.0f;
    float output = (float)((double)delayed - (double)input);
    guest_set_f32(slot, (float)((double)guest_f32(allpass) * (double)delayed + (double)input));
    guest_set_u32(allpass + 0xC, guest_u32(allpass + 0xC) + 1u);
    if (!((int32_t)guest_u32(allpass + 0xC) < (int32_t)guest_u32(allpass + 0x8))) guest_set_u32(allpass + 0xC, 0);
    return output;
}

/* 0x10E946D8 for count > 0: this [0] gain, [18] wet1, [1C] wet2, [20] dry,
 * combs L at +2C/R at +10C (stride 1C), all-passes L at +1EC/R at +22C
 * (stride 10). Returns the EDX the original leaves. In the original order. */
static uint32_t freeverb_by_sample(uint32_t self, uint32_t in_l, uint32_t in_r,
                                   uint32_t out_l, uint32_t out_r, int32_t count, uint32_t stride)
{
    uint32_t edx = 0, step = stride * 4u;
    while (count > 0) {
        --count;
        float acc_l = 0.0f, acc_r = 0.0f;
        float input = (float)(((double)guest_f32(in_l) + (double)guest_f32(in_r)) * (double)guest_f32(self));
        for (uint32_t i = 0; i < 8; ++i) {
            acc_l = (float)((double)freeverb_comb(self + 0x2C + i * 0x1C, input) + (double)acc_l);
            acc_r = (float)((double)freeverb_comb(self + 0x10C + i * 0x1C, input) + (double)acc_r);
        }
        for (uint32_t i = 0; i < 4; ++i) {
            acc_l = freeverb_allpass(self + 0x1EC + i * 0x10, acc_l, &edx);
            acc_r = freeverb_allpass(self + 0x22C + i * 0x10, acc_r, &edx);
        }
        double left = (double)guest_f32(self + 0x18) * (double)acc_l;
        left += (double)acc_r * (double)guest_f32(self + 0x1C);
        left += (double)guest_f32(in_l) * (double)guest_f32(self + 0x20);
        guest_set_f32(out_l, (float)left);
        double right = (double)guest_f32(self + 0x18) * (double)acc_r;
        right += (double)acc_l * (double)guest_f32(self + 0x1C);
        right += (double)guest_f32(self + 0x20) * (double)guest_f32(in_r);
        guest_set_f32(out_r, (float)right);
        out_l += step; out_r += step; in_l += step; in_r += step;
    }
    return edx;
}

/* ------------------------------------------------------------ the state in locals */
#define FREEVERB_FILTERS 24          /* 16 combs, 8 all-passes */
#define FREEVERB_OBJECT 0x26Cu       /* the reverb object up to the last all-pass */

static uint32_t freeverb_filter(uint32_t self, int f)
{
    return f < 8 ? self + 0x2Cu + (uint32_t)f * 0x1Cu : f < 16 ? self + 0x10Cu + (uint32_t)(f - 8) * 0x1Cu
         : f < 20 ? self + 0x1ECu + (uint32_t)(f - 16) * 0x10u : self + 0x22Cu + (uint32_t)(f - 20) * 0x10u;
}

/* Whether the state may stay in locals: every filter has size > 0 and its index inside its buffer (so the slots
 * it touches are the buffer's), and no buffer and no stream overlaps the reverb object (an input read there would
 * see a filterstore or an index the locals hold back). Buffers that overlap each other or the streams are fine:
 * those accesses stay in guest memory in the original order. */
static int freeverb_state_apart(uint32_t self, uint32_t in_l, uint32_t in_r, uint32_t out_l, uint32_t out_r, int32_t count,
                                uint32_t stride)
{
    if (count <= 0) return 0;
    const uint64_t top = 1ull << 32, lo = self, hi = (uint64_t)self + FREEVERB_OBJECT;
    const uint64_t reach = (uint64_t)(count - 1) * ((uint64_t)stride * 4u) + 4u;
    if (hi > top) return 0;
    const uint32_t streams[4] = { in_l, in_r, out_l, out_r };
    for (int k = 0; k < 4; ++k)
        if ((uint64_t)streams[k] + reach > top || ((uint64_t)streams[k] < hi && lo < (uint64_t)streams[k] + reach)) return 0;
    for (int f = 0; f < FREEVERB_FILTERS; ++f) {
        uint32_t filter = freeverb_filter(self, f);
        uint32_t buffer = guest_u32(filter + (f < 16 ? 0x10u : 0x4u));
        int32_t size = (int32_t)guest_u32(filter + (f < 16 ? 0x14u : 0x8u));
        int32_t index = (int32_t)guest_u32(filter + (f < 16 ? 0x18u : 0xCu));
        if (size <= 0 || index < 0 || index >= size) return 0;
        uint64_t end = (uint64_t)buffer + (uint64_t)size * 4u;
        if (end > top || ((uint64_t)buffer < hi && lo < end)) return 0;
    }
    return 1;
}

typedef struct { float feedback, filterstore, damp1, damp2; uint32_t buffer, size, index; } freeverb_comb_state;
typedef struct { float feedback; uint32_t buffer, size, index; } freeverb_allpass_state;

static inline float freeverb_comb_local(freeverb_comb_state *c, float input)
{
    uint32_t slot = c->buffer + c->index * 4u;
    float output = guest_f32(slot);
    if (exponent_zero(output)) output = 0.0f;
    double damped = (double)c->damp2 * (double)output;
    double kept = (double)c->damp1 * (double)c->filterstore;
    float store = (float)(damped + kept);
    if (exponent_zero(store)) store = 0.0f;
    c->filterstore = store;
    guest_set_f32(slot, (float)((double)c->feedback * (double)store + (double)input));
    uint32_t index = c->index + 1u;
    c->index = (int32_t)index < (int32_t)c->size ? index : 0;
    return output;
}

static inline float freeverb_allpass_local(freeverb_allpass_state *a, float input)
{
    uint32_t slot = a->buffer + a->index * 4u;
    float delayed = guest_f32(slot);
    if (exponent_zero(delayed)) delayed = 0.0f;
    float output = (float)((double)delayed - (double)input);
    guest_set_f32(slot, (float)((double)a->feedback * (double)delayed + (double)input));
    uint32_t index = a->index + 1u;
    a->index = (int32_t)index < (int32_t)a->size ? index : 0;
    return output;
}

static uint32_t freeverb_by_sample_local(uint32_t self, uint32_t in_l, uint32_t in_r,
                                         uint32_t out_l, uint32_t out_r, int32_t count, uint32_t stride)
{
    freeverb_comb_state comb[16];
    freeverb_allpass_state allpass[8];
    for (int f = 0; f < 16; ++f) {
        uint32_t at = self + (f < 8 ? 0x2Cu + (uint32_t)f * 0x1Cu : 0x10Cu + (uint32_t)(f - 8) * 0x1Cu);
        comb[f].feedback = guest_f32(at); comb[f].filterstore = guest_f32(at + 0x4);
        comb[f].damp1 = guest_f32(at + 0x8); comb[f].damp2 = guest_f32(at + 0xC);
        comb[f].buffer = guest_u32(at + 0x10); comb[f].size = guest_u32(at + 0x14); comb[f].index = guest_u32(at + 0x18);
    }
    for (int f = 0; f < 8; ++f) {
        uint32_t at = self + (f < 4 ? 0x1ECu + (uint32_t)f * 0x10u : 0x22Cu + (uint32_t)(f - 4) * 0x10u);
        allpass[f].feedback = guest_f32(at); allpass[f].buffer = guest_u32(at + 0x4);
        allpass[f].size = guest_u32(at + 0x8); allpass[f].index = guest_u32(at + 0xC);
    }
    const float gain = guest_f32(self), wet1 = guest_f32(self + 0x18), wet2 = guest_f32(self + 0x1C), dry = guest_f32(self + 0x20);
    const uint32_t step = stride * 4u;
    while (count > 0) {
        --count;
        float acc_l = 0.0f, acc_r = 0.0f;
        float input = (float)(((double)guest_f32(in_l) + (double)guest_f32(in_r)) * (double)gain);
        for (int i = 0; i < 8; ++i) {
            acc_l = (float)((double)freeverb_comb_local(&comb[i], input) + (double)acc_l);
            acc_r = (float)((double)freeverb_comb_local(&comb[8 + i], input) + (double)acc_r);
        }
        for (int i = 0; i < 4; ++i) {
            acc_l = freeverb_allpass_local(&allpass[i], acc_l);
            acc_r = freeverb_allpass_local(&allpass[4 + i], acc_r);
        }
        double left = (double)wet1 * (double)acc_l;
        left += (double)acc_r * (double)wet2;
        left += (double)guest_f32(in_l) * (double)dry;
        guest_set_f32(out_l, (float)left);
        double right = (double)wet1 * (double)acc_r;
        right += (double)acc_l * (double)wet2;
        right += (double)dry * (double)guest_f32(in_r);
        guest_set_f32(out_r, (float)right);
        out_l += step; out_r += step; in_l += step; in_r += step;
    }
    for (int f = 0; f < 16; ++f) {
        uint32_t at = self + (f < 8 ? 0x2Cu + (uint32_t)f * 0x1Cu : 0x10Cu + (uint32_t)(f - 8) * 0x1Cu);
        guest_set_f32(at + 0x4, comb[f].filterstore);
        guest_set_u32(at + 0x18, comb[f].index);
    }
    for (int f = 0; f < 8; ++f) {
        uint32_t at = self + (f < 4 ? 0x1ECu + (uint32_t)f * 0x10u : 0x22Cu + (uint32_t)(f - 4) * 0x10u);
        guest_set_u32(at + 0xC, allpass[f].index);
    }
    return guest_u32(self + 0x22Cu + 3u * 0x10u + 0x4u);
}

uint32_t void_bioshock_freeverb_compute(uint32_t self, uint32_t in_l, uint32_t in_r,
                                        uint32_t out_l, uint32_t out_r, int32_t count, uint32_t stride)
{
    static int memory = -1;   /* BIOSHOCK_FREEVERB_MEMORY=1: the state through guest memory every sample (A/B) */
    if (memory < 0) { const char *e = getenv("BIOSHOCK_FREEVERB_MEMORY"); memory = e && e[0] == '1'; }
    if (!memory && freeverb_state_apart(self, in_l, in_r, out_l, out_r, count, stride))
        return freeverb_by_sample_local(self, in_l, in_r, out_l, out_r, count, stride);
    return freeverb_by_sample(self, in_l, in_r, out_l, out_r, count, stride);
}

/* The lifted 0x10E946D8 body calls this at its first instruction and returns
 * (tools/lift_game.py). thiscall, `ret 18h`; EBX/ESI/EDI/EBP preserved; EAX,
 * ECX, EDX and the flags as the original leaves them. */
void void_bioshock_freeverb_block(void)
{
    uint32_t self = g_ecx, esp = g_esp;
    uint32_t in_l = MEM32(esp + 4), in_r = MEM32(esp + 8), out_l = MEM32(esp + 12), out_r = MEM32(esp + 16);
    int32_t count = (int32_t)MEM32(esp + 20);
    uint32_t stride = MEM32(esp + 24);
    if (count <= 0) {
        /* cmp [ebp+arg_10], 0; jle: EAX/ECX/EDX untouched. */
        g_flag_k = FK_CMP; g_flag_a = (uint32_t)count; g_flag_b = 0; g_flag_cf = 0;
        g_esp = esp + 0x1Cu;
        return;
    }
    uint32_t edx = void_bioshock_freeverb_compute(self, in_l, in_r, out_l, out_r, count, stride);
    g_eax = stride * 4u;          /* mov eax, [ebp+var_C] */
    g_ecx = self + 0x25Cu;        /* the last all-pass `this` */
    g_edx = edx;
    g_flag_k = FK_CMP; g_flag_a = 0; g_flag_b = 0; g_flag_cf = 0;   /* cmp [ebp+arg_10], 0 with 0 */
    g_esp = esp + 0x1Cu;
}

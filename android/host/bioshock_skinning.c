#include "host.h"
#include <arm_neon.h>

/* BioShock's CPU vertex skinning, 0x10B99DC0 (__stdcall, `ret 18h`), as
 * native NEON code with the lifted body's arithmetic, operation for
 * operation and in the same order (tests/test_skinning_native.py compares
 * both bit for bit on the device):
 *
 *   (rigid, rigid_count, soft, soft_count, bones, out) -> EAX past the output
 *   rigid  36 bytes: position xyz, three byte vectors at +0C/+10/+14, two
 *          dwords at +18/+1C copied through, bone byte at +20
 *   soft   40 bytes: the same up to +1F, four bone bytes at +20, four weight
 *          bytes at +24
 *   bones  64-byte matrices (rows r0..r3); out: 32 bytes per vertex
 *
 * The original body is SSE with MMX unpacking and an x87 weight. In the
 * lifted body every mulps/addps/subps is an IEEE single operation with the
 * host's rounding (MXCSR.RC is not applied to arithmetic), cvtps2pi
 * truncates under the RC=chop the function installs (NaN and |x| >= 2^31
 * give the integer indefinite 0x80000000), packssdw/packuswb saturate, and
 * the weight is (float)((double)byte * C) with the double C at 0x10FEB9C8.
 * The byte vectors unpack as byte * S - B and pack as sat(trunc(v * K + K))
 * with S, B and K the runtime vectors at 0x112BE120, 0x112BE130 and
 * 0x112BE110 (1/127.5, 1.0 and 127.5 in xyz; read on every call).
 *
 * Built with -ffp-contract=off: no multiply-add is fused, as in the lifted
 * body. The function runs on the caller's thread with the guest machine
 * held, like the lifted body. Overlapping input/output/stack ranges, counts
 * beyond any real mesh and wrapping ranges keep the lifted body. */

#define SKIN_RIGID 0x24u
#define SKIN_SOFT 0x28u
#define SKIN_OUT 0x20u
#define SKIN_BONES (256u * 64u)
#define SKIN_FRAME 0x400u          /* the lifted frame lies within [ESP-0x1D0, ESP) */
#define SKIN_MAX_COUNT 0x01000000u

static inline float32x4_t skin_row(uint32_t va)
{
    return vld1q_f32((const float *)GPTR(va));
}

static inline float guest_f32(uint32_t va)
{
    float value;
    memcpy(&value, GPTR(va), sizeof value);
    return value;
}

/* punpcklbw/punpck[lh]wd with zero, cvtpi2ps, mulps S, subps B. */
static inline float32x4_t skin_unpack(uint32_t va, float32x4_t scale, float32x4_t bias)
{
    uint8x8_t bytes = vreinterpret_u8_u32(vdup_n_u32(MEM32(va)));
    float32x4_t values = vcvtq_f32_u32(vmovl_u16(vget_low_u16(vmovl_u8(bytes))));
    return vsubq_f32(vmulq_f32(values, scale), bias);
}

/* mulps K, addps K, cvtps2pi (chop) x2, packssdw, packuswb with zero. */
static inline uint32_t skin_pack(float32x4_t value, float32x4_t k)
{
    float32x4_t q = vaddq_f32(vmulq_f32(value, k), k);
    /* FCVTZS saturates >= 2^31 to INT32_MAX where x86 gives 0x80000000
     * (which packs to 0); NaN already converts to 0 here, as the indefinite
     * packs to 0 there. */
    uint32x4_t indefinite = vcgeq_f32(q, vdupq_n_f32(2147483648.0f));
    int32x4_t ints = vbicq_s32(vcvtq_s32_f32(q), vreinterpretq_s32_u32(indefinite));
    int16x4_t words = vqmovn_s32(ints);
    uint8x8_t bytes = vqmovun_s16(vcombine_s16(words, words));
    return vget_lane_u32(vreinterpret_u32_u8(bytes), 0);
}

static inline void skin_store(uint32_t out, float32x4_t position, uint32_t t0, uint32_t t1, uint32_t t2,
                              uint32_t uv0, uint32_t uv1)
{
    uint32x4_t low = vsetq_lane_u32(t0, vreinterpretq_u32_f32(position), 3);
    uint32x4_t high = { t1, t2, uv0, uv1 };
    vst1q_u32((uint32_t *)GPTR(out), low);
    vst1q_u32((uint32_t *)GPTR(out + 16u), high);
}

static void skin_rigid(uint32_t vertex, uint32_t count, uint32_t bones, uint32_t out,
                       float32x4_t scale, float32x4_t bias, float32x4_t k)
{
    for (; count; --count, vertex += SKIN_RIGID, out += SKIN_OUT) {
        float px = guest_f32(vertex), py = guest_f32(vertex + 4u), pz = guest_f32(vertex + 8u);
        float32x4_t n0 = skin_unpack(vertex + 0x0Cu, scale, bias);
        float32x4_t n1 = skin_unpack(vertex + 0x10u, scale, bias);
        float32x4_t n2 = skin_unpack(vertex + 0x14u, scale, bias);
        uint32_t matrix = bones + ((uint32_t)MEM8(vertex + 0x20u) << 6);
        float32x4_t r0 = skin_row(matrix), r1 = skin_row(matrix + 0x10u);
        float32x4_t r2 = skin_row(matrix + 0x20u), r3 = skin_row(matrix + 0x30u);
        /* ((x*r0 + y*r1) + z*r2) + r3 */
        float32x4_t position = vaddq_f32(vaddq_f32(vaddq_f32(vmulq_n_f32(r0, px), vmulq_n_f32(r1, py)),
                                                   vmulq_n_f32(r2, pz)), r3);
        /* (n.y*r1 + n.x*r0) + n.z*r2 */
        float32x4_t t0 = vaddq_f32(vaddq_f32(vmulq_laneq_f32(r1, n0, 1), vmulq_laneq_f32(r0, n0, 0)),
                                   vmulq_laneq_f32(r2, n0, 2));
        float32x4_t t1 = vaddq_f32(vaddq_f32(vmulq_laneq_f32(r1, n1, 1), vmulq_laneq_f32(r0, n1, 0)),
                                   vmulq_laneq_f32(r2, n1, 2));
        float32x4_t t2 = vaddq_f32(vaddq_f32(vmulq_laneq_f32(r1, n2, 1), vmulq_laneq_f32(r0, n2, 0)),
                                   vmulq_laneq_f32(r2, n2, 2));
        skin_store(out, position, skin_pack(t0, k), skin_pack(t1, k), skin_pack(t2, k),
                   MEM32(vertex + 0x18u), MEM32(vertex + 0x1Cu));
    }
}

static void skin_soft(uint32_t vertex, uint32_t count, uint32_t bones, uint32_t out,
                      float32x4_t scale, float32x4_t bias, float32x4_t k, double c)
{
    for (; count; --count, vertex += SKIN_SOFT, out += SKIN_OUT) {
        float px = guest_f32(vertex), py = guest_f32(vertex + 4u), pz = guest_f32(vertex + 8u);
        float32x4_t n0 = skin_unpack(vertex + 0x0Cu, scale, bias);
        float32x4_t n1 = skin_unpack(vertex + 0x10u, scale, bias);
        float32x4_t n2 = skin_unpack(vertex + 0x14u, scale, bias);
        float32x4_t position = vdupq_n_f32(0.0f), a0 = position, a1 = position, a2 = position;
        for (uint32_t i = 0; i < 4u; ++i) {
            /* fild byte; fmul st(1) (C); fstp dword */
            float weight = (float)((double)MEM8(vertex + 0x24u + i) * c);
            uint32_t matrix = bones + ((uint32_t)MEM8(vertex + 0x20u + i) << 6);
            float32x4_t r0 = skin_row(matrix), r1 = skin_row(matrix + 0x10u);
            float32x4_t r2 = skin_row(matrix + 0x20u), r3 = skin_row(matrix + 0x30u);
            /* (((r1*y + r0*x) + r2*z) + r3) * w + accumulator */
            float32x4_t p = vaddq_f32(vaddq_f32(vaddq_f32(vmulq_n_f32(r1, py), vmulq_n_f32(r0, px)),
                                                vmulq_n_f32(r2, pz)), r3);
            position = vaddq_f32(vmulq_n_f32(p, weight), position);
            /* ((r0*n.x + r1*n.y) + n.z*r2) * w + accumulator */
            float32x4_t t0 = vaddq_f32(vaddq_f32(vmulq_laneq_f32(r0, n0, 0), vmulq_laneq_f32(r1, n0, 1)),
                                       vmulq_laneq_f32(r2, n0, 2));
            float32x4_t t1 = vaddq_f32(vaddq_f32(vmulq_laneq_f32(r0, n1, 0), vmulq_laneq_f32(r1, n1, 1)),
                                       vmulq_laneq_f32(r2, n1, 2));
            float32x4_t t2 = vaddq_f32(vaddq_f32(vmulq_laneq_f32(r0, n2, 0), vmulq_laneq_f32(r1, n2, 1)),
                                       vmulq_laneq_f32(r2, n2, 2));
            a2 = vaddq_f32(vmulq_n_f32(t2, weight), a2);
            a0 = vaddq_f32(vmulq_n_f32(t0, weight), a0);
            a1 = vaddq_f32(vmulq_n_f32(t1, weight), a1);
        }
        skin_store(out, position, skin_pack(a0, k), skin_pack(a1, k), skin_pack(a2, k),
                   MEM32(vertex + 0x18u), MEM32(vertex + 0x1Cu));
    }
}

static int skin_disjoint(uint64_t a, uint64_t a_size, uint64_t b, uint64_t b_size)
{
    return !a_size || !b_size || a + a_size <= b || b + b_size <= a;
}

/* Called at the lifted body's first instruction (tools/lift_game.py).
 * Returns 0 to run the lifted body instead. On 1 the guest state is the one
 * the original leaves at its `ret 18h`: EAX past the output, ECX/EDX as its
 * last moves left them, EBX/ESI/EDI/EBP and MXCSR restored, the x87 stack
 * as found, the flags of its final `test ecx, ecx` (no soft vertices) or
 * `sub ebx, 1`. XMM and MMX registers are caller-saved in the 32-bit MSVC
 * ABI (and EMMS ends the original); its two callers (0x10B41DA1,
 * 0x10B9A470) write them before any read. */
int void_bioshock_skin_vertices(void)
{
    uint32_t esp = g_esp;
    uint32_t rigid = MEM32(esp + 4u), rigid_count = MEM32(esp + 8u), soft = MEM32(esp + 12u);
    uint32_t soft_count = MEM32(esp + 16u), bones = MEM32(esp + 20u), out = MEM32(esp + 24u);
    if (rigid_count > SKIN_MAX_COUNT || soft_count > SKIN_MAX_COUNT) return 0;
    uint64_t out_size = (uint64_t)SKIN_OUT * ((uint64_t)rigid_count + soft_count);
    uint64_t rigid_size = (uint64_t)SKIN_RIGID * rigid_count, soft_size = (uint64_t)SKIN_SOFT * soft_count;
    uint64_t bones_size = rigid_count || soft_count ? SKIN_BONES : 0;
    uint64_t frame = (uint64_t)esp - SKIN_FRAME, frame_size = SKIN_FRAME + 0x1Cu;
    if (esp < SKIN_FRAME || (uint64_t)out + out_size > 0x100000000ull ||
        (uint64_t)rigid + rigid_size > 0x100000000ull || (uint64_t)soft + soft_size > 0x100000000ull ||
        (uint64_t)bones + bones_size > 0x100000000ull)
        return 0;
    if (!skin_disjoint(out, out_size, rigid, rigid_size) || !skin_disjoint(out, out_size, soft, soft_size) ||
        !skin_disjoint(out, out_size, bones, bones_size) || !skin_disjoint(out, out_size, frame, frame_size) ||
        !skin_disjoint(rigid, rigid_size, frame, frame_size) || !skin_disjoint(soft, soft_size, frame, frame_size) ||
        !skin_disjoint(bones, bones_size, frame, frame_size))
        return 0;
    float32x4_t k = skin_row(0x112BE110u), scale = skin_row(0x112BE120u), bias = skin_row(0x112BE130u);
    if (rigid_count) {
        skin_rigid(rigid, rigid_count, bones, out, scale, bias, k);
        g_edx = MEM32(rigid + SKIN_RIGID * (rigid_count - 1u) + 0x1Cu);
    }
    if (soft_count) {
        double c;
        memcpy(&c, GPTR(0x10FEB9C8u), sizeof c);
        skin_soft(soft, soft_count, bones, out + SKIN_OUT * rigid_count, scale, bias, k, c);
        g_ecx = MEM32(soft + SKIN_SOFT * (soft_count - 1u) + 0x1Cu);
        g_edx = soft + 0x10u + SKIN_SOFT * soft_count;
        g_flag_k = FK_CMP; g_flag_a = 1; g_flag_b = 1; g_flag_cf = 0;      /* sub ebx, 1 reaching 0 */
    } else {
        g_ecx = 0;                                                          /* mov ecx, [ebp+14h] */
        g_flag_k = FK_TEST; g_flag_a = 0; g_flag_b = 0; g_flag_cf = 0;      /* test ecx, ecx */
    }
    g_eax = out + SKIN_OUT * (rigid_count + soft_count);
    g_esp = esp + 0x1Cu;
    return 1;
}

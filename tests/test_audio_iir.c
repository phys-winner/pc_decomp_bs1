#include <inttypes.h>

static unsigned char guest_memory[65536];
#define VOID_GUEST_BASE ((uintptr_t)guest_memory)
#define RECOMP_GENERATED_CODE
#define RECOMP_X87_WINDOW
#include "../android/runtime/recomp_types.h"

uint32_t g_eax, g_ecx, g_edx, g_esp, g_ebx, g_esi, g_edi, g_ebp;
uint32_t g_flag_k, g_flag_a, g_flag_b, g_flag_cf;
double g_st_win[RECOMP_X87_SLOTS];
int64_t g_st_i64_win[RECOMP_X87_SLOTS];
int g_fp_sp, g_fp_top;
uint16_t g_fpu_cw;
int void_fpu_cmp;

void reference_sub_10E94630(void) {
    uint32_t _cf = 0;
    int _df = 1;
    uint32_t _flag_k = FK_NONE;
    uint32_t _flag_a = 0, _flag_b = 0;
    uint32_t _itail_tgt = 0;
    RECOMP_REGS_LOCALS
    RECOMP_ENTER(0x10E94630u);

L_10E94630:
    PUSH32(esp, ebp);
    ebp = esp;
    PUSH32(esp, ecx);
    edx = MEM32(ecx + 0x10);
    eax = MEM32(ecx + 0x18);
    eax = edx + eax * 4;
    fp_push(*(float*)&MEM32(eax));
    edx = 0x7F800000u;
    { float _v = (float)fp_pop(); *(float*)ADDR(ebp + (-0x4)) = _v; }
    _flag_a = (uint32_t)(MEM32(ebp + (-0x4))); _flag_b = (uint32_t)(edx);
    _cf = 0;
    _flag_k = FK_TEST;
    fp_push(0.0);
    if (TEST_NZ(_flag_a, _flag_b)) goto L_10E94651;
L_10E9464E:
    { float _v = (float)_st[0]; *(float*)ADDR(ebp + (-0x4)) = _v; }
L_10E94651:
    fp_push(*(float*)&MEM32(ecx + 0xC));
    _st[0] *= (double)*(float*)ADDR(ebp + (-0x4));
    fp_push(*(float*)&MEM32(ecx + 0x8));
    _st[0] *= (double)*(float*)ADDR(ecx + 4);
    { double _v = fp_pop(); _st[0] += _v; }
    { float _v = (float)fp_pop(); *(float*)ADDR(ecx + 0x4) = _v; }
    _flag_a = (uint32_t)(MEM32(ecx + 0x4)); _flag_b = (uint32_t)(edx);
    _cf = 0;
    _flag_k = FK_TEST;
    if (TEST_NZ(_flag_a, _flag_b)) goto L_10E9466C;
L_10E94667:
    { float _v = (float)fp_pop(); *(float*)ADDR(ecx + 0x4) = _v; }
    goto L_10E9466E;
L_10E9466C:
    { _st[0] = _st[0]; g_st_i64[0] = g_st_i64[0]; fp_pop(); }
L_10E9466E:
    fp_push(*(float*)&MEM32(ecx));
    _st[0] *= (double)*(float*)ADDR(ecx + 4);
    _st[0] += (double)*(float*)ADDR(ebp + 0x8);
    { float _v = (float)fp_pop(); *(float*)ADDR(eax) = _v; }
    _flag_a = (uint32_t)(MEM32(ecx + 0x18)); _flag_b = (uint32_t)(1);
    MEM32(ecx + 0x18) = MEM32(ecx + 0x18) + 1;
    _flag_k = FK_INC;
    eax = MEM32(ecx + 0x18);
    _flag_a = (uint32_t)(eax); _flag_b = (uint32_t)(MEM32(ecx + 0x14));
    _cf = (uint32_t)CMP_B(_flag_a, _flag_b);
    _flag_k = FK_CMP;
    if (CMP_L(_flag_a, _flag_b)) goto L_10E94687;
L_10E94683:
    _flag_a = (uint32_t)(MEM32(ecx + 0x18)); _flag_b = (uint32_t)(0);
    MEM32(ecx + 0x18) = MEM32(ecx + 0x18) & 0;
    _cf = 0;
    _flag_k = FK_TEST;
L_10E94687:
    fp_push(*(float*)&MEM32(ebp + (-0x4)));
    esp = ebp; ebp = POP32_VAL(esp);
    RECOMP_FLAGS_OUT(); esp += 8; { RECOMP_REGS_OUT(); return; }
    RECOMP_REGS_OUT();
}

void reference_sub_10E9468E(void) {
    uint32_t _cf = 0;
    int _df = 1;
    uint32_t _flag_k = FK_NONE;
    uint32_t _flag_a = 0, _flag_b = 0;
    uint32_t _itail_tgt = 0;
    RECOMP_REGS_LOCALS
    RECOMP_ENTER(0x10E9468Eu);

L_10E9468E:
    PUSH32(esp, ebp);
    ebp = esp;
    PUSH32(esp, ecx);
    PUSH32(esp, ecx);
    eax = MEM32(ecx + 0xC);
    edx = MEM32(ecx + 0x4);
    eax = edx + eax * 4;
    fp_push(*(float*)&MEM32(eax));
    { float _v = (float)fp_pop(); *(float*)ADDR(ebp + (-0x4)) = _v; }
    _flag_a = (uint32_t)(MEM32(ebp + (-0x4))); _flag_b = (uint32_t)(0x7F800000u);
    _cf = 0;
    _flag_k = FK_TEST;
    if (TEST_NZ(_flag_a, _flag_b)) goto L_10E946AF;
L_10E946AA:
    fp_push(0.0);
    { float _v = (float)fp_pop(); *(float*)ADDR(ebp + (-0x4)) = _v; }
L_10E946AF:
    fp_push(*(float*)&MEM32(ebp + (-0x4)));
    _st[0] -= (double)*(float*)ADDR(ebp + 0x8);
    { float _v = (float)fp_pop(); *(float*)ADDR(ebp + (-0x8)) = _v; }
    fp_push(*(float*)&MEM32(ecx));
    _st[0] *= (double)*(float*)ADDR(ebp + (-0x4));
    _st[0] += (double)*(float*)ADDR(ebp + 0x8);
    { float _v = (float)fp_pop(); *(float*)ADDR(eax) = _v; }
    _flag_a = (uint32_t)(MEM32(ecx + 0xC)); _flag_b = (uint32_t)(1);
    MEM32(ecx + 0xC) = MEM32(ecx + 0xC) + 1;
    _flag_k = FK_INC;
    eax = MEM32(ecx + 0xC);
    _flag_a = (uint32_t)(eax); _flag_b = (uint32_t)(MEM32(ecx + 8));
    _cf = (uint32_t)CMP_B(_flag_a, _flag_b);
    _flag_k = FK_CMP;
    if (CMP_L(_flag_a, _flag_b)) goto L_10E946D1;
L_10E946CD:
    _flag_a = (uint32_t)(MEM32(ecx + 0xC)); _flag_b = (uint32_t)(0);
    MEM32(ecx + 0xC) = MEM32(ecx + 0xC) & 0;
    _cf = 0;
    _flag_k = FK_TEST;
L_10E946D1:
    fp_push(*(float*)&MEM32(ebp + (-0x8)));
    esp = ebp; ebp = POP32_VAL(esp);
    RECOMP_FLAGS_OUT(); esp += 8; { RECOMP_REGS_OUT(); return; }
    RECOMP_REGS_OUT();
}

#include "../generated/bioshock/audio_iir.h"

static uint64_t seed = UINT64_C(0x724c91a25d86b0ef);
static uint32_t next_value(void)
{
    seed = seed * UINT64_C(6364136223846793005) + UINT64_C(1442695040888963407);
    return (uint32_t)(seed >> 24);
}

struct snapshot {
    uint32_t regs[8], flags[4], object[7], buffer[64];
    double value;
    int64_t shadow;
    int top, sp;
};

static struct snapshot save(uint32_t object, uint32_t buffer)
{
    struct snapshot s = {{g_eax, g_ecx, g_edx, g_ebx, g_esp, g_ebp, g_esi, g_edi},
        {g_flag_k, g_flag_a, g_flag_b, g_flag_cf}, {0}, {0}, g_st[0], g_st_i64[0], g_fp_top, g_fp_sp};
    for (unsigned i = 0; i < 7; ++i) s.object[i] = MEM32(object + i * 4);
    for (unsigned i = 0; i < 64; ++i) s.buffer[i] = MEM32(buffer + i * 4);
    return s;
}

static int verify(uint32_t object, uint32_t buffer, int second, unsigned iteration)
{
    for (unsigned i = 0; i < 7; ++i) MEM32(object + i * 4) = next_value();
    uint32_t limit = 1 + next_value() % 64;
    MEM32(object + (second ? 4 : 0x10)) = buffer;
    MEM32(object + (second ? 8 : 0x14)) = limit;
    MEM32(object + (second ? 0xC : 0x18)) = next_value() % limit;
    for (unsigned i = 0; i < 64; ++i) {
        uint32_t special[] = {0, 0x80000000u, 1, 0x007FFFFFu, 0x7F800000u, 0x7FC00001u, 0xFF800000u, 0x3F800000u};
        MEM32(buffer + i * 4) = i < 8 ? special[i] : next_value();
    }
    MEM32(0x9000) = 0xDEAD0000u;
    MEM32(0x9004) = next_value();
    g_esp = 0x9000;
    g_ebp = 0x11223344u;
    g_ecx = object;
    g_eax = next_value();
    g_edx = next_value();
    g_ebx = next_value();
    g_esi = next_value();
    g_edi = next_value();
    g_fp_sp = 0;
    g_fp_top = 0;
    g_st[0] = -13.25;
    g_st_i64[0] = INT64_C(0x123456789);
    struct snapshot initial = save(object, buffer);
    if (second) reference_sub_10E9468E(); else reference_sub_10E94630();
    struct snapshot expected = save(object, buffer);
    for (unsigned i = 0; i < 7; ++i) MEM32(object + i * 4) = initial.object[i];
    for (unsigned i = 0; i < 64; ++i) MEM32(buffer + i * 4) = initial.buffer[i];
    g_eax = initial.regs[0];
    g_ecx = initial.regs[1];
    g_edx = initial.regs[2];
    g_ebx = initial.regs[3];
    g_esp = initial.regs[4];
    g_ebp = initial.regs[5];
    g_esi = initial.regs[6];
    g_edi = initial.regs[7];
    g_flag_k = initial.flags[0];
    g_flag_a = initial.flags[1];
    g_flag_b = initial.flags[2];
    g_flag_cf = initial.flags[3];
    g_fp_sp = initial.sp;
    g_fp_top = initial.top;
    g_st[0] = initial.value;
    g_st_i64[0] = initial.shadow;
    if (second) sub_10E9468E(); else sub_10E94630();
    struct snapshot actual = save(object, buffer);
    if (memcmp(&expected, &actual, sizeof expected)) {
        fprintf(stderr, "mismatch second=%d iteration=%u expected_esp=%08X actual_esp=%08X expected_top=%d actual_top=%d\n",
            second, iteration, expected.regs[4], actual.regs[4], expected.top, actual.top);
        fprintf(stderr, "expected eax=%08X edx=%08X value=%016" PRIx64 " actual eax=%08X edx=%08X value=%016" PRIx64 "\n",
            expected.regs[0], expected.regs[2], *(uint64_t *)&expected.value,
            actual.regs[0], actual.regs[2], *(uint64_t *)&actual.value);
        for (unsigned i = 0; i < 8; ++i)
            if (expected.regs[i] != actual.regs[i]) fprintf(stderr, "reg[%u] expected=%08X actual=%08X\n", i, expected.regs[i], actual.regs[i]);
        for (unsigned i = 0; i < 4; ++i)
            if (expected.flags[i] != actual.flags[i]) fprintf(stderr, "flag[%u] expected=%08X actual=%08X\n", i, expected.flags[i], actual.flags[i]);
        for (unsigned i = 0; i < 7; ++i)
            fprintf(stderr, "object[%u] initial=%08X expected=%08X actual=%08X\n", i, initial.object[i], expected.object[i], actual.object[i]);
        for (unsigned i = 0; i < 64; ++i)
            if (expected.buffer[i] != actual.buffer[i])
                fprintf(stderr, "buffer[%u] initial=%08X expected=%08X actual=%08X\n", i, initial.buffer[i], expected.buffer[i], actual.buffer[i]);
        fprintf(stderr, "shadow expected=%016" PRIx64 " actual=%016" PRIx64 "\n", (uint64_t)expected.shadow, (uint64_t)actual.shadow);
        return 1;
    }
    return 0;
}

int main(void)
{
    for (unsigned i = 0; i < 50000; ++i) {
        if (verify(0x1000, 0x4000, 0, i) || verify(0x2000, 0x5000, 1, i)) return 1;
    }
    printf("PASS IIR helpers: 100000 exact guest-state comparisons\n");
    return 0;
}

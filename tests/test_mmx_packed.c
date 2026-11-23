#include "../android/runtime/recomp_types.h"
#include <inttypes.h>

static uint64_t next_value(uint64_t *state)
{
    *state = *state * UINT64_C(6364136223846793005) + UINT64_C(1442695040888963407);
    return *state;
}

static uint64_t reference_subusw(uint64_t a, uint64_t b)
{
    uint64_t result = 0;
    for (unsigned lane = 0; lane < 4; ++lane) {
        int value = (int)(uint16_t)(a >> (lane * 16)) - (int)(uint16_t)(b >> (lane * 16));
        result |= (uint64_t)(uint16_t)(value < 0 ? 0 : value) << (lane * 16);
    }
    return result;
}

static uint64_t reference_pslld(uint64_t a, uint32_t count)
{
    if (count > 31) return 0;
    return (uint64_t)((uint32_t)a << count) | ((uint64_t)((uint32_t)(a >> 32) << count) << 32);
}

int main(void)
{
    uint64_t state = UINT64_C(0x734c91a25d86b0ef);
    uint64_t checks = 0;
    for (uint32_t a = 0; a <= UINT16_MAX; ++a) {
        uint64_t left = UINT64_C(0x8000ffff00000000) | a;
        uint64_t right = UINT64_C(0x7fff00010000ffff) | (a * 257u);
        uint64_t actual = mmx_psubusw(left, right), expected = reference_subusw(left, right);
        if (actual != expected) {
            fprintf(stderr, "psubusw exhaustive a=%u actual=%016" PRIx64 " expected=%016" PRIx64 "\n", a, actual, expected);
            return 1;
        }
        ++checks;
    }
    for (unsigned i = 0; i < 500000; ++i) {
        uint64_t a = next_value(&state), b = next_value(&state);
        uint32_t count = i & 63;
        uint64_t actual_sub = mmx_psubusw(a, b), expected_sub = reference_subusw(a, b);
        uint64_t actual_shift = mmx_pslld(a, count), expected_shift = reference_pslld(a, count);
        if (actual_sub != expected_sub || actual_shift != expected_shift) {
            fprintf(stderr, "i=%u a=%016" PRIx64 " b=%016" PRIx64 " count=%u sub=%016" PRIx64 "/%016" PRIx64 " shift=%016" PRIx64 "/%016" PRIx64 "\n",
                i, a, b, count, actual_sub, expected_sub, actual_shift, expected_shift);
            return 1;
        }
        checks += 2;
    }
    printf("PASS MMX psubusw/pslld scalar equivalence: %" PRIu64 " checks\n", checks);
    return 0;
}

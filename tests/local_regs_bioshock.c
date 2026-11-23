#include "host.h"
#include <assert.h>
#include <inttypes.h>

void fixture_check(int good, int code);
void reset(void);
extern uint32_t expected_entry_sp;
void sub_10AC7810(void);

static uint64_t checksum = 1469598103934665603ull;

static void mix(uint32_t value)
{
    checksum = (checksum ^ value) * 1099511628211ull;
}

void sub_10914DB0(void)
{
    fixture_check(0, 301);
    abort();
}

void sub_1094F290(void)
{
    fixture_check(0, 302);
    abort();
}

void bioshock_owner_cases(void)
{
    const uint32_t object = 0x10000;
    const uint32_t entries = 0x20000;
    const uint32_t structures = 0x40000;
    for (unsigned test = 0; test < 2048; ++test) {
        uint32_t count = test * 37u % 4096u + 1u;
        for (uint32_t i = 0; i < count; ++i) {
            MEM32(entries + i * 4) = structures + i * 20;
            MEM32(structures + i * 20 + 0x14) = 1 + i % 3;
        }
        reset();
        g_ecx = object;
        MEM32(object + 4) = entries;
        MEM32(object + 8) = count;
        MEM32(object + 12) = count;
        sub_10AC7810();
        fixture_check(g_esp == expected_entry_sp + 4, 310);
        fixture_check(g_eax == entries, 311);
        fixture_check(g_ebx == 0x4444 && g_ebp == 0x7777 && g_esi == 0x5555 && g_edi == 0x6666, 312);
        fixture_check(MEM32(object + 8) == count && MEM32(object + 12) == count, 313);
        for (uint32_t i = 0; i < count; i += count / 3 + 1) {
            fixture_check(MEM32(structures + i * 20 + 0x14) == 1 + i % 3, 314);
            mix(MEM32(entries + i * 4));
        }
        mix(g_eax); mix(g_ecx); mix(g_edx); mix(g_ebx); mix(g_esp); mix(g_esi); mix(g_edi); mix(g_ebp);
        mix(g_flag_k); mix(g_flag_a); mix(g_flag_b); mix(g_flag_cf);
    }
    printf("BioShock owner cases=2048 checksum=%016" PRIx64 "\n", checksum);
}

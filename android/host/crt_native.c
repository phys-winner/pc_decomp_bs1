/* UT3's msvcr80 memory primitives over the byte-identical BioShock CRT input.
 * memcpy 10014FB0 overlaps like memmove; memmove 10015320 and memset 10014A20.
 * Preserve the C ABI's memory effects, destination EAX, cdecl stack and saved
 * registers. ECX/EDX, flags and XMM are caller-saved; their original scratch
 * values are not reproduced. Import dispatch uses these wrappers; direct CRT
 * calls keep the lifted bodies. Exact mode and wrapping ranges keep originals. */
#include "host.h"

static void_fn original_memcpy, original_memmove, original_memset;
static int crt_mode = -1;

static __attribute__((cold, noinline)) int crt_mode_load(void)
{
    const char *mode = getenv("BIOSHOCK_EXECUTION_MODE");
    if (!mode) mode = getenv("UT3_EXECUTION_MODE");
    const char *enabled = getenv("BIOSHOCK_NATIVE_CRT");
    crt_mode = !((mode && !strcmp(mode, "exact")) || (enabled && *enabled == '0'));
    void_log("[native-crt] memcpy/memmove/memset: mode %d", crt_mode);
    return crt_mode;
}

static inline int crt_native(void)
{
    return __builtin_expect(crt_mode >= 0, 1) ? crt_mode : crt_mode_load();
}

static inline int crt_range(uint32_t address, uint32_t count)
{
    return (uint64_t)address + count <= 0x100000000ull;
}

static void crt_copy(void_fn original)
{
    const uint32_t dst = ARG(0), src = ARG(1), count = ARG(2);
    if (!crt_native() || !crt_range(dst, count) || !crt_range(src, count)) { original(); return; }
    if (count) memmove(GPTR(dst), GPTR(src), count);
    g_eax = dst;
    g_esp += 4;
}

static void crt_memcpy(void) { crt_copy(original_memcpy); }
static void crt_memmove(void) { crt_copy(original_memmove); }

static void crt_memset(void)
{
    const uint32_t dst = ARG(0), value = ARG(1), count = ARG(2);
    if (!crt_native() || !crt_range(dst, count)) { original_memset(); return; }
    if (count) memset(GPTR(dst), (int)(uint8_t)value, count);
    g_eax = dst;
    g_esp += 4;
}

void void_crt_native_init(void)
{
    original_memcpy = void_dispatch_find(0x10014FB0u);
    original_memmove = void_dispatch_find(0x10015320u);
    original_memset = void_dispatch_find(0x10014A20u);
    if (!original_memcpy || !original_memmove || !original_memset) {
        void_log("[native-crt] msvcr80 memory primitives not found; keeping the lifted code");
        return;
    }
    void_dispatch_add(0x10014FB0u, crt_memcpy);
    void_dispatch_add(0x10015320u, crt_memmove);
    void_dispatch_add(0x10014A20u, crt_memset);
}

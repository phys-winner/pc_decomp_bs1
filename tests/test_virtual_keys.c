#include "../android/host/user32.c"
#include <assert.h>
#include <stdio.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <sys/mman.h>
#endif

VOID_HIDDEN uint32_t g_eax, g_esp;
int void_vm_writable(uint32_t address, uint32_t size) { return address <= 4096 && size <= 4096 - address; }

static uint32_t invoke(void (*entry)(void), uint32_t code, uint32_t type)
{
    g_esp = 0;
    MEM32(4) = code;
    MEM32(8) = type;
    entry();
    if (g_esp != 12) fprintf(stderr, "entry failure code=%08X type=%u esp=%08X eax=%08X\n", code, type, g_esp, g_eax);
    assert(g_esp == 12);
    return g_eax;
}

int main(void)
{
#ifdef _WIN32
    assert(VirtualAlloc((void *)VOID_GUEST_BASE, 4096, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
#else
    void *page = mmap((void *)VOID_GUEST_BASE, 4096, PROT_READ | PROT_WRITE,
        MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
    assert(page == (void *)VOID_GUEST_BASE);
#endif
#define CHECK_NAME(param, expected) do { \
    g_esp = 0; MEM32(4) = (param); MEM32(8) = 0x100; MEM32(12) = 63; \
    GetKeyNameTextW_entry(); assert(g_esp == 16); \
    const char *expected_text = (expected); size_t expected_length = strlen(expected_text); \
    assert(g_eax == expected_length); \
    for (size_t i = 0; i < expected_length; ++i) assert(MEM16(0x100 + 2 * i) == (uint8_t)expected_text[i]); \
    assert(MEM16(0x100 + 2 * expected_length) == 0); \
} while (0)
    CHECK_NAME(0x110000, "W");
    CHECK_NAME(0x1E0000, "A");
    CHECK_NAME(0x200000, "D");
    CHECK_NAME(0x390000, "Space");
    CHECK_NAME(0x4B0000, "Num 4");
    CHECK_NAME(0x014B0000, "Left");
    CHECK_NAME(0x011D0000, "Right Ctrl");
    CHECK_NAME(0x01350000, "Num /");
    CHECK_NAME(0x450000, "Pause");
    CHECK_NAME(0x570000, "F11");
    CHECK_NAME(0x010000, "Esc");
    g_esp = 0; MEM32(4) = 0xC50000; MEM32(8) = 0x100; MEM32(12) = 63;
    GetKeyNameTextW_entry(); assert(g_eax == 0);
    g_esp = 0; MEM32(4) = 0x110000; MEM32(8) = 0x100; MEM32(12) = 1; MEM16(0x100) = 'Z'; MEM16(0x102) = 'Q';
    GetKeyNameTextW_entry(); assert(g_eax == 1 && MEM16(0x100) == 'W' && MEM16(0x102) == 'Q');
#undef CHECK_NAME
    assert(invoke(MapVirtualKeyW_entry, 0x57, 0) == 0x11);
    assert(invoke(MapVirtualKeyA_entry, 0x57, 0) == 0x11);
    assert(invoke(MapVirtualKeyW_entry, 0x41, 0) == 0x1E);
    assert(invoke(MapVirtualKeyW_entry, 0x44, 0) == 0x20);
    assert(invoke(MapVirtualKeyW_entry, 0x20, 0) == 0x39);
    assert(invoke(MapVirtualKeyW_entry, 0x25, 0) == 0x4B);
    assert(invoke(MapVirtualKeyW_entry, 0x57, 1) == 0x7A);
    assert(invoke(MapVirtualKeyW_entry, 0x4B, 1) == 0x25);
    assert(invoke(MapVirtualKeyW_entry, 0xE04B, 3) == 0x25);
    assert(invoke(MapVirtualKeyW_entry, 0x1D, 1) == 0x11);
    assert(invoke(MapVirtualKeyW_entry, 0xE01D, 3) == 0xA3);
    assert(invoke(MapVirtualKeyW_entry, 0x35, 1) == 0xBF);
    assert(invoke(MapVirtualKeyW_entry, 0xB5, 1) == 0);
    assert(invoke(MapVirtualKeyW_entry, 0, 1) == 0);
    assert(invoke(MapVirtualKeyW_entry, 0x100, 0) == 0);
    assert(invoke(MapVirtualKeyW_entry, 0x57, 0xFFFFFFFF) == 0);
    for (uint32_t vk = 0; vk < 256; ++vk) {
        uint32_t scan = invoke(MapVirtualKeyW_entry, vk, 0);
        if (!scan || vk == 0x13 || vk == 0x90) continue;
        uint32_t decoded = invoke(MapVirtualKeyW_entry, scan, 1);
        assert(decoded && (decoded == vk || vk >= 0x21));
    }
    puts("PASS production MapVirtualKeyA/W stdcall, W and reverse scan-code mappings, extended keys, zero and invalid inputs");
    return 0;
}

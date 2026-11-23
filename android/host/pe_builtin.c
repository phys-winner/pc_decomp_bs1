/* Prepared VC80 CRT images linked into the AArch64 runtime; pe_images.c verifies their CRC. */
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define LINKED(symbol, file) \
    ".globl " #symbol "\n.hidden " #symbol "\n.balign 16\n" #symbol ":\n.incbin \"" file "\"\n" \
    ".globl " #symbol "_end\n.hidden " #symbol "_end\n" #symbol "_end:\n"
__asm__(".pushsection .rodata.bioshock_crt80,\"a\"\n"
        LINKED(bioshock_msvcr80, "input/msvcr80.dll")
        LINKED(bioshock_msvcp80, "input/msvcp80.dll")
        ".popsection\n");
extern __attribute__((visibility("hidden"))) const uint8_t bioshock_msvcr80[], bioshock_msvcr80_end[],
    bioshock_msvcp80[], bioshock_msvcp80_end[];

const uint8_t *void_pe_builtin(const char *file, size_t *size)
{
    if (!strcmp(file, "msvcr80.dll")) { *size = (size_t)(bioshock_msvcr80_end - bioshock_msvcr80); return bioshock_msvcr80; }
    if (!strcmp(file, "msvcp80.dll")) { *size = (size_t)(bioshock_msvcp80_end - bioshock_msvcp80); return bioshock_msvcp80; }
    return NULL;
}

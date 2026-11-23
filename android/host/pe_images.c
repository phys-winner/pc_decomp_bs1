/* Build the exact PE images used by static generation from the installed game or linked CRT.
 * Every image is checked by size and CRC-32 before any lifted code can run. */
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "replacements/crc32_ieee.h"

void void_log(const char *fmt, ...);
uint8_t *void_files_read_game(const char *relative, size_t *size);
const uint8_t *void_pe_builtin(const char *file, size_t *size);

static const struct { const char *file, *path; uint32_t size, crc; } images[] = {
    {"Bioshock.exe", "Builds/Release/Bioshock.exe", 9938296u, 0x23B94693u},
    {"msvcr80.dll", NULL, 626688u, 0x34F8A54Du},
    {"msvcp80.dll", NULL, 548864u, 0xEE3F9D91u},
    {"binkw32.dll", "Builds/Release/binkw32.dll", 193024u, 0x282AE423u},
    {"d3dx9_33.dll", "Builds/Release/d3dx9_33.dll", 3495784u, 0x28C8AFAFu},
};

static uint16_t rd16(const uint8_t *p) { uint16_t v; memcpy(&v, p, 2); return v; }
static uint32_t rd32(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return v; }

static uint32_t raw_offset(const uint8_t *file, size_t size, uint32_t table, uint32_t sections,
                           uint32_t rva, uint32_t length)
{
    for (uint32_t i = 0; i < sections; ++i) {
        const uint8_t *s = file + table + 40u * i;
        uint32_t va = rd32(s + 12), raw_size = rd32(s + 16), raw = rd32(s + 20);
        if (rva >= va && (uint64_t)rva + length <= (uint64_t)va + raw_size && (uint64_t)raw + raw_size <= size)
            return raw + (rva - va);
    }
    return 0;
}

static int rebase(uint8_t *file, size_t size, uint32_t base)
{
    uint32_t pe = size >= 0x40 ? rd32(file + 0x3C) : 0;
    if (!pe || (uint64_t)pe + 0xF8 > size || memcmp(file + pe, "PE\0\0", 4) || rd16(file + pe + 4) != 0x14C ||
        rd16(file + pe + 0x18) != 0x10B) return 0;
    uint32_t delta = base - rd32(file + pe + 0x34);
    if (!delta) return 1;
    uint32_t sections = rd16(file + pe + 6), table = pe + 0x18 + rd16(file + pe + 0x14);
    if ((uint64_t)table + 40u * sections > size) return 0;
    uint32_t length = rd32(file + pe + 0xA4);
    uint32_t at = raw_offset(file, size, table, sections, rd32(file + pe + 0xA0), length);
    if (!length || !at) return 0;
    for (uint32_t end = at + length; end - at >= 8;) {
        uint32_t page = rd32(file + at), block = rd32(file + at + 4);
        if (!block) break;
        if (block < 8 || block > end - at) return 0;
        for (uint32_t k = at + 8; k + 2 <= at + block; k += 2) {
            uint16_t entry = rd16(file + k);
            if (!(entry >> 12)) continue;
            uint32_t site = entry >> 12 == 3 ? raw_offset(file, size, table, sections, page + (entry & 0xFFFu), 4) : 0;
            if (!site) return 0;
            uint32_t value = rd32(file + site) + delta;
            memcpy(file + site, &value, 4);
        }
        at += block;
    }
    memcpy(file + pe + 0x34, &base, 4);
    return 1;
}

uint8_t *void_pe_image(const char *file, uint32_t base, size_t *size)
{
    size_t i = 0, count = sizeof images / sizeof images[0];
    while (i < count && strcmp(images[i].file, file)) ++i;
    if (i == count) { void_log("[void] no source for image %s", file); errno = ENOENT; return NULL; }
    const char *path = images[i].path ? images[i].path : "the library";
    size_t n = 0;
    uint8_t *data = NULL;
    if (images[i].path) {
        data = void_files_read_game(images[i].path, &n);
    } else {
        const uint8_t *linked = void_pe_builtin(file, &n);
        errno = ENOENT;
        if (linked && (data = (uint8_t *)malloc(n ? n : 1))) memcpy(data, linked, n);
    }
    if (!data) {
        int error = errno;
        void_log("[void] cannot read image %s from %s: %s", file, path, strerror(error));
        errno = error;
        return NULL;
    }
    uint32_t crc = 0;
    if (n != images[i].size || (images[i].path && !rebase(data, n, base)) ||
        (crc = port_crc32_ieee(0, data, n)) != images[i].crc) {
        void_log("[void] %s from %s is not the file the port was built from: %zu bytes, CRC-32 %08X "
                 "(expected %u bytes, %08X)", file, path, n, crc, images[i].size, images[i].crc);
        free(data);
        errno = EILSEQ;
        return NULL;
    }
    *size = n;
    return data;
}

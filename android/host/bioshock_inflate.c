#include "host.h"
#include <zlib.h>

/* Original one-shot zlib wrapper 10922AC0, cdecl. Streaming Flash inflaters stay original. */
static int inflate_mode = -1;

static int inflate_overlap(uint32_t a, uint32_t n, uint32_t b, uint32_t m)
{
    return (uint64_t)a < (uint64_t)b + m && (uint64_t)b < (uint64_t)a + n;
}

int void_bioshock_uncompress(void)
{
    if (inflate_mode < 0) {
        const char *mode = getenv("BIOSHOCK_EXECUTION_MODE");
        if (!mode) mode = getenv("UT3_EXECUTION_MODE");
        const char *enabled = getenv("BIOSHOCK_NATIVE_ZLIB");
        inflate_mode = !((mode && !strcmp(mode,"exact")) || (enabled && *enabled == '0'));
        void_log("[native-zlib] one-shot inflate mode %d",inflate_mode);
    }
    uint32_t stack = g_esp;
    if (!inflate_mode || stack < 128 || stack > UINT32_MAX - 28 || !void_vm_readable(stack,28)) return 0;
    uint32_t out = MEM32(stack+4), length = MEM32(stack+8), in = MEM32(stack+12), size = MEM32(stack+16);
    uint32_t alloc = MEM32(stack+20), release = MEM32(stack+24);
    if (!((alloc == 0x1092A1D0u && release == 0x1092A200u) || (!alloc && !release)) ||
        !void_vm_readable(length,4) || !void_vm_writable(length,4)) return 0;
    uint32_t capacity = MEM32(length);
    /* shortcut: oversized/aliased buffers and custom allocators use the original wrapper. */
    if (!size || !capacity || size > 64u*1024u*1024u || capacity > 64u*1024u*1024u ||
        !void_vm_readable(in,size) || !void_vm_writable(out,capacity) ||
        inflate_overlap(in,size,out,capacity) || inflate_overlap(length,4,out,capacity) ||
        inflate_overlap(length,4,in,size) || inflate_overlap(stack-128,156,in,size) ||
        inflate_overlap(stack-128,156,out,capacity) || inflate_overlap(stack-128,156,length,4)) return 0;
    uint8_t *temporary = malloc((size_t)size + capacity);
    if (!temporary) return 0;
    memcpy(temporary,GPTR(in),size);
    z_stream stream = {0};
    stream.next_in = temporary;
    stream.avail_in = size;
    stream.next_out = temporary + size;
    stream.avail_out = capacity;
    /* Work only on host buffers while the guest mixer can keep feeding audio. */
    void_blocking_begin();
    int result = inflateInit(&stream);
    if (result == Z_OK) {
        result = inflate(&stream,Z_FINISH);
        int ended = inflateEnd(&stream);
        if (result == Z_STREAM_END && ended != Z_OK) result = ended;
    }
    void_blocking_end();
    if (result != Z_STREAM_END) { free(temporary); return 0; }
    memcpy(GPTR(out),temporary + size,stream.total_out);
    MEM32(length) = (uint32_t)stream.total_out;
    free(temporary);
    g_eax = 0;
    g_flag_k = FK_ADD; g_flag_a = stack - 0x38u; g_flag_b = 0x38u; g_flag_cf = 0;
    g_esp = stack + 4;
    return 1;
}

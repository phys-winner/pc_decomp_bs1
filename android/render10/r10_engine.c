/* Bioshock.exe's shader cache (D3DDrv10's FShaderCache) on the build-time shader library: the engine's lookups
 * are answered with tokens of library programs (SPIR-V and GLSL ES made on the PC, r10_shaderlib.h), the engine
 * never reads ShaderCache.pcs10 and never compiles a material shader.  Seams of tools/build_android.py:
 *
 * - 0x109AFABE, the shader getter sub_109AF8E0's call of FShaderCache::Find (sub_10AF7D40: this = cache;
 *   identity, time -> record {size, time, bytecode} or NULL, ret 8) calls void_bioshock_shader_cache_find10, which
 *   answers or runs the original.  (A seam at Find's entry cannot tell its caller: the lifted calls push a dummy
 *   return address.)  The getter's frame holds the identity at ebp-84h (the cache name FString {data, count, max},
 *   sub_109AF770, and the 128-bit GUID) and 44h bytes above it, at ebp-40h, the compilation data {recorder, GUID,
 *   cursor} (sub_10AF8030, sub_10D794F0; its GUID equals the identity's, the getter asserts it); its ebx is the
 *   shader descriptor {+0 pixel shader, +4 the pass file FString, +16 the entry FString}.  The
 *   recorder holds what the factories' field writers recorded: +0 the factory files {wchar name, wchar file}
 *   (sub_10AF8000), +12 the literals {struct, field, type 0 bool / 1 int, value} (sub_10AF7F80, sub_10AF7FC0),
 *   +24 the defines {name, value text} (sub_10AF80B0); FShaderCompiler10 (sub_109B9350) would hand the compiler
 *   the defines, DEFINE_LITERALS made of the literals and D3DDRV10=1.
 * - 0x10AF77F0 FShaderCache::Load(this = cache; compressed), ret 4 (UD3DRenderDevice10 slot 69 before a level
 *   load): not run, the engine's cache stays empty and its 66.7 MB file unread.
 *
 * A variant the library lacks gets its family's program (r10_record_key levels 1 and 2), else the placeholder of
 * its stage, and is recorded in <userdata>/r10-requests/<record key>.req (R10REQ2: the name, the pass and factory
 * files, the GUID, the time stamp and the defines) for tools/build_shaderlib10.py.  BIOSHOCK_R10_SHADERS=dxbc keeps
 * the original bodies: the engine loads ShaderCache.pcs10, creates shaders from its DXBC and compiles what the
 * cache lacks through D3DX10CompileFromMemory (r10_reflect.c). */
#include "r10.h"
#include "r10_shaderlib.h"
#include <stdlib.h>
#include <strings.h>
#include <sys/stat.h>

#define COMPILATION_DATA 0x44u             /* ebp-40h - (ebp-84h) */
#define R10_PLACEHOLDER_VS 0xFFFFFFFFFFFFFF01ull
#define R10_PLACEHOLDER_PS 0xFFFFFFFFFFFFFF02ull

static int mode = -1;   /* 1: the library answers, 0: the engine's own path */

int r10_native_shaders(void)
{
    if (mode < 0) {
        const char *v = getenv("BIOSHOCK_R10_SHADERS");
        if (v && !strcmp(v, "dxbc")) {
            mode = 0;
            R10_LOG("shaders: BIOSHOCK_R10_SHADERS=dxbc, the engine reads ShaderCache.pcs10 and creates shaders from its DXBC");
        } else if (!r10_shaderlib_open()) {
            mode = 0;
            R10_LOG("shaders: no library, the engine's own shader cache is used");
        } else {
            mode = 1;
            R10_LOG("shaders: the engine's shader cache lookups are answered by the library; ShaderCache.pcs10 is not read");
        }
    }
    return mode;
}

/* An FString {wchar data, count with the terminator, max} of ASCII text. */
static int fstring(uint32_t at, char *out, uint32_t cap)
{
    if (!void_vm_readable(at, 12)) return 0;
    uint32_t data = MEM32(at), count = MEM32(at + 4);
    if (!data || !count || count > cap || !void_vm_readable(data, count * 2)) return 0;
    for (uint32_t i = 0; i < count; ++i) {
        uint16_t c = MEM16(data + 2 * i);
        if (!c) { out[i] = 0; return i > 0; }
        if (c >= 128) return 0;
        out[i] = (char)c;
    }
    return 0;
}

/* A NUL-terminated wide string of ASCII text. */
static int wstring(uint32_t data, char *out, uint32_t cap)
{
    for (uint32_t i = 0; i < cap; ++i) {
        if (!void_vm_readable(data + 2 * i, 2)) return 0;
        uint16_t c = MEM16(data + 2 * i);
        if (!c) { out[i] = 0; return i > 0; }
        if (c >= 128) return 0;
        out[i] = (char)c;
    }
    return 0;
}

static const char *gstring(uint32_t va)
{
    if (!va || !void_vm_readable(va, 1)) return NULL;
    size_t n = void_gstrlen(va);
    return n < 4096 && void_vm_readable(va, (uint32_t)n + 1) ? (const char *)GPTR(va) : NULL;
}

typedef struct text { char *data; size_t size, cap; } text;

static int text_add(text *t, const char *s)
{
    size_t n = strlen(s);
    if (t->size + n + 1 > t->cap) {
        size_t cap = t->cap ? t->cap * 2 : 1024;
        while (t->size + n + 1 > cap) cap *= 2;
        char *grown = (char *)realloc(t->data, cap);
        if (!grown) return 0;
        t->data = grown;
        t->cap = cap;
    }
    memcpy(t->data + t->size, s, n + 1);
    t->size += n;
    return 1;
}

enum { MAX_DEFINES = 64, MAX_LITERALS = 256, MAX_FILES = 8 };

/* What the engine asked for: the descriptor's pass, entry and stage, the recorded files and the compiler's defines. */
typedef struct variant {
    int pixel;
    char pass[128], entry[128], files[MAX_FILES][2][128];
    uint32_t file_count;
    r10_define defines[MAX_DEFINES + 2];
    uint32_t define_count;
    text literals;
} variant;

static int variant_read(uint32_t identity, uint32_t descriptor, const char *name, variant *v)
{
    memset(v, 0, sizeof *v);
    if (!void_vm_readable(descriptor, 28) || MEM32(descriptor) > 1) return 0;
    v->pixel = MEM32(descriptor) != 0;
    if (!fstring(descriptor + 4, v->pass, sizeof v->pass) || !fstring(descriptor + 16, v->entry, sizeof v->entry)) return 0;
    uint32_t recorder = MEM32(identity + COMPILATION_DATA);
    if (!void_vm_readable(recorder, 48)) return 0;
    /* The factory files, and the name they make (sub_109AF770): the frame is the one this seam expects. */
    uint32_t fd = MEM32(recorder), fn = MEM32(recorder + 4);
    if (fn > MAX_FILES || (fn && !void_vm_readable(fd, fn * 8))) return 0;
    char joined[512];
    snprintf(joined, sizeof joined, "%s", v->pass);
    for (uint32_t i = 0; i < fn; ++i) {
        if (!wstring(MEM32(fd + 8 * i), v->files[i][0], sizeof v->files[i][0]) ||
            !wstring(MEM32(fd + 8 * i + 4), v->files[i][1], sizeof v->files[i][1])) return 0;
        size_t at = strlen(joined);
        snprintf(joined + at, sizeof joined - at, "%s", v->files[i][1]);
    }
    v->file_count = fn;
    size_t at = strlen(joined);
    snprintf(joined + at, sizeof joined - at, "%s", v->entry);
    if (strcasecmp(joined, name)) return 0;
    /* FShaderCompiler10's defines (sub_109B9350): the recorded defines, DEFINE_LITERALS, D3DDRV10. */
    uint32_t dd = MEM32(recorder + 24), dn = MEM32(recorder + 28), ld = MEM32(recorder + 12), ln = MEM32(recorder + 16);
    if (dn > MAX_DEFINES || ln > MAX_LITERALS || (dn && !void_vm_readable(dd, dn * 8)) || (ln && !void_vm_readable(ld, ln * 16)))
        return 0;
    for (uint32_t i = 0; i < dn; ++i) {
        const char *n = gstring(MEM32(dd + 8 * i)), *value = gstring(MEM32(dd + 8 * i + 4));
        if (!n || !value) return 0;
        v->defines[v->define_count++] = (r10_define){ n, value };
    }
    if (!text_add(&v->literals, "")) return 0;
    for (uint32_t i = 0; i < ln; ++i) {
        uint32_t e = ld + 16 * i, type = MEM32(e + 8);
        const char *st = MEM32(e) ? gstring(MEM32(e)) : NULL, *field = gstring(MEM32(e + 4));
        int32_t value = (int32_t)MEM32(e + 12);
        char line[600];
        if (!field || (MEM32(e) && !st) || type > 1) return 0;   /* other types: the engine appends a stale line */
        if (st) {
            if (type == 1) snprintf(line, sizeof line, "\t%s.%s = %d;\\\n", st, field, value);
            else snprintf(line, sizeof line, "%s.%s = %s;\\\n", st, field, value ? "true" : "false");
        } else {
            if (type == 1) snprintf(line, sizeof line, "\t%s = %d;\\\n", field, value);
            else snprintf(line, sizeof line, "\t%s = %s;\\\n", field, value ? "true" : "false");
        }
        if (!text_add(&v->literals, line)) return 0;
    }
    v->defines[v->define_count++] = (r10_define){ "DEFINE_LITERALS", v->literals.data };
    v->defines[v->define_count++] = (r10_define){ "D3DDRV10", "1" };
    return 1;
}

static void variant_free(variant *v) { free(v->literals.data); v->literals.data = NULL; }

/* An R10REQ2 block: what tools/build_shaderlib10.py needs to compile the variant without its text. */
static void request_format(FILE *f, const variant *v, const char *name, const uint32_t guid[4], uint32_t time, uint64_t record)
{
    fprintf(f, "R10REQ2\nrecord=%016llx\nname=%s\npass=%s\n", (unsigned long long)record, name, v->pass);
    for (uint32_t i = 0; i < v->file_count; ++i) fprintf(f, "pair=%s:%s\n", v->files[i][0], v->files[i][1]);
    fprintf(f, "entry=%s\nprofile=%s\nflags=4096\nguid=%08x%08x%08x%08x\ntime=%u\n", v->entry, v->pixel ? "ps_4_0" : "vs_4_0",
            guid[0], guid[1], guid[2], guid[3], time);
    for (uint32_t i = 0; i < v->define_count; ++i)
        fprintf(f, "define=%zu:%s%zu:%s\n", strlen(v->defines[i].name), v->defines[i].name, strlen(v->defines[i].value),
                v->defines[i].value);
    fprintf(f, "end\n");
}

/* R10REQ2 for tools/build_shaderlib10.py, once per record key and session. */
static int request_write(const variant *v, const char *name, const uint32_t guid[4], uint32_t time, uint64_t record)
{
    static uint64_t written[512];
    static unsigned count;
    for (unsigned i = 0; i < count; ++i) if (written[i] == record) return 1;
    const char *dir = void_data_dir();
    if (!dir) return 0;
    char path[1200];
    snprintf(path, sizeof path, "%s/r10-requests", dir);
    mkdir(path, 0775);
    snprintf(path, sizeof path, "%s/r10-requests/%016llx.req", dir, (unsigned long long)record);
    FILE *f = fopen(path, "wb");
    if (!f) return 0;
    request_format(f, v, name, guid, time, record);
    int ok = !fclose(f);
    if (ok && count < sizeof written / sizeof written[0]) written[count++] = record;
    return ok;
}

/* QA (BIOSHOCK_R10_SHADER_TRACE=1): every cache identity the engine asks for, once per session, with how the library
 * answered (1 identity, 2 record, 3 family, 4 placeholder) and the defines its field writers recorded, as R10REQ2
 * blocks after an R10TRACE line in <userdata>/r10-shader-trace.txt: the game's variants with their GUIDs. */
static void trace_lookup(int how, int32_t program, uint32_t identity, const char *name, const uint32_t guid[4], uint32_t time)
{
    static int mode = -1;
    static FILE *f;
    static uint64_t *seen;
    enum { SEEN = 1u << 16 };
    if (mode < 0) {
        const char *v = getenv("BIOSHOCK_R10_SHADER_TRACE");
        mode = v && !strcmp(v, "1");
        if (mode) {
            const char *dir = void_data_dir();
            char path[1200];
            snprintf(path, sizeof path, "%s/r10-shader-trace.txt", dir ? dir : ".");
            f = fopen(path, "ab");
            seen = (uint64_t *)calloc(SEEN, sizeof *seen);
            if (!f || !seen) mode = 0;
            else R10_LOG("shader trace: %s", path);
        }
    }
    if (!mode) return;
    uint64_t key = r10_name_key(name) ^ ((uint64_t)guid[0] << 32 | guid[1]) * 0x9E3779B97F4A7C15ull ^ ((uint64_t)guid[2] << 32 | guid[3]);
    if (!key) key = 1;
    for (uint32_t i = (uint32_t)key & (SEEN - 1), n = 0; n < SEEN; i = (i + 1) & (SEEN - 1), ++n) {
        if (seen[i] == key) return;
        if (!seen[i]) { seen[i] = key; break; }
    }
    variant v;
    if (!variant_read(identity, g_ebx, name, &v)) {
        fprintf(f, "R10TRACE unreadable how=%d program=%d name=%s guid=%08x%08x%08x%08x time=%u\n", how, program, name,
                guid[0], guid[1], guid[2], guid[3], time);
    } else {
        fprintf(f, "R10TRACE how=%d program=%d\n", how, program);
        request_format(f, &v, name, guid, time, r10_record_key(name, v.pixel ? "ps_4_0" : "vs_4_0", v.defines, v.define_count, 0));
        variant_free(&v);
    }
    fflush(f);
}

/* Find with the stack of a lifted call: [esp] the (dummy) return address, [esp+4] the identity, [esp+8] the time
 * stamp.  1: answered (eax = record, the arguments popped as by ret 8), 0: registers untouched. */
static int shader_cache_find(void)
{
    if (!r10_native_shaders()) return 0;
    uint32_t esp = g_esp;
    if (!void_vm_readable(esp, 12)) return 0;
    uint32_t identity = MEM32(esp + 4), time = MEM32(esp + 8);
    if (!void_vm_readable(identity, COMPILATION_DATA + 24)) return 0;
    const uint32_t guid[4] = { MEM32(identity + 12), MEM32(identity + 16), MEM32(identity + 20), MEM32(identity + 24) };
    /* The getter's frame: its compilation data carries the same GUID (sub_109AF8E0 asserts it). */
    if (memcmp(GPTR(identity + COMPILATION_DATA + 4), guid, sizeof guid)) return 0;
    char name[256];
    if (!fstring(identity, name, sizeof name)) return 0;
    int32_t program = r10_shaderlib_material(r10_name_key(name), guid, time);
    int how = program >= 0 ? 1 : 0;
    if (program < 0) {
        variant v;
        if (variant_read(identity, g_ebx, name, &v)) {
            const char *profile = v.pixel ? "ps_4_0" : "vs_4_0";
            uint64_t record = r10_record_key(name, profile, v.defines, v.define_count, 0);
            program = r10_shaderlib_record(record, time);
            if (program >= 0) how = 2;
            else {
                int level = 0;
                for (int l = 1; l <= 2 && program < 0; ++l) {
                    program = r10_shaderlib_record(r10_record_key(name, profile, v.defines, v.define_count, l), time);
                    if (program >= 0) level = l;
                }
                int recorded = request_write(&v, name, guid, time, record);
                if (program < 0) program = r10_shaderlib_request(v.pixel ? R10_PLACEHOLDER_PS : R10_PLACEHOLDER_VS);
                static unsigned misses;
                if (misses++ < 256)
                    R10_LOG("COMPILE_MISS native %016llx %s %s time %u%s: %s", (unsigned long long)record, name, profile, time,
                            recorded ? ", request recorded" : "",
                            level == 1 ? "the material's program for other scene options is used" :
                            level == 2 ? "a program of the same material is used" : "a placeholder program is used");
                how = level ? 3 : 4;
            }
            variant_free(&v);
        } else {
            static unsigned odd;
            if (odd++ < 16) R10_LOG("shader lookup %s: not in the library, and its compilation data is not the one this seam reads", name);
        }
    }
    trace_lookup(how, program, identity, name, guid, time);
    if (program < 0) return 0;   /* the engine's own path: its cache is empty, it compiles (r10_reflect.c) */
    uint32_t rec = r10_shaderlib_cache_record((uint32_t)program, time);
    if (!rec) return 0;
    if (r10_dev) {
        if (how == 1) ++r10_dev->shader_stats.native_identity;
        else if (how == 2) ++r10_dev->shader_stats.native_record;
        else { ++r10_dev->shader_stats.native_misses; if (how == 3) ++r10_dev->shader_stats.native_fallbacks; }
    }
    g_eax = rec;
    g_esp += 12;
    return 1;
}

/* The shader getter's call of FShaderCache::Find (seam at 0x109AFABE): the library's answer, else the original. */
extern void sub_10AF7D40(void);
void void_bioshock_shader_cache_find10(void)
{
    if (!shader_cache_find()) sub_10AF7D40();
}

int void_bioshock_shader_cache_load(void)
{
    if (!r10_native_shaders()) return 0;
    if (!void_vm_readable(g_esp, 8)) return 0;
    if (r10_dev) ++r10_dev->shader_stats.cache_loads_skipped;
    static unsigned logged;
    if (logged++ < 4) R10_LOG("ShaderCache.pcs10 not loaded: the library answers the engine's shader lookups");
    g_eax = 1;
    g_esp += 8;
    return 1;
}

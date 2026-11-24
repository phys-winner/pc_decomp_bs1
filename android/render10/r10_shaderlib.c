/* The build-time shader library (r10_shaderlib.h): an uncompressed APK asset mapped once, its lookups and the
 * tokens the game holds in place of bytecode. */
#include "r10.h"
#include "r10_shaderlib.h"
#include <stdlib.h>
#include <zlib.h>
#ifdef __ANDROID__
#include <android/asset_manager.h>
#endif

const uint8_t *r10_lib;
static uint32_t lib_size;
static const r10_lib_header *lib_header;
static const r10_lib_shader *lib_shaders;
static const r10_lib_material *lib_materials;
static const r10_lib_key *lib_records, *lib_requests;
/* Index + 1; the mapped identity and timestamp are still checked on a hit. */
static uint32_t recent_materials[4096];
/* Guest blocks of the programs the game was handed: a cache record {size, time, data} at +0, the token at +16. */
static uint32_t *lib_tokens;
#ifdef __ANDROID__
static AAssetManager *lib_manager;
static AAsset *lib_asset;
void r10_shaderlib_set_manager(void *manager) { lib_manager = (AAssetManager *)manager; }
#else
void r10_shaderlib_set_manager(void *manager) { (void)manager; }
#endif

static uint64_t fnv(const uint8_t *data, size_t size, uint64_t h)
{
    for (size_t i = 0; i < size; ++i) { h ^= data[i]; h *= 1099511628211ull; }
    return h;
}

static int range_ok(uint32_t offset, uint64_t size)
{
    return offset <= lib_size && size <= lib_size - offset;
}

static int table_ok(uint32_t offset, uint32_t count, uint32_t entry)
{
    return !(offset & 7) && range_ok(offset, (uint64_t)count * entry);
}

static int material_less(const r10_lib_material *a, const r10_lib_material *b)
{
    if (a->name != b->name) return a->name < b->name;
    for (int i = 0; i < 4; ++i) if (a->guid[i] != b->guid[i]) return a->guid[i] < b->guid[i];
    return 0;
}

static void lib_reject(const char *what, uint32_t index)
{
    R10_LOG("shader library %s %u is invalid", what, index);
    r10_lib = NULL;
}

int r10_shaderlib_open(void)
{
    if (r10_lib) return 1;
    const uint8_t *data = NULL;
    size_t size = 0;
    const char *source = "asset";
#ifdef __ANDROID__
    if (lib_manager && !lib_asset) lib_asset = AAssetManager_open(lib_manager, "shaders/bs_shaderlib10.bin", AASSET_MODE_BUFFER);
    if (lib_asset) {
        data = (const uint8_t *)AAsset_getBuffer(lib_asset);
        size = (size_t)AAsset_getLength64(lib_asset);
    }
#endif
    if (!data) {
        /* Development copy beside the game data (BIOSHOCK_SHADERLIB10=path). */
        const char *path = getenv("BIOSHOCK_SHADERLIB10");
        FILE *f = path ? fopen(path, "rb") : NULL;
        if (f) {
            fseek(f, 0, SEEK_END);
            long n = ftell(f);
            fseek(f, 0, SEEK_SET);
            uint8_t *copy = n > 0 ? (uint8_t *)malloc((size_t)n) : NULL;
            if (copy && fread(copy, 1, (size_t)n, f) == (size_t)n) { data = copy; size = (size_t)n; source = path; }
            else free(copy);
            fclose(f);
        }
    }
    if (!data || size < sizeof(r10_lib_header) || size > UINT32_MAX) {
        R10_LOG("shader library unavailable (assets/shaders/bs_shaderlib10.bin)");
        return 0;
    }
    const r10_lib_header *h = (const r10_lib_header *)data;
    lib_size = (uint32_t)size;
    r10_lib = data;
    if (memcmp(h->magic, R10_LIB_MAGIC, 8) || h->version != R10_LIB_VERSION || h->total_size != size ||
        !table_ok(h->shaders, h->shader_count, sizeof(r10_lib_shader)) ||
        !table_ok(h->materials, h->material_count, sizeof(r10_lib_material)) ||
        !table_ok(h->records, h->record_count, sizeof(r10_lib_key)) ||
        !table_ok(h->requests, h->request_count, sizeof(r10_lib_key))) {
        R10_LOG("shader library header is invalid (version %u, expected %u)", h->version, R10_LIB_VERSION);
        r10_lib = NULL;
        return 0;
    }
    lib_header = h;
    lib_shaders = (const r10_lib_shader *)(data + h->shaders);
    lib_materials = (const r10_lib_material *)(data + h->materials);
    lib_records = (const r10_lib_key *)(data + h->records);
    lib_requests = (const r10_lib_key *)(data + h->requests);
    for (uint32_t i = 0; i < h->shader_count; ++i) {
        const r10_lib_shader *s = &lib_shaders[i];
        if (!range_ok(s->bindings, (uint64_t)s->binding_count * sizeof(r10_lib_binding)) ||
            !range_ok(s->combined, (uint64_t)s->combined_count * sizeof(r10_lib_combined)) ||
            !range_ok(s->ubos, (uint64_t)s->ubo_count * sizeof(r10_lib_ubo)) || !range_ok(s->icb, s->icb_size) ||
            !range_ok(s->spirv, s->spirv_size) || !range_ok(s->noclip, s->noclip_size) || !range_ok(s->es, s->es_size) ||
            !range_ok(s->iface, s->iface_size) || s->type > 1 ||
            (i && (s->hash < lib_shaders[i - 1].hash ||
                   (s->hash == lib_shaders[i - 1].hash && s->dxbc_size <= lib_shaders[i - 1].dxbc_size)))) {
            lib_reject("program", i);
            return 0;
        }
    }
    for (uint32_t i = 0; i < h->material_count; ++i)
        if (lib_materials[i].shader >= h->shader_count || (i && !material_less(&lib_materials[i - 1], &lib_materials[i]))) {
            lib_reject("material", i);
            return 0;
        }
    for (uint32_t i = 0; i < h->record_count; ++i)
        if (lib_records[i].shader >= h->shader_count || (i && lib_records[i - 1].key >= lib_records[i].key)) {
            lib_reject("record", i);
            return 0;
        }
    for (uint32_t i = 0; i < h->request_count; ++i)
        if (lib_requests[i].shader >= h->shader_count || (i && lib_requests[i - 1].key >= lib_requests[i].key)) {
            lib_reject("request", i);
            return 0;
        }
    lib_tokens = (uint32_t *)calloc(h->shader_count ? h->shader_count : 1, sizeof *lib_tokens);
    if (!lib_tokens) {
        lib_reject("token allocation", h->shader_count);
        return 0;
    }
    R10_LOG("shader library %016llx from %s: %u programs (no Direct3D bytecode), %u cache identities, %u records, "
            "%u compile requests, %u bytes",
            (unsigned long long)h->key, source, h->shader_count, h->material_count, h->record_count, h->request_count, lib_size);
    return 1;
}

uint64_t r10_shaderlib_key(void) { return r10_lib ? lib_header->key : 0; }

uint32_t r10_shaderlib_count(void) { return r10_lib ? lib_header->shader_count : 0; }

int r10_shaderlib_warmup(r10_warmup *out)
{
#ifdef __ANDROID__
    static AAsset *asset;
    if (!asset && lib_manager) asset = AAssetManager_open(lib_manager, "shaders/bs_warmup.bin", AASSET_MODE_BUFFER);
    if (!asset) return 0;
    uint64_t size = (uint64_t)AAsset_getLength64(asset);
    uint32_t header[6];
    if (size < sizeof header || size > 1024 * 1024) return 0;
    const uint8_t *bytes = AAsset_getBuffer(asset);
    if (!bytes) return 0;
    memcpy(header, bytes, sizeof header);
    if (memcmp(bytes, "BSWP", 4) || header[1] != 1 || header[2] > R10_WARM_MAX || header[3] > R10_WARM_MAX ||
        header[4] > 2048 || (header[3] && !header[4])) return 0;
    size_t pairs_size = header[2] * sizeof(r10_warm_pair);
    if (sizeof header + pairs_size + (size_t)header[3] * header[4] != size) return 0;
    *out = (r10_warmup){bytes + sizeof header, bytes + sizeof header + pairs_size, header[2], header[3], header[4]};
    return 1;
#else
    (void)out;
    return 0;
#endif
}

const r10_lib_shader *r10_shaderlib_by_hash(uint64_t hash, int type)
{
    if (!r10_lib || !hash) return NULL;
    uint32_t lo = 0, hi = lib_header->shader_count;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        if (lib_shaders[mid].hash < hash) lo = mid + 1;
        else hi = mid;
    }
    for (; lo < lib_header->shader_count && lib_shaders[lo].hash == hash; ++lo)
        if (lib_shaders[lo].type == type) return &lib_shaders[lo];
    return NULL;
}

const r10_lib_shader *r10_shaderlib_find(const uint8_t *dxbc, uint32_t size, uint64_t *hash_out)
{
    uint64_t hash = fnv(dxbc, size, 1469598103934665603ull);
    if (hash_out) *hash_out = hash;
    if (!r10_lib || size < 20) return NULL;
    uint32_t lo = 0, hi = lib_header->shader_count;
    while (lo < hi) {
        uint32_t mid = (lo + hi) / 2;
        const r10_lib_shader *s = &lib_shaders[mid];
        if (s->hash < hash || (s->hash == hash && s->dxbc_size < size)) lo = mid + 1;
        else hi = mid;
    }
    if (lo < lib_header->shader_count) {
        const r10_lib_shader *s = &lib_shaders[lo];
        if (s->hash == hash && s->dxbc_size == size && !memcmp(s->checksum, dxbc + 4, 16)) return s;
    }
    return NULL;
}

const r10_lib_shader *r10_shaderlib_shader(uint32_t index)
{
    return r10_lib && index < lib_header->shader_count ? &lib_shaders[index] : NULL;
}

uint32_t r10_shaderlib_index(const r10_lib_shader *s) { return (uint32_t)(s - lib_shaders); }

const r10_lib_shader *r10_shaderlib_token(const uint8_t *bytes, uint32_t size)
{
    if (!r10_lib || size != R10_TOKEN_SIZE) return NULL;
    r10_token t;
    memcpy(&t, bytes, sizeof t);
    if (t.magic != R10_TOKEN_MAGIC || t.version != R10_LIB_VERSION || t.library != lib_header->key ||
        t.shader >= lib_header->shader_count || t.type != lib_shaders[t.shader].type) return NULL;
    return &lib_shaders[t.shader];
}

/* The guest block of a program: made once, never freed (the engine keeps copies of the token). */
static uint32_t token_block(uint32_t index)
{
    if (!r10_lib || index >= lib_header->shader_count) return 0;
    if (!lib_tokens[index]) {
        uint32_t va = void_galloc(16 + R10_TOKEN_SIZE);
        if (!va) return 0;
        r10_token t = { R10_TOKEN_MAGIC, R10_LIB_VERSION, index, lib_shaders[index].type, lib_header->key, { 0, 0 } };
        MEM32(va) = R10_TOKEN_SIZE;
        MEM32(va + 4) = 0;
        MEM32(va + 8) = va + 16;
        MEM32(va + 12) = 0;
        memcpy(GPTR(va + 16), &t, sizeof t);
        lib_tokens[index] = va;
    }
    return lib_tokens[index];
}

uint32_t r10_shaderlib_token_va(uint32_t index)
{
    uint32_t block = token_block(index);
    return block ? block + 16 : 0;
}

uint32_t r10_shaderlib_cache_record(uint32_t index, uint32_t time)
{
    uint32_t block = token_block(index);
    if (block) MEM32(block + 4) = time;
    return block;
}

int32_t r10_shaderlib_material(uint64_t name, const uint32_t guid[4], uint32_t time)
{
    if (!r10_lib) return -1;
    uint64_t hash = name ^ ((uint64_t)guid[0] << 32 | guid[1]) * UINT64_C(0x9e3779b97f4a7c15)
                         ^ ((uint64_t)guid[2] << 32 | guid[3]);
    uint32_t *entry = &recent_materials[(hash ^ (hash >> 32)) & (sizeof recent_materials / sizeof *recent_materials - 1)];
    if (*entry) {
        const r10_lib_material *m = &lib_materials[*entry - 1];
        if (m->name == name && !memcmp(m->guid, guid, sizeof m->guid))
            return m->time == time ? (int32_t)m->shader : -1;
    }
    r10_lib_material want = { name, { guid[0], guid[1], guid[2], guid[3] }, 0, 0 };
    uint32_t lo = 0, hi = lib_header->material_count;
    while (lo < hi) {
        uint32_t mid = (lo + hi) / 2;
        if (material_less(&lib_materials[mid], &want)) lo = mid + 1;
        else hi = mid;
    }
    if (lo >= lib_header->material_count) return -1;
    const r10_lib_material *m = &lib_materials[lo];
    if (m->name != name || memcmp(m->guid, guid, sizeof m->guid)) return -1;
    *entry = lo + 1;
    return m->time == time ? (int32_t)m->shader : -1;
}

static int32_t key_find(const r10_lib_key *table, uint32_t count, uint64_t key, const r10_lib_key **out)
{
    uint32_t lo = 0, hi = count;
    while (lo < hi) {
        uint32_t mid = (lo + hi) / 2;
        if (table[mid].key < key) lo = mid + 1;
        else hi = mid;
    }
    if (lo >= count || table[lo].key != key) return -1;
    if (out) *out = &table[lo];
    return (int32_t)table[lo].shader;
}

int32_t r10_shaderlib_record(uint64_t key, uint32_t time)
{
    const r10_lib_key *k = NULL;
    if (!r10_lib || key_find(lib_records, lib_header->record_count, key, &k) < 0 || k->time != time) return -1;
    return (int32_t)k->shader;
}

int32_t r10_shaderlib_request(uint64_t key)
{
    return r10_lib ? key_find(lib_requests, lib_header->request_count, key, NULL) : -1;
}

uint64_t r10_name_key(const char *name)
{
    uint64_t h = 1469598103934665603ull;
    for (const char *p = name; *p; ++p) {
        uint8_t c = (uint8_t)*p;
        if (c >= 'A' && c <= 'Z') c = (uint8_t)(c + 32);
        h = fnv(&c, 1, h);
    }
    return h;
}

/* tools/build_shaderlib10.py record_key; SCENE_DEFINES as there. */
static int scene_define(const char *name)
{
    static const char *const scene[] = { "MAX_INCIDENT_LIGHTS", "PER_VERTEX_LUMINANCE", "USES_SPECULAR_CUBEMAP",
                                         "USING_COMPOSITE_LAYERS", "NVIDIA_DX10_SHADOW_OPTS", "USE_VSM_SHADOWS" };
    for (size_t i = 0; i < sizeof scene / sizeof scene[0]; ++i) if (!strcmp(name, scene[i])) return 1;
    return 0;
}

uint64_t r10_record_key(const char *name, const char *profile, const r10_define *defines, uint32_t count, int level)
{
    static const uint8_t domain[8] = { 'R', '1', '0', 'R', 'E', 'C', '1', 0 }, zero = 0, one = 1, eq = '=';
    uint64_t h = fnv(domain, sizeof domain, 1469598103934665603ull);
    for (const char *p = name; *p; ++p) {
        uint8_t c = (uint8_t)*p;
        if (c >= 'A' && c <= 'Z') c = (uint8_t)(c + 32);
        h = fnv(&c, 1, h);
    }
    h = fnv(&zero, 1, h);
    h = fnv((const uint8_t *)profile, strlen(profile), h);
    h = fnv(&zero, 1, h);
    uint8_t l = (uint8_t)level;
    h = fnv(&l, 1, h);
    for (uint32_t i = 0; i < count; ++i) {
        if ((level == 1 && scene_define(defines[i].name)) || (level == 2 && strcmp(defines[i].name, "DEFINE_LITERALS")))
            continue;
        h = fnv(&one, 1, h);
        h = fnv((const uint8_t *)defines[i].name, strlen(defines[i].name), h);
        h = fnv(&eq, 1, h);
        h = fnv((const uint8_t *)defines[i].value, strlen(defines[i].value), h);
    }
    return h;
}

uint64_t r10_request_key(const uint8_t *source, uint32_t size, const char *entry, const char *profile, uint32_t flags,
                         uint32_t defines_va)
{
    uint64_t h = fnv(source, size, 1469598103934665603ull);
    static const uint8_t zero = 0, one = 1;
    h = fnv(&zero, 1, h); h = fnv((const uint8_t *)entry, strlen(entry), h);
    h = fnv(&zero, 1, h); h = fnv((const uint8_t *)profile, strlen(profile), h);
    uint8_t f[4] = { (uint8_t)flags, (uint8_t)(flags >> 8), (uint8_t)(flags >> 16), (uint8_t)(flags >> 24) };
    h = fnv(&zero, 1, h); h = fnv(f, 4, h);
    for (uint32_t at = defines_va; at && void_vm_readable(at, 8) && MEM32(at); at += 8) {
        uint32_t name = MEM32(at), value = MEM32(at + 4);
        h = fnv(&one, 1, h);
        h = fnv(GPTR(name), void_gstrlen(name), h);
        static const uint8_t eq = '=';
        h = fnv(&eq, 1, h);
        if (value) h = fnv(GPTR(value), void_gstrlen(value), h);
    }
    return h;
}

/* Fallback keys of a request (tools/build_shaderlib10.py family_keys): level 1 without the scene's defines,
 * level 2 with the material's literals only. */
uint64_t r10_request_family(const uint8_t *source, uint32_t size, const char *entry, const char *profile, uint32_t flags,
                            uint32_t defines_va, int level)
{
    uint64_t h = r10_request_key(source, size, entry, profile, flags, 0);
    const uint8_t marker = level == 1 ? 2 : 3, one = 1, eq = '=';
    h = fnv(&marker, 1, h);
    for (uint32_t at = defines_va; at && void_vm_readable(at, 8) && MEM32(at); at += 8) {
        const char *name = (const char *)GPTR(MEM32(at));
        uint32_t value = MEM32(at + 4);
        int keep = level == 1 ? !scene_define(name) : !strcmp(name, "DEFINE_LITERALS");
        if (!keep) continue;
        h = fnv(&one, 1, h);
        h = fnv((const uint8_t *)name, strlen(name), h);
        h = fnv(&eq, 1, h);
        if (value) h = fnv(GPTR(value), void_gstrlen(value), h);
    }
    return h;
}

static void *inflate_blob(uint32_t offset, uint32_t packed, uint32_t raw, int text)
{
    if (!packed || !raw) return NULL;
    uint8_t *out = (uint8_t *)malloc(raw + (text ? 1 : 0));
    if (!out) return NULL;
    uLongf length = raw;
    if (uncompress(out, &length, r10_lib + offset, packed) != Z_OK || length != raw) {
        free(out);
        R10_LOG("shader library blob at %u does not inflate", offset);
        return NULL;
    }
    if (text) out[raw] = 0;
    return out;
}

uint32_t *r10_shaderlib_spirv(const r10_lib_shader *s, int noclip, uint32_t *words)
{
    int use_noclip = noclip && s->noclip_size;
    uint32_t *code = (uint32_t *)inflate_blob(use_noclip ? s->noclip : s->spirv, use_noclip ? s->noclip_size : s->spirv_size,
                                              use_noclip ? s->noclip_raw : s->spirv_raw, 0);
    *words = code ? (use_noclip ? s->noclip_raw : s->spirv_raw) / 4 : 0;
    return code;
}

char *r10_shaderlib_glsl(const r10_lib_shader *s)
{
    return (char *)inflate_blob(s->es, s->es_size, s->es_raw, 1);
}

uint8_t *r10_shaderlib_interface(const r10_lib_shader *s, uint32_t *size)
{
    uint8_t *data = (uint8_t *)inflate_blob(s->iface, s->iface_size, s->iface_raw, 0);
    *size = data ? s->iface_raw : 0;
    return data;
}

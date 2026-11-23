/* Every per-site indirect-call cache (recomp_types.h) that was ever filled.
 * A site registers on its first fill; a flush clears the keys and keeps the
 * fill counts, so the list holds each site once. */
static recomp_icache_t **icache_sites;
static size_t icache_count, icache_capacity;

/* UT3's four-target extensions and 256-entry tables for polymorphic sites.
 * Pools allocate lazily; exhaustion keeps the original resolver. */
#define SITE_EXT 0x80000000u
#define SITE_BIG 0x40000000u
#define SITE_INDEX 0x3FFFFFFFu
#define SITE_EXT_SLOTS 4u
#define SITE_EXT_MAX 65536u
#define SITE_BIG_BITS 8u
#define SITE_BIG_MAX 512u
#define SITE_BIG_AFTER 512u
typedef struct {
    uint32_t key[SITE_EXT_SLOTS];      /* ~VA as the inline key: 0 matches no target */
    uint32_t next, misses;
    uint64_t hits;
    recomp_func_t fn[SITE_EXT_SLOTS];
} site_ext;
typedef struct { uint32_t key, unused; recomp_func_t fn; } site_big_entry;
typedef struct { site_big_entry e[1u << SITE_BIG_BITS]; } site_big;
_Static_assert(sizeof(site_ext) == 64, "an extension is one cache line");
static site_ext *site_exts;
static site_big *site_bigs;
static uint32_t site_ext_used, site_big_used;
static int site_cache_mode = 1;
static int site_big_unavailable;

static void icache_flush(void)
{
    for (size_t i = 0; i < icache_count; ++i) icache_sites[i]->key = 0;
    for (uint32_t i = 0; i < site_ext_used; ++i) memset(site_exts[i].key, 0, sizeof site_exts[i].key);
    for (uint32_t i = 0; i < site_big_used; ++i)
        for (uint32_t k = 0; k < (1u << SITE_BIG_BITS); ++k) site_bigs[i].e[k].key = 0;
}

static inline uint32_t site_big_slot(uint32_t va) { return (va * 0x9E3779B1u) >> (32 - SITE_BIG_BITS); }

/* The first fill: the site joins the flush list. */
static __attribute__((noinline)) recomp_func_t site_first(recomp_icache_t *site, uint32_t va)
{
    recomp_func_t fn = void_resolve(va);
    if (icache_count == icache_capacity) {
        size_t capacity = icache_capacity ? icache_capacity * 2 : 65536;
        recomp_icache_t **grown = (recomp_icache_t **)realloc(icache_sites, capacity * sizeof *grown);
        if (!grown) return fn;                 /* uncached: still correct */
        icache_sites = grown;
        icache_capacity = capacity;
    }
    icache_sites[icache_count++] = site;
    site->fills = 1;
    site->fn = fn;
    site->key = ~va;
    return fn;
}

/* A filled single-entry site missed: refill (after a flush or with
 * the pool used up) or move the new target into an extension. */
static __attribute__((noinline)) recomp_func_t site_second(recomp_icache_t *site, uint32_t va)
{
    recomp_func_t fn = void_resolve(va);
    if (site_cache_mode && site->key && !site_exts) {
        site_exts = (site_ext *)calloc(SITE_EXT_MAX, sizeof *site_exts);
        if (!site_exts) site_cache_mode = 0;
    }
    if (site_cache_mode && site->key && site_ext_used < SITE_EXT_MAX) {
        const uint32_t index = site_ext_used++;
        site_ext *x = &site_exts[index];
        x->key[0] = ~va;
        x->fn[0] = fn;
        x->next = 1;
        site->fills = SITE_EXT | index;
        return fn;
    }
    if (site->fills >= RECOMP_ICACHE_FILLS) return fn;
    ++site->fills;
    site->fn = fn;
    site->key = ~va;
    return fn;
}

static __attribute__((noinline)) recomp_func_t site_ext_miss(recomp_icache_t *site, site_ext *x, uint32_t va)
{
    recomp_func_t fn = void_resolve(va);
    if (!site->key) {                          /* flushed: the inline entry first */
        site->fn = fn;
        site->key = ~va;
        return fn;
    }
    ++x->misses;
    if (x->misses >= SITE_BIG_AFTER && x->misses > x->hits && !site_bigs && !site_big_unavailable) {
        site_bigs = (site_big *)calloc(SITE_BIG_MAX, sizeof *site_bigs);
        if (!site_bigs) site_big_unavailable = 1;
    }
    if (site_bigs && x->misses >= SITE_BIG_AFTER && x->misses > x->hits && site_big_used < SITE_BIG_MAX) {
        const uint32_t index = site_big_used++;
        site_big *t = &site_bigs[index];
        for (uint32_t i = 0; i < SITE_EXT_SLOTS; ++i) {
            if (!x->key[i]) continue;
            site_big_entry *e = &t->e[site_big_slot(~x->key[i])];
            e->key = x->key[i];
            e->fn = x->fn[i];
        }
        site_big_entry *e = &t->e[site_big_slot(va)];
        e->key = ~va;
        e->fn = fn;
        site->fills = SITE_BIG | index;
        return fn;
    }
    const uint32_t slot = x->next;
    x->next = (slot + 1) & (SITE_EXT_SLOTS - 1);
    x->key[slot] = ~va;
    x->fn[slot] = fn;
    return fn;
}

static __attribute__((noinline)) recomp_func_t site_big_miss(recomp_icache_t *site, uint32_t index, uint32_t va)
{
    recomp_func_t fn = void_resolve(va);
    if (!site->key) {
        site->fn = fn;
        site->key = ~va;
        return fn;
    }
    site_big_entry *e = &site_bigs[index].e[site_big_slot(va)];
    e->key = ~va;
    e->fn = fn;
    return fn;
}

recomp_func_t void_resolve_site(recomp_icache_t *site, uint32_t va)
{
    /* Call-time diagnostics/counts and invalid targets retain the original resolver. */
    if (__builtin_expect(count_thunks != 0 || !va || va == UINT32_MAX || resolve_diagnostic_va(va), 0))
        return void_resolve(va);
    const uint32_t fills = site->fills, key = ~va;
    if (fills & SITE_EXT) {
        site_ext *x = &site_exts[fills & SITE_INDEX];
        for (uint32_t i = 0; i < SITE_EXT_SLOTS; ++i)
            if (x->key[i] == key) { ++x->hits; return x->fn[i]; }
        return site_ext_miss(site, x, va);
    }
    if (fills & SITE_BIG) {
        const uint32_t index = fills & SITE_INDEX;
        const site_big_entry *e = &site_bigs[index].e[site_big_slot(va)];
        if (e->key == key) return e->fn;
        return site_big_miss(site, index, va);
    }
    if (!fills) return site_first(site, va);
    return site_second(site, va);
}


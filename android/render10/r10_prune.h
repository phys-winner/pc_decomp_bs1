/* Start-up pruning of regenerable files in the profile folder (<userdata>):
 *  - GLES program binaries "ShaderCache/r10-es-<driver>-<lib>-<vs>-<ps>.bin" whose driver key is not the current one can
 *    never be loaded again (a new GPU, driver or shader capability set writes a new key);
 *  - "r10-shader-miss-<hash>.dxbc" dumps of the first 64 library misses of every session, never read by the port:
 *    when more than R10_MISS_KEEP exist, those older than this session go.
 * The name and decision helpers are pure (tested on the host); the folder walks use POSIX and are skipped on Windows. */
#ifndef R10_PRUNE_H
#define R10_PRUNE_H

#include <stdint.h>
#include <string.h>

#define R10_MISS_KEEP 64

static inline int r10_prune_hex(const char *s, unsigned digits, uint64_t *value)
{
    uint64_t v = 0;
    for (unsigned i = 0; i < digits; ++i) {
        char c = s[i];
        unsigned d = c >= '0' && c <= '9' ? (unsigned)(c - '0') : c >= 'a' && c <= 'f' ? (unsigned)(c - 'a' + 10) : 16u;
        if (d > 15) return 0;
        v = v << 4 | d;
    }
    if (value) *value = v;
    return 1;
}

/* Exactly the naming of program_cache_path() in r10_gles.c; 1 and the driver key when name matches. */
static inline int r10_prune_es_name(const char *name, uint64_t *driver_key)
{
    static const char prefix[] = "r10-es-", suffix[] = ".bin";
    size_t prefix_len = sizeof prefix - 1, suffix_len = sizeof suffix - 1;
    if (strlen(name) != prefix_len + 4 * 16 + 3 + suffix_len || strncmp(name, prefix, prefix_len)) return 0;
    const char *p = name + prefix_len;
    for (unsigned i = 0; i < 4; ++i, p += 17) {
        if (!r10_prune_hex(p, 16, i == 0 ? driver_key : NULL)) return 0;
        if (i < 3 && p[16] != '-') return 0;
    }
    return !strcmp(p - 1, suffix);
}

static inline int r10_prune_es_stale(const char *name, uint64_t current_driver_key)
{
    uint64_t key;
    return r10_prune_es_name(name, &key) && key != current_driver_key;
}

/* "r10-shader-miss-<16 hex>.dxbc" as written by r10_objects.c. */
static inline int r10_prune_miss_name(const char *name)
{
    static const char prefix[] = "r10-shader-miss-", suffix[] = ".dxbc";
    size_t prefix_len = sizeof prefix - 1;
    return strlen(name) == prefix_len + 16 + sizeof suffix - 1 && !strncmp(name, prefix, prefix_len) &&
           r10_prune_hex(name + prefix_len, 16, NULL) && !strcmp(name + prefix_len + 16, suffix);
}

/* total: how many dumps exist; a dump goes when there are more than R10_MISS_KEEP and it predates this session. */
static inline int r10_prune_miss_delete(unsigned total, int64_t modified, int64_t session_start)
{
    return total > R10_MISS_KEEP && modified < session_start;
}

#ifndef _WIN32
#include <dirent.h>
#include <stdio.h>
#include <sys/stat.h>
#include <unistd.h>

/* Returns how many files were removed. */
static inline unsigned r10_prune_program_binaries(const char *dir, uint64_t current_driver_key)
{
    DIR *d = opendir(dir);
    unsigned removed = 0;
    if (!d) return 0;
    for (struct dirent *e; (e = readdir(d)); ) {
        char path[1400];
        if (!r10_prune_es_stale(e->d_name, current_driver_key)) continue;
        snprintf(path, sizeof path, "%s/%s", dir, e->d_name);
        if (!unlink(path)) ++removed;
    }
    closedir(d);
    return removed;
}

static inline unsigned r10_prune_shader_misses(const char *dir, int64_t session_start)
{
    unsigned total = 0, removed = 0;
    DIR *d = opendir(dir);
    if (!d) return 0;
    for (struct dirent *e; (e = readdir(d)); ) total += r10_prune_miss_name(e->d_name);
    if (total > R10_MISS_KEEP) {
        rewinddir(d);
        for (struct dirent *e; (e = readdir(d)); ) {
            char path[1400];
            struct stat st;
            if (!r10_prune_miss_name(e->d_name)) continue;
            snprintf(path, sizeof path, "%s/%s", dir, e->d_name);
            if (!stat(path, &st) && S_ISREG(st.st_mode) && r10_prune_miss_delete(total, (int64_t)st.st_mtime, session_start) &&
                !unlink(path)) ++removed;
        }
    }
    closedir(d);
    return removed;
}
#endif

#endif

/* Files: Windows paths over host directories, file handles, directory
 * enumeration and file mappings.
 *
 * The guest sees one drive. "C:\Games\UT3" is the read-mostly game
 * installation; profile folders and the rest of C: are writable directories
 * under the host data root. Every component is resolved case-insensitively,
 * as NTFS lookups are.
 *
 * Shared storage (/sdcard) is a FUSE file system: each lookup, open and
 * directory read is a round trip to the media provider, and a mapped page
 * fault is another. H5 probes every UI/data file as a loose file before it
 * reads its .pak archives (hundreds of misses per screen) and reads archive
 * entries through small mapped windows. Listings of the game's directories
 * are therefore kept in memory (they change only through this file), archive
 * windows are read in one call, and the guest machine is released while the
 * host waits on the file system, so FMOD keeps mixing. */
#define _GNU_SOURCE
#include <sys/syscall.h>
#include "host.h"
#include "zipmount.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <fnmatch.h>
#include <strings.h>
#include <sys/mman.h>
#include <sys/inotify.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <time.h>
#include <unistd.h>
#include <zlib.h>

#define WIN_MAX_PATH 260

typedef struct { const char *win; char host[1024]; } mount_t;
static mount_t mounts[8];
static int mount_count;
static char game_root[1024], data_root[1024], overlay_root[1024];
static zm_mount *game_zip;
static int zip_lookup(const char *windows_path, const zm_entry **entry, int localized_movies);
#ifndef UT3_BIN
#define UT3_BIN "Binaries"
#endif
static char cwd[WIN_MAX_PATH] = "C:\\Games\\BioShock\\" UT3_BIN;
static pthread_mutex_t cwd_lock = PTHREAD_MUTEX_INITIALIZER;

const char *void_root_dir(void) { return game_root; }
const char *void_data_dir(void) { return data_root; }

/* ---------------------------------------------------- directory listings */
typedef struct {
    char *name;
    uint32_t folded;            /* hash of the ASCII-case-folded name */
    unsigned char kind;         /* DT_DIR, DT_REG or DT_UNKNOWN */
    unsigned char stat_valid;
    struct stat st;
} listed_entry;

typedef struct dir_listing {
    struct dir_listing *next;
    uint64_t key;
    char *path;
    int count;
    listed_entry *entries;
    int watch;
} dir_listing;

#define LISTING_BUCKETS 2048u
static dir_listing *listings[LISTING_BUCKETS];
static pthread_mutex_t listing_lock = PTHREAD_MUTEX_INITIALIZER;
static uint64_t listing_hits, listing_loads;
static int listing_watch_fd = -2;

static uint64_t path_key(const char *path, size_t length)
{
    uint64_t h = 1469598103934665603ull;
    for (size_t i = 0; i < length; ++i) h = (h ^ (uint8_t)path[i]) * 1099511628211ull;
    return h;
}

static uint32_t folded_hash(const char *name)
{
    uint32_t h = 2166136261u;
    for (const char *p = name; *p; ++p) {
        unsigned char c = (unsigned char)*p;
        if (c >= 'A' && c <= 'Z') c = (unsigned char)(c + 32);
        h = (h ^ c) * 16777619u;
    }
    return h;
}

static int under(const char *path, const char *root)
{
    size_t n = strlen(root);
    return n && !strncmp(path, root, n) && (path[n] == 0 || path[n] == '/');
}

static int profile_configuration(const char *path)
{
    if (!under(path,data_root)) return 0;
    const char *relative = path + strlen(data_root);
    static const char *const folders[] = {"/AppData/Roaming/Bioshock", "/Documents/Bioshock"};
    for (unsigned i = 0; i < sizeof folders / sizeof *folders; ++i) {
        size_t n = strlen(folders[i]);
        if (strncasecmp(relative,folders[i],n)) continue;
        if (!relative[n]) return 1;
        if (relative[n] != '/' || strchr(relative+n+1,'/')) continue;
        const char *extension = strrchr(relative+n+1,'.');
        if (extension && (!strcasecmp(extension,".ini") || !strcasecmp(extension,".int") ||
            !strcasecmp(extension,".rus") || !strcasecmp(extension,".deu") || !strcasecmp(extension,".fra") ||
            !strcasecmp(extension,".esp") || !strcasecmp(extension,".ita"))) return 1;
    }
    return 0;
}

/* Only the installation and the port's overlay: the selected profile may live
 * inside the installation, but changes outside this cache's view. */
static int cacheable(const char *dir)
{
    if (under(dir, data_root)) {
        const char *relative = dir + strlen(data_root);
        /* Immutable fallback asset searches; settings and saves stay uncached. */
        return profile_configuration(dir) || !strncasecmp(relative, "/AppData/Roaming/FlashMovies", 26) ||
               !strncasecmp(relative, "/Documents/FlashMovies", 22);
    }
    return under(dir, game_root) || under(dir, overlay_root);
}

/* Installation archives are immutable during normal play. A writer opts this
 * process out of read-ahead, including while writable mapped views stay open. */
static int archive_cache_disabled;
static int archive_path(const char *path)
{
    size_t n = path ? strlen(path) : 0;
    return n >= 4 && cacheable(path) && !strcasecmp(path + n - 4, ".pak");
}
static void archive_changed(const char *path)
{
    if (archive_path(path)) __atomic_store_n(&archive_cache_disabled, 1, __ATOMIC_RELEASE);
}

/* Bumped whenever a directory listing is dropped or forgotten: the negative stat cache below keeps an entry
 * only for the generation it was made in. */
static uint32_t listing_generation;

static void listing_free(dir_listing *l)
{
    __atomic_add_fetch(&listing_generation, 1, __ATOMIC_RELEASE);
    if (l->watch >= 0) inotify_rm_watch(listing_watch_fd,l->watch);
    for (int i = 0; i < l->count; ++i) free(l->entries[i].name);
    free(l->entries);
    free(l->path);
    free(l);
}

/* Called with listing_lock: external edits invalidate watched configuration
 * metadata too. Save directories always use the live filesystem. */
static void listing_watch_drain(void)
{
    if (listing_watch_fd < 0) return;
    union { struct inotify_event alignment; char bytes[4096]; } events;
    int changed = 0;
    ssize_t size;
    while ((size = read(listing_watch_fd,events.bytes,sizeof events.bytes)) > 0) {
        for (size_t at = 0; at + sizeof(struct inotify_event) <= (size_t)size;) {
            const struct inotify_event *event = (const void *)(events.bytes+at);
            if ((event->mask & ~IN_IGNORED) && (!(event->mask & IN_ACCESS) ||
                (event->len && !(event->mask & IN_ISDIR)))) changed = 1;
            at += sizeof *event + event->len;
        }
    }
    if (size < 0 && errno != EAGAIN && errno != EINTR) changed = 1;
    if (changed) for (unsigned i = 0; i < LISTING_BUCKETS; ++i)
        for (dir_listing **slot = &listings[i]; *slot;) {
            dir_listing *l = *slot;
            if (l->watch >= 0) { *slot = l->next; listing_free(l); }
            else slot = &l->next;
        }
}

/* Caller holds listing_lock. NULL when the directory cannot be read. */
static dir_listing *listing_get(const char *dir, int load)
{
    listing_watch_drain();
    size_t length = strlen(dir);
    uint64_t key = path_key(dir, length);
    dir_listing **slot = &listings[key % LISTING_BUCKETS];
    for (dir_listing *l = *slot; l; l = l->next)
        if (l->key == key && !strcmp(l->path, dir)) { ++listing_hits; return l; }
    if (!load) return NULL;
    int watch = -1;
    if (profile_configuration(dir)) {
        if (listing_watch_fd == -2) listing_watch_fd = inotify_init1(IN_CLOEXEC|IN_NONBLOCK);
        if (listing_watch_fd < 0) return NULL;
        watch = inotify_add_watch(listing_watch_fd,dir,IN_CREATE|IN_DELETE|IN_MOVED_FROM|IN_MOVED_TO|
            IN_MODIFY|IN_ATTRIB|IN_CLOSE_WRITE|IN_ACCESS|IN_DELETE_SELF|IN_MOVE_SELF|IN_UNMOUNT);
        if (watch < 0) return NULL; /* No notifications: keep live lookups. */
    }
    DIR *d = opendir(length ? dir : "/");
    if (!d) {
        struct stat st;
        int saved_errno = errno;
        if ((void_obb_stat(dir, &st) || !S_ISDIR(st.st_mode)) && saved_errno != ENOENT && saved_errno != ENOTDIR) {
            if (watch >= 0) inotify_rm_watch(listing_watch_fd,watch);
            return NULL;
        }
    }
    dir_listing *l = (dir_listing *)calloc(1, sizeof *l);
    if (!l) { if (d) closedir(d); if (watch >= 0) inotify_rm_watch(listing_watch_fd,watch); return NULL; }
    l->watch = watch;
    int capacity = 0;
    for (unsigned index = 0;;) {
        const char *name;
        unsigned char kind;
        if (d) {
            struct dirent *e = readdir(d);
            if (!e) break;
            name = e->d_name;
            kind = e->d_type == DT_DIR ? DT_DIR : e->d_type == DT_REG ? DT_REG : DT_UNKNOWN;
        } else {
            char child[512];
            int directory = 0;
            if (void_obb_list(dir, index++, child, sizeof child, &directory, NULL)) break;
            name = child;
            kind = directory ? DT_DIR : DT_REG;
        }
        if (!strcmp(name, ".") || !strcmp(name, "..")) continue;
        if (l->count == capacity) {
            capacity = capacity ? capacity * 2 : 32;
            l->entries = (listed_entry *)realloc(l->entries, sizeof *l->entries * (size_t)capacity);
        }
        listed_entry *entry = &l->entries[l->count++];
        memset(entry, 0, sizeof *entry);
        entry->name = strdup(name);
        entry->folded = folded_hash(name);
        entry->kind = kind;
    }
    if (d) {
        for (unsigned index = 0;; ++index) {
            char child[512];
            int directory = 0;
            if (void_obb_list(dir, index, child, sizeof child, &directory, NULL)) break;
            int duplicate = 0;
            for (int i = 0; i < l->count && !duplicate; ++i) duplicate = !strcasecmp(l->entries[i].name, child);
            if (duplicate) continue;
            if (l->count == capacity) {
                capacity = capacity ? capacity * 2 : 32;
                l->entries = (listed_entry *)realloc(l->entries, sizeof *l->entries * (size_t)capacity);
            }
            listed_entry *entry = &l->entries[l->count++];
            memset(entry, 0, sizeof *entry);
            entry->name = strdup(child);
            entry->folded = folded_hash(child);
            entry->kind = directory ? DT_DIR : DT_REG;
        }
        closedir(d);
    }
    l->key = key;
    l->path = strdup(dir);
    l->next = *slot;
    *slot = l;
    ++listing_loads;
    return l;
}

static listed_entry *listing_find(dir_listing *l, const char *name)
{
    uint32_t folded = folded_hash(name);
    for (int i = 0; i < l->count; ++i)
        if (l->entries[i].folded == folded && !strcasecmp(l->entries[i].name, name)) return &l->entries[i];
    return NULL;
}

/* A directory's contents changed through this host (created, removed, renamed). */
static void listing_forget(const char *dir)
{
    __atomic_add_fetch(&listing_generation, 1, __ATOMIC_RELEASE);
    size_t length = strlen(dir);
    uint64_t key = path_key(dir, length);
    pthread_mutex_lock(&listing_lock);
    for (dir_listing **slot = &listings[key % LISTING_BUCKETS]; *slot;) {
        dir_listing *l = *slot;
        if (l->key == key && !strcmp(l->path, dir)) { *slot = l->next; listing_free(l); }
        else slot = &l->next;
    }
    pthread_mutex_unlock(&listing_lock);
}

/* A file's parent listing; a removed or renamed directory also drops its subtree. */
static void listing_forget_parent(const char *path, int subtree)
{
    archive_changed(path);
    char parent[2048];
    snprintf(parent, sizeof parent, "%s", path);
    char *slash = strrchr(parent, '/');
    if (slash) *slash = 0;
    listing_forget(parent);
    if (!subtree) return;
    pthread_mutex_lock(&listing_lock);
    for (unsigned b = 0; b < LISTING_BUCKETS; ++b) {
        for (dir_listing **slot = &listings[b]; *slot;) {
            dir_listing *l = *slot;
            if (under(l->path, path)) { *slot = l->next; listing_free(l); }
            else slot = &l->next;
        }
    }
    pthread_mutex_unlock(&listing_lock);
}

/* 1 present (actual spelling and kind returned), 0 absent, -1 not answerable. */
static int listing_lookup(const char *dir, const char *name, char *actual, size_t cap, unsigned char *kind, int load)
{
    if (!cacheable(dir)) return -1;
    pthread_mutex_lock(&listing_lock);
    dir_listing *l = listing_get(dir, load);
    int result = -1;
    if (l) {
        listed_entry *entry = listing_find(l, name);
        result = entry != NULL;
        if (entry) {
            snprintf(actual, cap, "%s", entry->name);
            if (kind) *kind = entry->kind;
        }
    }
    pthread_mutex_unlock(&listing_lock);
    return result;
}

/* stat through the listing: archive enumeration stats every map file. */
static int cached_stat_mode(const char *path, struct stat *out, int load)
{
    char dir[2048];
    snprintf(dir, sizeof dir, "%s", path);
    char *slash = strrchr(dir, '/');
    if (!slash || !cacheable(path)) { if (!load) { errno=EAGAIN; return -1; } return stat(path,out); }
    *slash = 0;
    const char *name = slash + 1;
    pthread_mutex_lock(&listing_lock);
    dir_listing *l = listing_get(dir, load);
    listed_entry *entry = l ? listing_find(l, name) : NULL;
    int result;
    if (l && !entry) {
        errno = ENOENT;
        result = -1;
    } else if (entry && entry->stat_valid) {
        *out = entry->st;
        result = 0;
    } else if (!load) {
        errno = EAGAIN;
        result = -1;
    } else {
        result = stat(path, out);
        if (result) result = void_obb_stat(path, out);
        int e = errno;
        if (!result && entry) { entry->st = *out; entry->stat_valid = 1; }
        errno = e;
    }
    pthread_mutex_unlock(&listing_lock);
    return result;
}

static int cached_stat(const char *path, struct stat *out) { return cached_stat_mode(path,out,1); }

void void_files_set_overlay(const char *dir)
{
    snprintf(overlay_root, sizeof overlay_root, "%s", dir ? dir : "");
}

/* The app's private, fast storage for derived data (program binaries). */
static char cache_root[1024];
void void_files_set_cache(const char *dir) { snprintf(cache_root, sizeof cache_root, "%s", dir ? dir : ""); }
const char *void_cache_dir(void) { return cache_root[0] ? cache_root : NULL; }

/* "[files] ..." counters for the hitch log. */
void void_files_stats(uint64_t *hits, uint64_t *loads)
{
    pthread_mutex_lock(&listing_lock);
    *hits = listing_hits; *loads = listing_loads;
    pthread_mutex_unlock(&listing_lock);
}

static void mkdirs(const char *path)
{
    char tmp[1024];
    snprintf(tmp, sizeof tmp, "%s", path);
    for (char *p = tmp + 1; *p; ++p) {
        if (*p == '/') { *p = 0; mkdir(tmp, 0775); *p = '/'; }
    }
    mkdir(tmp, 0775);
}

static void add_mount(const char *win, const char *host_fmt, const char *base)
{
    mount_t *m = &mounts[mount_count++];
    m->win = win;
    snprintf(m->host, sizeof m->host, host_fmt, base);
    mkdirs(m->host);
}

int void_files_init(const char *game, const char *data)
{
    snprintf(game_root, sizeof game_root, "%s", game);
    snprintf(data_root, sizeof data_root, "%s", data);
    /* Longest prefixes first. */
    add_mount("C:\\Users\\Player\\AppData\\Local\\Temp", "%s/Temp", data);
    add_mount("C:\\Users\\Player\\Documents", "%s/Documents", data);
    add_mount("C:\\Users\\Player\\AppData", "%s/AppData", data);
    add_mount("C:\\Users\\Public\\Documents", "%s/PublicDocuments", data);
    add_mount("C:\\ProgramData", "%s/ProgramData", data);
    add_mount("C:\\Games\\BioShock", "%s", game);
    add_mount("C:", "%s/drive_c", data);
    game_zip = zm_from_env("game");
    if (game_zip) {
        mkdirs(game_root);
        void_log("[void] game archive: %zu files, sidecar %s", zm_count(game_zip), game_root);
    } else if (getenv("PORT_MOUNT_GAME") && *getenv("PORT_MOUNT_GAME")) {
        void_log("[void] game archive index %s is unreadable", getenv("PORT_MOUNT_GAME"));
        return 0;
    }
    struct stat st;
    if ((stat(game_root, &st) && void_obb_stat(game_root, &st)) || !S_ISDIR(st.st_mode)) {
        void_log("[void] game root %s is missing", game_root);
        return 0;
    }
    return 1;
}

/* Normalize a Windows path to "C:\a\b" (no trailing separator, no . or ..).
 * Returns 0 if it cannot be represented. */
static int normalize(const char *in, char *out, size_t cap)
{
    char work[1024];
    size_t n = 0;
    const char *p = in;
    if (!strncmp(p, "\\\\?\\", 4)) p += 4;
    if (((p[0] | 0x20) >= 'a' && (p[0] | 0x20) <= 'z') && p[1] == ':') {
        if (p[2] == '\\' || p[2] == '/') {
            snprintf(work, sizeof work, "C:%s", p + 2);
        } else {
            pthread_mutex_lock(&cwd_lock);
            snprintf(work, sizeof work, "%s\\%s", cwd, p + 2);
            pthread_mutex_unlock(&cwd_lock);
        }
    } else if (p[0] == '\\' || p[0] == '/') {
        snprintf(work, sizeof work, "C:%s", p);
    } else {
        pthread_mutex_lock(&cwd_lock);
        snprintf(work, sizeof work, "%s\\%s", cwd, p);
        pthread_mutex_unlock(&cwd_lock);
    }
    /* Split and rebuild. */
    char *parts[128];
    int count = 0;
    for (char *s = work; *s; ++s) if (*s == '/') *s = '\\';
    char *save = NULL;
    char *tok = strtok_r(work + 2, "\\", &save);
    while (tok) {
        if (!strcmp(tok, ".") || !*tok) {
        } else if (!strcmp(tok, "..")) {
            if (count) --count;
        } else {
            /* Windows strips trailing dots and spaces from components. */
            size_t len = strlen(tok);
            while (len > 1 && (tok[len - 1] == '.' || tok[len - 1] == ' ')) tok[--len] = 0;
            if (count == 128) return 0;
            parts[count++] = tok;
        }
        tok = strtok_r(NULL, "\\", &save);
    }
    n = (size_t)snprintf(out, cap, "C:");
    for (int i = 0; i < count; ++i) {
        n += (size_t)snprintf(out + n, n < cap ? cap - n : 0, "\\%s", parts[i]);
        if (n >= cap) return 0;
    }
    if (count == 0) snprintf(out, cap, "C:\\");
    if (strcasestr(out, "_audio")) {
        static unsigned audio_paths;
        if (audio_paths++ < 256) void_log("[audio-path] %s", out);
    }
    return 1;
}

/* Append component `name` to host path `base`, matching an existing entry
 * case-insensitively. Returns 1 if the entry exists, 0 if it does not. */
static int resolve_component(char *base, size_t cap, const char *name, unsigned char *kind, int cache_only, int directory_component)
{
    size_t len = strlen(base);
    char actual[512];
    int listed = listing_lookup(base, name, actual, sizeof actual, kind, !cache_only);
    /* Reuse known profile directory spellings, while save/config file names,
     * misses and metadata still come from the live filesystem. */
    if (listed < 0 && directory_component && under(base,data_root)) {
        pthread_mutex_lock(&listing_lock);
        dir_listing *l = listing_get(base,!cache_only);
        listed_entry *entry = l ? listing_find(l,name) : NULL;
        if (entry && entry->kind == DT_DIR) {
            snprintf(actual,sizeof actual,"%s",entry->name);
            if (kind) *kind = DT_DIR;
            listed = 1;
        }
        pthread_mutex_unlock(&listing_lock);
    }
    if (listed >= 0) {
        snprintf(base + len, cap - len, "/%s", listed ? actual : name);
        return listed;
    }
    if (cache_only) return -1;
    if (kind) *kind = DT_UNKNOWN;
    snprintf(base + len, cap - len, "/%s", name);
    struct stat st;
    if (!lstat(base, &st)) {
        if (kind) *kind = S_ISDIR(st.st_mode) ? DT_DIR : DT_REG;
        return 1;
    }
    base[len] = 0;
    DIR *d = opendir(len ? base : "/");
    int found = 0;
    if (d) {
        struct dirent *e;
        while ((e = readdir(d))) {
            if (!strcasecmp(e->d_name, name)) {
                snprintf(base + len, cap - len, "/%s", e->d_name);
                found = 1;
                break;
            }
        }
        closedir(d);
    }
    if (!found) snprintf(base + len, cap - len, "/%s", name);
    return found;
}

/* Resolution flags: the port's overlay shadows game files for reading. */
#define RESOLVE_OVERLAY 1
#define RESOLVE_CACHED_ONLY 2

static int mobile_shader_cache(const char *normalized)
{
    const char *enabled = getenv("BIOSHOCK_MOBILE_SHADERS");
    if (!overlay_root[0] || !enabled || strcmp(enabled, "1")) return 0;
    const char *name = strrchr(normalized, '\\');
    return name && (!strcasecmp(name + 1, "ShaderCache.pcs") || !strcasecmp(name + 1, "ShaderCache.pcs10"));
}

static const char *voice_language(void)
{
    const char *code = getenv("PORT_OPTION_VOICE_LANGUAGE");
    if (!code || !strcmp(code, "same")) code = getenv("PORT_OPTION_GAME_LANGUAGE");
    static const char *const codes[] = {"deu","fra","esp","ita","rus"};
    if (code) for (unsigned i = 0; i < sizeof codes / sizeof *codes; ++i)
        if (!strcmp(code, codes[i])) return code;
    return "int";
}

enum { BINK_RUSSIAN_TRACK = 5, BINK_COMPACT_TRACKS = 6 };

/* The pinned Bink 1.x SDK has already loaded the track IDs (+0x250) and the rate/flag words (+0x24C) at
 * this seam. Its match loop keeps a requested ID only when the file lists it, and opens that track's sound
 * only when the flag word has a bit of 0x90000000 (binkw32 0x1800B1D4). Pick Russian ID 5 only when it
 * passes both, and log every open: a movie without sound otherwise leaves no trace. */
/* QA experiment (PlasmiNow crash): BIOSHOCK_BINK_THREADED_IO=0 adds BINKNOTHREADEDIO (0x08000000) to the engine's
 * FBinkMovie opens, so Bink services its reads from BinkDoFrame/BinkWait on the calling thread instead of a
 * background IO thread. Default: the original flags. */
uint32_t void_bioshock_bink_open_flags(uint32_t flags)
{
    static int threaded = -1;
    if (threaded < 0) { const char *e = getenv("BIOSHOCK_BINK_THREADED_IO"); threaded = !e || atoi(e) ? 1 : 0; }
    uint32_t result = threaded ? flags : flags | 0x08000000u;
    static unsigned logs;
    if (logs++ < 8) void_log("[bink] open flags %08X -> %08X", flags, result);
    return result;
}

/* QA (BIOSHOCK_BINK_TRACE=1 in perf-flags.env, off by default): the DoFrame lines, the HBINK at its open compared
 * with its first decode, and a hash of each compressed frame. The opens and closes are logged always. */
static int bink_trace(void)
{
    static int on = -1;
    if (on < 0) { const char *e = getenv("BIOSHOCK_BINK_TRACE"); on = e && atoi(e) ? 1 : 0; }
    return on;
}

/* QA: the HBINK as BinkOpen returned it, compared with the HBINK at its first decode (void_bioshock_bink_decode). */
enum { BINK_SNAP_BYTES = 0x400 };
static uint32_t bink_snap[BINK_SNAP_BYTES / 4], bink_snap_owner, bink_calls_since_open, bink_decodes_since_open;

/* QA (seam 0x1800BAA8, BinkDoFrame after it loaded the HBINK): the first calls after an open with the frame
 * number and +0x2D4, the frame the last BinkDoFrame finished (equal to the frame number: returns at once). */
void void_bioshock_bink_doframe(uint32_t bink, uint32_t esp)
{
    if (!bink_trace() || !bink || !void_vm_readable(bink, 0x300) || bink_calls_since_open >= 4) return;
    ++bink_calls_since_open;
    void_log("[bink-doframe] call %u after open: bink %08X frame %u last %u done %u decode-on %u buffers %08X "
        "caller %08X", bink_calls_since_open, bink, MEM32(bink + 0xC), MEM32(bink + 0x10), MEM32(bink + 0x2D4),
        MEM32(bink + 0x28C), MEM32(bink + 0xB8), void_vm_readable(esp + 0x2C, 4) ? MEM32(esp + 0x2C) : 0);
}

static void bink_snap_diff(uint32_t bink, const char *why)
{
    if (!bink_snap_owner || bink != bink_snap_owner || !void_vm_readable(bink, BINK_SNAP_BYTES)) return;
    bink_snap_owner = 0;
    static unsigned logs;
    if (logs++ >= 64) return;
    char text[1400];
    size_t used = 0;
    unsigned changed = 0;
    for (unsigned i = 0; i < BINK_SNAP_BYTES / 4; ++i) {
        uint32_t now = MEM32(bink + i * 4);
        if (now == bink_snap[i]) continue;
        if (++changed <= 48 && used < sizeof text - 32)
            used += (size_t)snprintf(text + used, sizeof text - used, " +%03X:%08X>%08X", i * 4, bink_snap[i], now);
    }
    void_log("[bink-decode] first decode (%s) bink %08X: %u dwords changed since open%s", why, bink, changed,
        used ? text : "");
}

/* QA: the engine's FBinkMovie lifecycle (seams 0x10BAC904 after BinkOpen, 0x10BAC448 BinkClose, 0x10B91448 the
 * viewport's tick of the menu's background movie): opens and closes always, a tick when its movie or Bink changes. */
void void_bioshock_bink_life(uint32_t what, uint32_t a, uint32_t b)
{
    static uint32_t tick_movie, tick_bink;
    static unsigned logs;
    if (what == 2) {
        uint32_t holder = a && void_vm_readable(a + 8, 4) ? MEM32(a + 8) : 0;
        uint32_t bink = holder && void_vm_readable(holder, 4) ? MEM32(holder) : 0;
        if (a == tick_movie && bink == tick_bink) return;
        tick_movie = a;
        tick_bink = bink;
        if (logs++ < 400) void_log("[bink-life] tick movie %08X (eax %08X) holder %08X bink %08X", a, b, holder, bink);
        return;
    }
    if (logs++ < 400) void_log("[bink-life] %s bink %08X movie %08X thread %u", what ? "close" : "open", a, b, void_thread_id());
    if (!what && a && bink_trace() && void_vm_readable(a, BINK_SNAP_BYTES)) {
        memcpy(bink_snap, GPTR(a), BINK_SNAP_BYTES);
        bink_snap_owner = a;
        bink_calls_since_open = bink_decodes_since_open = 0;
        void_log("[bink-life] open state %ux%u frames %u frame %u last %u done %u table %08X start %08X io-read %08X",
            bink_snap[0], bink_snap[1], bink_snap[2], bink_snap[3], bink_snap[4], bink_snap[0x2D4 / 4],
            bink_snap[0xEC / 4], bink_snap[0xE4 / 4], bink_snap[0xF4 / 4]);
    } else if (what && a == bink_snap_owner) bink_snap_owner = 0;
}

/* QA (seam 0x1800BFA9, BinkDoFrame's call of the video decoder): a hash of each compressed frame, kept per frame
 * number for the movie that is open; a reopen of the same movie must hand the decoder the same bytes per frame.
 * A mismatch is logged with the frame's offsets: then the input is wrong (IO or a stray writer), not the decoder. */
void void_bioshock_bink_decode(uint32_t bink, uint32_t data)
{
    enum { MAX_FRAMES = 8192 };
    static uint32_t sig_w, sig_h, sig_frames, sig_first, ref_hash[MAX_FRAMES], ref_size[MAX_FRAMES];
    static uint32_t last_bink, opens, checked, mismatches;
    if (!bink_trace()) return;
    if (!bink || !void_vm_readable(bink, 0xF0)) {
        void_log("[bink-decode] unreadable bink %08X data %08X", bink, data);
        return;
    }
    uint32_t width = MEM32(bink), height = MEM32(bink + 4), frames = MEM32(bink + 8), n = MEM32(bink + 12);
    uint32_t table = MEM32(bink + 0xEC), start = MEM32(bink + 0xE4);
    int bad = !n || n > frames || n >= MAX_FRAMES || !table || !void_vm_readable(table + (n - 1) * 4, 8);
    bink_snap_diff(bink, bad ? "BAD" : "ok");
    if (bad) {
        static unsigned bad_logs;
        if (bad_logs++ < 32)
            void_log("[bink-decode] BAD state bink %08X: %ux%u frame %u/%u table %08X start %08X data %08X", bink,
                width, height, n, frames, table, start, data);
        return;
    }
    uint32_t from = MEM32(table + (n - 1) * 4) & ~1u, to = MEM32(table + n * 4) & ~1u;
    uint32_t size = to > from ? to - from : 0;
    if (size > (4u << 20) || !start || (size && !void_vm_readable(start, size))) {
        void_log("[bink-decode] bink %08X frame %u/%u odd: start %08X data %08X size %u", bink, n, frames, start, data, size);
        return;
    }
    uint32_t first = MEM32(table);
    if (width != sig_w || height != sig_h || frames != sig_frames || first != sig_first) {
        sig_w = width; sig_h = height; sig_frames = frames; sig_first = first;
        memset(ref_hash, 0, sizeof ref_hash); memset(ref_size, 0, sizeof ref_size);
        void_log("[bink-decode] reference reset: %ux%u, %u frames, first offset %08X", width, height, frames, first);
    }
    if (bink != last_bink || n == 1) { last_bink = bink; ++opens; }
    uint32_t hash = 2166136261u;
    const uint8_t *p = (const uint8_t *)GPTR(start);
    for (uint32_t i = 0; i < size; ++i) hash = (hash ^ p[i]) * 16777619u;
    if (!hash) hash = 1;
    ++checked;
    if (!ref_hash[n]) { ref_hash[n] = hash; ref_size[n] = size; }
    else if (ref_hash[n] != hash || ref_size[n] != size) {
        if (mismatches++ < 200)
            void_log("[bink-decode] MISMATCH open %u bink %08X frame %u/%u key %u: start %08X data %08X off %08X..%08X "
                "size %u hash %08X, reference size %u hash %08X; bytes %08X %08X %08X %08X", opens, bink, n, frames,
                MEM32(table + (n - 1) * 4) & 1, start, data, from, to, size, hash, ref_size[n], ref_hash[n],
                size >= 16 ? MEM32(start) : 0, size >= 16 ? MEM32(start + 4) : 0, size >= 16 ? MEM32(start + 8) : 0,
                size >= 16 ? MEM32(start + 12) : 0);
    }
    if (n <= 3 || bink_decodes_since_open++ < 3 || !(checked % 600))
        void_log("[bink-decode] open %u bink %08X frame %u/%u start %08X data %08X size %u hash %08X (checked %u, "
            "mismatches %u)", opens, bink, n, frames, start, data, size, hash, checked, mismatches);
}

void void_bioshock_bink_select_track(uint32_t bink)
{
    uint32_t count = MEM32(0x1802F068), requested = MEM32(0x1802F06C);
    uint32_t tracks = MEM32(bink + 0xD0), ids = MEM32(bink + 0x250), words = MEM32(bink + 0x24C);
    const char *language = voice_language();
    char list[128] = "";
    size_t used = 0;
    int russian = 0;
    if (ids && words && tracks <= 16)
        for (uint32_t i = 0; i < tracks; ++i) {
            uint32_t id = MEM32(ids + i * 4), word = MEM32(words + i * 4);
            if (used < sizeof list)
                used += (size_t)snprintf(list + used, sizeof list - used, "%s%u:%04X", i ? " " : "", id, word >> 16);
            if (id == BINK_RUSSIAN_TRACK && (word & 0x90000000u)) russian = 1;
        }
    uint32_t chosen = requested;
    if (count == 1 && !strcmp(language, "rus") && tracks == BINK_COMPACT_TRACKS && russian)
        chosen = MEM32(0x1802F06C) = BINK_RUSSIAN_TRACK;
    void_log("[bink] open %08X: %u tracks [%s], request %u x id %u, voice %s -> id %u",
        bink, tracks, list, count, requested, language, chosen);
}

uint32_t void_bioshock_bink_volume_track(uint32_t bink, uint32_t requested)
{
    if (!bink || MEM32(bink + 0xD0) != BINK_COMPACT_TRACKS || MEM32(bink + 0x2E8) != 1) return requested;
    uint32_t index = MEM32(MEM32(bink + 0x244));
    uint32_t ids = MEM32(bink + 0x250);
    return index < BINK_COMPACT_TRACKS && ids && MEM32(ids + index * 4) == BINK_RUSSIAN_TRACK
        ? BINK_RUSSIAN_TRACK : requested;
}

/* Unreal chooses these banks with its text language. Keep the engine's text
 * tables and substitute only the banks carrying speech. */
static const char voice_sounds[] = "C:\\Games\\BioShock\\Content\\Sounds_Windows";
static const char *const voice_banks[] = {"0", "1", "2", "3", "4", "5", "6", "7", "99", "aivo"};

static int voice_bank_name(const char *name, const char *dot)
{
    if (dot - name <= 14 || strncasecmp(name, "streams_", 8) || strncasecmp(dot - 6, "_audio", 6)) return 0;
    size_t length = (size_t)(dot - name - 14);
    for (unsigned i = 0; i < sizeof voice_banks / sizeof *voice_banks; ++i)
        if (strlen(voice_banks[i]) == length && !strncasecmp(name + 8, voice_banks[i], length)) return 1;
    return 0;
}

static const char *voice_bank_path(const char *path, char mapped[1024])
{
    const char *ext = path ? strrchr(path, '.') : NULL;
    if (!ext || (strcasecmp(ext, ".bsm") && strcasecmp(ext, ".fsb") &&
                 (strlen(ext) != 8 || strcasecmp(ext + 4, "_fsb")))) return path;
    const char *voice = voice_language();
    const char *text = getenv("PORT_OPTION_GAME_LANGUAGE");
    if (!text) text = "int";
    if (!strcmp(voice, text)) return path;

    static const char maps[] = "C:\\Games\\BioShock\\Content\\Maps\\";
    char norm[1024];
    if (!normalize(path, norm, sizeof norm)) return path;
    char *name = strrchr(norm, '\\') + 1;
    char *dot = strrchr(name, '.');
    if (!dot) return path;
    if (name == norm + sizeof maps - 1 && !strncasecmp(norm, maps, sizeof maps - 1) &&
        !strcasecmp(dot, ".bsm") && dot - name > 4 && dot[-4] == '_' &&
        (!strncasecmp(dot - 3, text, 3) || !strncasecmp(dot - 3, "int", 3))) {
        snprintf(mapped, 1024, "%.*s%s.bsm", (int)(dot - norm - 3), norm, voice);
        return mapped;
    }
    if (name == norm + sizeof voice_sounds && !strncasecmp(norm, voice_sounds, sizeof voice_sounds - 1) &&
        voice_bank_name(name, dot)) {
        snprintf(mapped, 1024, "%.*s.%s%s", (int)(dot - norm), norm,
                 !strcmp(voice, "int") ? "" : voice, !strcmp(voice, "int") ? "fsb" : "_fsb");
        return mapped;
    }
    return path;
}

/* The path below a mount, resolved component by component under `root`.
 * Returns 1 if the leaf exists, 0 if not, with *parent_ok as below. */
static int resolve_below(char *host, size_t cap, const char *root, const char *rest, int *parent_ok,
                         unsigned char *leaf_kind, int cache_only)
{
    snprintf(host, cap, "%s", root);
    int ok = 1, leaf = 1;
    char component[512];
    if (leaf_kind) *leaf_kind = DT_DIR;
    while (*rest) {
        while (*rest == '\\') ++rest;
        if (!*rest) break;
        size_t k = 0;
        while (rest[k] && rest[k] != '\\' && k < sizeof component - 1) { component[k] = rest[k]; ++k; }
        component[k] = 0;
        rest += k;
        int last = !*rest || (rest[0] == '\\' && !rest[1]);
        if (!ok) {
            size_t len = strlen(host);
            snprintf(host + len, cap - len, "/%s", component);
            leaf = 0;
        } else {
            unsigned char kind = DT_UNKNOWN;
            int exists = resolve_component(host, cap, component, &kind, cache_only, !last);
            if (exists < 0) return -1;
            if (!exists && !last) ok = 0;
            if (last) { leaf = exists; if (leaf_kind) *leaf_kind = kind; }
        }
    }
    if (parent_ok) *parent_ok = ok;
    return ok && leaf;
}

/* Startup runs before mount initialization; resolve against its explicit
 * profile so existing cache directories retain their actual spelling. */
int void_files_cache_directory(const char *profile, const char *name, char *path, size_t cap)
{
    int parent = 0;
    if (resolve_below(path, cap, profile, name, &parent, NULL, 0) < 0 || !parent) return 0;
    if (mkdir(path, 0775) && errno != EEXIST) return 0;
    struct stat st;
    return !stat(path, &st) && S_ISDIR(st.st_mode);
}

static int read_bink_u32(FILE *file, uint32_t *value)
{
    unsigned char b[4];
    if (fread(b, 1, sizeof b, file) != sizeof b) return 0;
    *value = b[0] | (uint32_t)b[1] << 8 | (uint32_t)b[2] << 16 | (uint32_t)b[3] << 24;
    return 1;
}

static int bink_basename(const char *name, size_t size)
{
    return size > 4 && size <= 104 &&
        strspn(name, "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_-") == size - 4 &&
        !strcasecmp(name + size - 4, ".bik");
}

static int copy_bink_segment(FILE *source, FILE *out, uint32_t count, uLong *crc)
{
    unsigned char bytes[16384];
    while (count) {
        size_t n = count < sizeof bytes ? count : sizeof bytes;
        if (fread(bytes, 1, n, source) != n || fwrite(bytes, 1, n, out) != n) return 0;
        *crc = crc32(*crc, bytes, (uInt)n);
        count -= (uint32_t)n;
    }
    return 1;
}

/* BKA1 stores the original localized header and audio packets, with ranges of
 * unchanged encoded video from a base movie. Cache a verified Bink for the SDK. */
static int localized_bink(char *host, const char *movie, int cache_only)
{
    char sidecar[1024];
    snprintf(sidecar, sizeof sidecar, "%.*s.bka", (int)strlen(movie) - 4, movie);
    if (resolve_below(host, 2048, game_root, sidecar, NULL, NULL, cache_only) <= 0 || cache_only) return 0;
    FILE *audio = fopen(host, "rb");
    if (!audio) return 0;
    unsigned char magic[4];
    uint32_t name_size, frames, size, expected;
    char base_name[105];
    int ok = fread(magic, 1, 4, audio) == 4 && !memcmp(magic, "BKA1", 4) &&
        read_bink_u32(audio, &name_size) && name_size > 4 && name_size < sizeof base_name &&
        read_bink_u32(audio, &frames) && frames && frames <= 1000000 &&
        read_bink_u32(audio, &size) && size && size <= 256u * 1024u * 1024u &&
        read_bink_u32(audio, &expected) &&
        fread(base_name, 1, name_size, audio) == name_size;
    if (!ok) { fclose(audio); return 0; }
    base_name[name_size] = 0;
    if (!bink_basename(base_name, name_size)) {
        fclose(audio);
        return 0;
    }
    char folder[2048], base[160], original[2048];
    snprintf(folder, sizeof folder, "%s/bink-voice", data_root);
    snprintf(host, 2048, "%s/%08x-%s", folder, expected, strrchr(movie, '\\') + 1);
    struct stat st;
    if (!stat(host, &st) && S_ISREG(st.st_mode) && st.st_size == size) {
        fclose(audio);
        return 1;
    }
    snprintf(base, sizeof base, "\\Content\\BinkMovies\\%s", base_name);
    if (resolve_below(original, sizeof original, game_root, base, NULL, NULL, 0) <= 0 ||
        (mkdir(folder, 0775) && errno != EEXIST)) {
        fclose(audio);
        return 0;
    }
    FILE *video = fopen(original, "rb");
    if (!video) { fclose(audio); return 0; }
    char draft[2048];
    snprintf(draft, sizeof draft, "%s/.tmp-XXXXXX", folder);
    int fd = mkstemp(draft);
    FILE *out = fd < 0 ? NULL : fdopen(fd, "wb");
    if (fd >= 0 && !out) close(fd);
    uLong crc = 0;
    uint32_t remaining = size;
    ok = out && !fstat(fileno(video), &st) && S_ISREG(st.st_mode);
    for (uint32_t i = 0; ok && i < frames; ++i) {
        uint32_t literal, offset, copy;
        ok = read_bink_u32(audio, &literal) && read_bink_u32(audio, &offset) &&
             read_bink_u32(audio, &copy) && literal <= remaining && copy && copy <= remaining - literal &&
             (uint64_t)offset + copy <= (uint64_t)st.st_size &&
             copy_bink_segment(audio, out, literal, &crc) && !fseeko(video, offset, SEEK_SET) &&
             copy_bink_segment(video, out, copy, &crc);
        if (ok) remaining -= literal + copy;
    }
    ok = ok && !remaining && crc == expected && fgetc(audio) == EOF && !ferror(audio);
    if (out && fclose(out)) ok = 0;
    fclose(video);
    fclose(audio);
    if (ok && !rename(draft, host)) return 1;
    if (fd >= 0) unlink(draft);
    void_log("[bink] compact audio failed for %s", movie);
    return 0;
}

/* Windows path (UTF-8) -> host path. *exists_parent reports whether every
 * component but the last exists (ERROR_PATH_NOT_FOUND vs FILE_NOT_FOUND);
 * *leaf reports the last one: 1 present, 0 absent. */
static char *to_host_ex(const char *windows_path, int flags, int *parent_exists, int *leaf, char *normalized_out)
{
    char norm[1024];
    if (!windows_path || !normalize(windows_path, norm, sizeof norm)) return NULL;
    if (normalized_out) snprintf(normalized_out, 1024, "%s", norm);
    const mount_t *best = NULL;
    size_t best_len = 0;
    for (int i = 0; i < mount_count; ++i) {
        size_t l = strlen(mounts[i].win);
        if (!strncasecmp(norm, mounts[i].win, l) && (norm[l] == 0 || norm[l] == '\\') && l > best_len) {
            best = &mounts[i];
            best_len = l;
        }
    }
    if (!best) return NULL;
    int cache_only = (flags & RESOLVE_CACHED_ONLY) != 0;
    if (cache_only && !cacheable(best->host) && !under(best->host,data_root)) return NULL;
    char *host = (char *)malloc(2048);
    const char *rest = norm + best_len;
    /* Optional language movies keep the English originals intact. */
    char movie[1024], suffix[9] = "";
    const char *language = voice_language();
    if (strcmp(language, "int")) snprintf(suffix,sizeof suffix,"_%s.bik",language);
    size_t movie_length = strlen(rest);
    if ((flags & RESOLVE_OVERLAY) && !strcmp(best->host, game_root) && suffix[0] &&
        !strncasecmp(rest, "\\Content\\BinkMovies\\", 20) && movie_length > 4 && movie_length + 5 < sizeof movie &&
        !strcasecmp(rest + movie_length - 4, ".bik") && !strcasestr(rest,suffix)) {
        snprintf(movie, sizeof movie, "%.*s%s", (int)movie_length - 4, rest,suffix);
        if (resolve_below(host, 2048, game_root, movie, NULL, NULL, cache_only) > 0 ||
            localized_bink(host, movie, cache_only)) {
            if (parent_exists) *parent_exists = 1;
            if (leaf) *leaf = 1;
            return host;
        }
    }
    if (mobile_shader_cache(norm)) {
        const char *name = !strcasecmp(strrchr(norm, '\\') + 1, "ShaderCache.pcs10") ? "ShaderCache.pcs10" : "ShaderCache.pcs";
        char relative[80];
        snprintf(relative, sizeof relative, "ShaderCache-mobile-v3\\%s", name);
        int parent = 0;
        int present = resolve_below(host, 2048, data_root, relative, &parent, NULL, cache_only);
        if (present < 0) { free(host); return NULL; }
        if (parent_exists) *parent_exists = parent;
        if (leaf) *leaf = present;
        return host;
    }
    /* A regular file of the overlay replaces the installation's for reading. */
    if ((flags & RESOLVE_OVERLAY) && overlay_root[0] && !strcmp(best->host, game_root) && *rest) {
        int overlay_parent = 0;
        unsigned char kind = DT_UNKNOWN;
        int present = resolve_below(host, 2048, overlay_root, rest, &overlay_parent, &kind, cache_only);
        if (present < 0) { free(host); return NULL; }
        if (present && kind != DT_DIR) {
            if (parent_exists) *parent_exists = 1;
            if (leaf) *leaf = 1;
            return host;
        }
    }
    int ok = 1;
    int present = resolve_below(host, 2048, best->host, rest, &ok, NULL, cache_only);
    if (present < 0) { free(host); return NULL; }
    if (!present && !strcmp(best->host, game_root)) {
        static unsigned misses;
        if (__atomic_fetch_add(&misses, 1, __ATOMIC_RELAXED) < 128) void_log("[obb-miss] %s -> %s", norm, host);
    }
    if (parent_exists) *parent_exists = ok;
    if (leaf) *leaf = present;
    return host;
}

static char *to_host(const char *windows_path, int *parent_exists, char *normalized_out)
{
    return to_host_ex(windows_path, 0, parent_exists, NULL, normalized_out);
}

static int zip_any(const zm_entry *e, void *user) { (void)e; (void)user; return 1; }

static int zip_lookup(const char *windows_path, const zm_entry **entry, int localized_movies)
{
    static const char game_mount[] = "C:\\Games\\BioShock";
    const size_t length = sizeof game_mount - 1;
    char norm[1024];
    if (!game_zip || !windows_path || !normalize(windows_path, norm, sizeof norm) ||
        strncasecmp(norm, game_mount, length) || (norm[length] && norm[length] != '\\')) return 0;
    const char *relative = norm + length;
    const char *language = voice_language();
    size_t size = strlen(relative);
    const zm_entry *e = NULL;
    if (localized_movies && strcmp(language, "int") &&
        !strncasecmp(relative, "\\Content\\BinkMovies\\", 20) &&
        size > 4 && size + 5 < sizeof norm && !strcasecmp(relative + size - 4, ".bik")) {
        char movie[1024];
        snprintf(movie, sizeof movie, "%.*s_%s.bik", (int)size - 4, relative, language);
        e = zm_find(game_zip, movie);
        if (e && (e->flags & ZM_SIDECAR)) e = NULL;
    }
    if (!e) e = zm_find(game_zip, relative);
    if (e) {
        if (e->flags & ZM_SIDECAR) return 0;
        if (entry) *entry = e;
        return 1;
    }
    return zm_list(game_zip, norm + length, zip_any, NULL) ? 2 : 0;
}

/* The console Maps and InGameManual movies (maps.swf, ingamemanual.swf), which the game loads while a
 * gamepad was used last, answer only controller commands, so a finger cannot pan the map or pick a list
 * row, and their frame overruns a wide screen. The PC movies define the same UIHighlight*, AcceptOrSelect,
 * UIGeneric*, UpdateRadialAxis and UpLevel functions the *UIActive XENON_* bindings call, plus the PC
 * pointer, so a gamepad and touch both work in them: always open the PC movie. */
static int flash_pc_movie(const char *path, char out[1024])
{
    static const char folder[] = "\\Content\\FlashMovies\\";
    static const char *const names[] = {"maps", "ingamemanual"};
    char norm[1024];
    if (!path || !normalize(path, norm, sizeof norm)) return 0;
    const char *slash = strrchr(norm, '\\');
    if (!slash) return 0;
    size_t prefix = (size_t)(slash + 1 - norm);
    if (prefix < sizeof folder - 1 || strncasecmp(slash + 1 - (sizeof folder - 1), folder, sizeof folder - 1))
        return 0;
    for (unsigned i = 0; i < sizeof names / sizeof *names; ++i) {
        size_t length = strlen(names[i]);
        if (!strncasecmp(slash + 1, names[i], length) && !strcasecmp(slash + 1 + length, ".swf")) {
            snprintf(out, 1024, "%.*s%sPC.swf", (int)prefix, norm, names[i]);
            static unsigned logged;
            if (__atomic_fetch_add(&logged, 1, __ATOMIC_RELAXED) < 16)
                void_log("[flash] %s -> %s: touch and gamepad", norm, out);
            return 1;
        }
    }
    return 0;
}

/* A Russian movie may be identical to the Russian track of a different base movie.
 * The pack records that mapping as BKR1 followed by one sibling Bink filename. */
static int bink_voice_alias(const char *path, char out[1024])
{
    static const char prefix[] = "C:\\Games\\BioShock\\Content\\BinkMovies\\";
    const char *ext = path ? strrchr(path, '.') : NULL;
    if (!ext || strcasecmp(ext, ".bik") || strcmp(voice_language(), "rus")) return 0;
    char normalized[1024];
    if (!normalize(path, normalized, sizeof normalized) ||
        strncasecmp(normalized, prefix, sizeof prefix - 1)) return 0;
    const char *name = normalized + sizeof prefix - 1;
    size_t name_size = strlen(name);
    if (!bink_basename(name, name_size)) return 0;
    char marker[160];
    snprintf(marker, sizeof marker, "%s%.*s_rus.bkr", prefix, (int)name_size - 4, name);
    char *host = to_host(marker, NULL, NULL);
    int fd = host ? open(host, O_RDONLY | O_NONBLOCK | O_CLOEXEC | O_NOFOLLOW) : -1;
    free(host);
    char data[110];
    ssize_t size = -1;
    if (fd >= 0) {
        size = pread(fd, data, sizeof data, 0);
        close(fd);
    } else {
        const zm_entry *entry;
        if (zip_lookup(marker, &entry, 0) == 1 && entry->size < sizeof data)
            size = pread(zm_fd(game_zip), data, (size_t)entry->size, (off_t)entry->offset);
    }
    if (size <= 4 || size >= (ssize_t)sizeof data || memcmp(data, "BKR1", 4)) return 0;
    data[size] = 0;
    if (!bink_basename(data + 4, (size_t)size - 4) || !strcasecmp(name, data + 4)) return 0;
    snprintf(out, 1024, "%s%s", prefix, data + 4);
    return 1;
}

static void zip_stat(const zm_entry *e, struct stat *st)
{
    memset(st, 0, sizeof *st);
    st->st_mode = e ? S_IFREG | 0444 : S_IFDIR | 0755;
    if (e) st->st_size = (off_t)e->size;
    if (e) {
        st->st_mtim.tv_sec = (time_t)(e->time / 1000u);
        st->st_mtim.tv_nsec = (long)(e->time % 1000u) * 1000000L;
        st->st_atim = st->st_ctim = st->st_mtim;
    }
}

/* FSD1 from tools/compact_voice_banks.py. Read unchanged ranges from the
 * English bank and literals from the delta. No reconstructed file on disk. */
#define VOICE_DELTA_ENGLISH UINT32_C(0x80000000)
typedef struct { uint32_t end, source; } voice_extent;
typedef struct {
    int base_fd;
    uint64_t base_bias, patch_bias;
    uint32_t size, count;
    voice_extent extents[];
} voice_delta;

static int voice_delta_path(const char *path, char delta[1024])
{
    const char *ext = path ? strrchr(path, '.') : NULL;
    if (!ext || strcasecmp(ext, ".rus_fsb") || !normalize(path, delta, 1024) ||
        strncasecmp(delta, voice_sounds, sizeof voice_sounds - 1) || delta[sizeof voice_sounds - 1] != '\\') return 0;
    char *name = delta + sizeof voice_sounds;
    char *dot = strrchr(name, '.');
    if (!dot || strchr(name, '\\') || !voice_bank_name(name, dot)) return 0;
    strcpy(dot, ".rus_fsd");
    return 1;
}

static int game_asset_source(const char *path, obb_file *file)
{
    const zm_entry *entry;
    if (zip_lookup(path, &entry, 0) == 1) {
        *file = (obb_file){.fd = dup(zm_fd(game_zip)), .bias = entry->offset, .size = entry->size};
        return file->fd >= 0;
    }
    int leaf = 0;
    char *host = to_host_ex(path, RESOLVE_OVERLAY, NULL, &leaf, NULL);
    if (!void_obb_lookup(host && under(host, game_root) ? host : path, file)) {
        free(host);
        return 1;
    }
    int fd = host && leaf ? open(host, O_RDONLY | O_NONBLOCK | O_CLOEXEC | O_NOFOLLOW) : -1;
    free(host);
    if (fd < 0) return 0;
    struct stat st;
    if (fstat(fd, &st) || !S_ISREG(st.st_mode)) { close(fd); return 0; }
    *file = (obb_file){.fd = fd, .size = (uint64_t)st.st_size};
    return 1;
}

static int bulk_file(const char *path)
{
    static const char prefix[] = "C:\\Games\\BioShock\\Content\\BulkContent\\";
    if (!path || strncasecmp(path, prefix, sizeof prefix - 1)) return 0;
    const char *name = path + sizeof prefix - 1;
    const char *ext = strrchr(name, '.');
    return !strchr(name, '\\') && ext && ext != name && !strcasecmp(ext, ".blk");
}

#define BULK_PREFETCH_DEPTH 16u
#define BULK_PREFETCH_MAX (2u * 1024 * 1024)
typedef struct { char path[1024]; uint32_t start, size; } bulk_range;
static bulk_range bulk_queue[BULK_PREFETCH_DEPTH];
static unsigned bulk_head, bulk_count;
static int bulk_started;
static pthread_mutex_t bulk_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t bulk_ready = PTHREAD_COND_INITIALIZER;

static void *bulk_prefetch_worker(void *ignored)
{
    (void)ignored;
    extern int void_native_worker_affinity_apply(void);
    (void)void_native_worker_affinity_apply();
    char active[1024] = "";
    obb_file file = { .fd = -1 };
    int reported = 0;
    for (;;) {
        pthread_mutex_lock(&bulk_lock);
        while (!bulk_count) pthread_cond_wait(&bulk_ready, &bulk_lock);
        bulk_range range = bulk_queue[bulk_head];
        bulk_head = (bulk_head + 1) % BULK_PREFETCH_DEPTH;
        --bulk_count;
        pthread_mutex_unlock(&bulk_lock);
        if (strcasecmp(active, range.path)) {
            if (file.fd >= 0) close(file.fd);
            file.fd = -1;
            active[0] = 0;
            if (!game_asset_source(range.path, &file)) { file.fd = -1; continue; }
            strcpy(active, range.path);
        }
        if (range.start >= file.size) continue;
        uint64_t length = file.size - range.start;
        if (length > range.size) length = range.size;
        int error = posix_fadvise(file.fd, (off_t)(file.bias + range.start), (off_t)length, POSIX_FADV_WILLNEED);
        if (!reported) {
            reported = 1;
            void_log("[files] bulk read-ahead %s (%s, offset %u, %llu bytes)",
                     error ? strerror(error) : "advised", range.path, range.start, (unsigned long long)length);
        }
    }
}

/* These offsets come from the original content worker's archive requests. */
void void_bioshock_prefetch_bulk(uint32_t filename, uint32_t start, uint32_t end)
{
    char path[WIN_MAX_PATH];
    if (end <= start || end - start > BULK_PREFETCH_MAX || !void_vm_readable(filename, sizeof path * 2)) return;
    unsigned length = 0;
    while (length < sizeof path) {
        uint16_t unit = MEM16(filename + length * 2);
        if (unit > 0x7f) return;
        path[length] = (char)unit;
        if (!unit) break;
        ++length;
    }
    if (length == sizeof path) return;
    bulk_range range = { .start = start, .size = end - start };
    if (!normalize(path, range.path, sizeof range.path) || !bulk_file(range.path)) return;

    if (pthread_mutex_trylock(&bulk_lock)) return;
    if (!bulk_started) {
        pthread_t thread;
        bulk_started = pthread_create(&thread, NULL, bulk_prefetch_worker, NULL) ? -1 : 1;
        if (bulk_started == 1) pthread_detach(thread);
    }
    if (bulk_started == 1 && bulk_count < BULK_PREFETCH_DEPTH) {
        unsigned tail = (bulk_head + bulk_count) % BULK_PREFETCH_DEPTH;
        bulk_queue[tail] = range;
        ++bulk_count;
        pthread_cond_signal(&bulk_ready);
    }
    pthread_mutex_unlock(&bulk_lock);
}

static int voice_read_exact(int fd, void *out, size_t length, uint64_t position)
{
    uint8_t *bytes = out;
    while (length) {
        ssize_t got = pread(fd, bytes, length, (off_t)position);
        if (got < 0 && errno == EINTR) continue;
        if (got <= 0) { if (!got) errno = EIO; return 0; }
        bytes += got;
        length -= (size_t)got;
        position += (uint64_t)got;
    }
    return 1;
}

static int voice_delta_header(const obb_file *patch, uint32_t header[5])
{
    const unsigned header_size = 5 * sizeof *header;
    return patch->size >= header_size && patch->size <= (256u << 20) &&
        voice_read_exact(patch->fd, header, header_size, patch->bias) && !memcmp(header, "FSD1", 4) &&
        header[1] && header[1] <= (256u << 20) && header[2] && header[2] <= (256u << 20) &&
        header[4] && header[4] <= 65536 && header_size + header[4] * sizeof(voice_extent) <= patch->size;
}

static int voice_delta_stat(const char *path, struct stat *st)
{
    char delta_path[1024];
    obb_file patch;
    uint32_t header[5];
    if (!voice_delta_path(path, delta_path) || !game_asset_source(delta_path, &patch)) return 0;
    int ok = voice_delta_header(&patch, header);
    close(patch.fd);
    if (!ok) return 0;
    memset(st, 0, sizeof *st);
    st->st_mode = S_IFREG | 0444;
    st->st_size = header[1];
    return 1;
}

static voice_delta *voice_delta_open(char base_path[1024], const obb_file *patch)
{
    uint32_t header[5];
    if (!voice_delta_header(patch, header)) return NULL;
    strcpy(strrchr(base_path, '.'), ".fsb");
    obb_file base;
    if (!game_asset_source(base_path, &base)) return NULL;
    voice_delta *delta = NULL;
    unsigned char prefix[65536];
    unsigned prefix_size = base.size < sizeof prefix ? (unsigned)base.size : sizeof prefix;
    /* shortcut: the prefix identifies the shipped banks, check full hashes if accepting modified bases. */
    if (base.size != header[2] || !voice_read_exact(base.fd, prefix, prefix_size, base.bias) ||
        crc32(0, prefix, prefix_size) != header[3]) goto invalid;
    size_t bytes = header[4] * sizeof(voice_extent);
    delta = malloc(sizeof *delta + bytes);
    if (!delta || !voice_read_exact(patch->fd, delta->extents, bytes, patch->bias + sizeof header)) goto invalid;
    uint32_t end = 0;
    uint64_t literal_offset = sizeof header + bytes;
    for (uint32_t i = 0; i < header[4]; ++i) {
        voice_extent extent = delta->extents[i];
        if (extent.end <= end || extent.end > header[1]) goto invalid;
        uint32_t length = extent.end - end;
        if (extent.source & VOICE_DELTA_ENGLISH) {
            if ((extent.source & ~VOICE_DELTA_ENGLISH) + (uint64_t)length > base.size) goto invalid;
        } else {
            if (extent.source != literal_offset || literal_offset + length > patch->size) goto invalid;
            literal_offset += length;
        }
        end = extent.end;
    }
    if (end != header[1] || literal_offset != patch->size) goto invalid;
    delta->base_fd = base.fd;
    delta->base_bias = base.bias;
    delta->patch_bias = patch->bias;
    delta->size = header[1];
    delta->count = header[4];
    return delta;

invalid:
    close(base.fd);
    free(delta);
    return NULL;
}

static ssize_t voice_delta_pread(const voice_delta *delta, int patch_fd, void *out, size_t size, uint64_t position)
{
    if (position >= delta->size) return 0;
    if (size > delta->size - position) size = (size_t)(delta->size - position);
    uint32_t first = 0, last = delta->count;
    while (first < last) {
        uint32_t middle = first + (last - first) / 2;
        if (delta->extents[middle].end <= position) first = middle + 1;
        else last = middle;
    }
    for (size_t done = 0; done < size; ++first) {
        voice_extent extent = delta->extents[first];
        uint64_t start = first ? delta->extents[first - 1].end : 0;
        size_t length = (size_t)(extent.end - position);
        if (length > size - done) length = size - done;
        int english = (extent.source & VOICE_DELTA_ENGLISH) != 0;
        uint64_t offset = (extent.source & ~VOICE_DELTA_ENGLISH) + position - start;
        if (!voice_read_exact(english ? delta->base_fd : patch_fd, (uint8_t *)out + done, length,
                              (english ? delta->base_bias : delta->patch_bias) + offset)) return -1;
        done += length;
        position += length;
    }
    return (ssize_t)size;
}

static int zip_copy_to_sidecar(const char *windows_path, char *host)
{
    const zm_entry *e = NULL;
    int kind = zip_lookup(windows_path, &e, 0);
    if (kind == 2) { mkdirs(host); return 0; }
    if (kind != 1) return -1;
    struct stat existing;
    if (!stat(host, &existing)) return 0;
    char parent[2048], temporary[2048];
    snprintf(parent, sizeof parent, "%s", host);
    char *slash = strrchr(parent, '/');
    if (slash) { *slash = 0; mkdirs(parent); }
    snprintf(temporary, sizeof temporary, "%s.zmi-part", host);
    int fd = open(temporary, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0664);
    if (fd < 0) return -1;
    uint8_t buffer[65536];
    uint64_t done = 0;
    int result = 0;
    while (done < e->size) {
        size_t chunk = (size_t)(e->size - done < sizeof buffer ? e->size - done : sizeof buffer);
        long long got = zm_pread(game_zip, e, buffer, chunk, done);
        if (got <= 0) { result = -1; break; }
        long long offset = 0;
        while (offset < got) {
            ssize_t written = write(fd, buffer + offset, (size_t)(got - offset));
            if (written < 0 && errno == EINTR) continue;
            if (written <= 0) { result = -1; break; }
            offset += written;
        }
        if (result) break;
        done += (uint64_t)got;
    }
    if (close(fd)) result = -1;
    if (result || rename(temporary, host)) { unlink(temporary); return -1; }
    return 0;
}

uint8_t *void_files_read_game(const char *relative, size_t *size)
{
    char windows[1024];
    int n = snprintf(windows, sizeof windows, "C:\\Games\\BioShock\\%s", relative);
    if (n < 0 || n >= (int)sizeof windows) { errno = ENAMETOOLONG; return NULL; }
    for (char *p = windows; *p; ++p) if (*p == '/') *p = '\\';
    int leaf = 0, parent = 0, error = ENOENT;
    char *host = to_host_ex(windows, 0, &parent, &leaf, NULL);
    uint8_t *data = NULL;
    if (host && leaf) {
        FILE *f = fopen(host, "rb");
        long length = -1;
        if (f && !fseek(f, 0, SEEK_END)) length = ftell(f);
        if (f && length >= 0 && !fseek(f, 0, SEEK_SET) && (data = (uint8_t *)malloc(length ? (size_t)length : 1)) &&
            fread(data, 1, (size_t)length, f) == (size_t)length) *size = (size_t)length;
        else { error = f ? EIO : errno; free(data); data = NULL; }
        if (f) fclose(f);
    }
    if (!data) {
        const zm_entry *e = NULL;
        if (zip_lookup(windows, &e, 0) == 1) {
            error = EIO;
            data = (uint8_t *)malloc(e->size ? (size_t)e->size : 1);
            for (uint64_t done = 0; data && done < e->size;) {
                long long got = zm_pread(game_zip, e, data + done, (size_t)(e->size - done), done);
                if (got > 0) done += (uint64_t)got;
                else { free(data); data = NULL; }
            }
            if (data) *size = (size_t)e->size;
        }
    }
    if (!data) {
        void *packed = NULL;
        if (!void_obb_read(windows, &packed, size) && packed) data = (uint8_t *)packed;
    }
    free(host);
    if (!data) errno = error;
    return data;
}

char *void_path_to_host(const char *windows_path) { return to_host(windows_path, NULL, NULL); }

/* The overlay's twin of an installation directory, for merged listings. */
static char *overlay_dir(const char *windows_dir)
{
    char norm[1024];
    if (!overlay_root[0] || !normalize(windows_dir, norm, sizeof norm)) return NULL;
    static const char game_mount[] = "C:\\Games\\BioShock";
    size_t l = sizeof game_mount - 1;
    if (strncasecmp(norm, game_mount, l) || (norm[l] && norm[l] != '\\')) return NULL;
    char *host = (char *)malloc(2048);
    int parent_ok = 0;
    unsigned char kind = DT_UNKNOWN;
    if (!resolve_below(host, 2048, overlay_root, norm + l, &parent_ok, &kind, 0) || kind == DT_REG) {
        free(host);
        return NULL;
    }
    return host;
}

uint32_t void_cwd_get(char *out, uint32_t cap)
{
    pthread_mutex_lock(&cwd_lock);
    uint32_t n = (uint32_t)snprintf(out, cap, "%s", cwd);
    pthread_mutex_unlock(&cwd_lock);
    return n;
}

int void_cwd_set(const char *windows_path)
{
    char norm[1024];
    if (!normalize(windows_path, norm, sizeof norm)) return 0;
    char *host = to_host(norm, NULL, NULL);
    struct stat st;
    int ok = host && (!stat(host, &st) || !void_obb_stat(norm, &st)) && S_ISDIR(st.st_mode);
    free(host);
    if (!ok) return 0;
    pthread_mutex_lock(&cwd_lock);
    snprintf(cwd, sizeof cwd, "%s", norm);
    pthread_mutex_unlock(&cwd_lock);
    return 1;
}

uint32_t void_errno_to_win32(int e)
{
    switch (e) {
    case ENOENT: return ERROR_FILE_NOT_FOUND_;
    case ENOTDIR: return ERROR_PATH_NOT_FOUND_;
    case EACCES: case EPERM: case EROFS: return ERROR_ACCESS_DENIED_;
    case EEXIST: return ERROR_FILE_EXISTS_;
    case ENOMEM: return ERROR_NOT_ENOUGH_MEMORY_;
    case EBADF: return ERROR_INVALID_HANDLE_;
    case EINVAL: return ERROR_INVALID_PARAMETER_;
    case ENOSPC: return 112u;          /* ERROR_DISK_FULL */
    case ENOTEMPTY: return 145u;       /* ERROR_DIR_NOT_EMPTY */
    default: return ERROR_ACCESS_DENIED_;
    }
}

/* ----------------------------------------------------------- file objects */
typedef struct {
    int fd;
    uint32_t granted_access;
    int std_stream;       /* 0 none, 1 stdin, 2 stdout, 3 stderr */
    int is_dir;
    char *host_path;
    int delete_on_close;
    char line[512];       /* buffered guest console output */
    size_t line_len;
    unsigned keep_times;
    struct timespec kept_times[2]; /* SetFileTime(-1) suppresses this handle's updates */
    int packed, bulk;
    uint64_t bias, packed_size, position;
    voice_delta *delta;
    pthread_mutex_t lock;
} file_t;

static int audio_file_trace(file_t *f)
{
    return f && f->host_path && strcasestr(f->host_path, "_audio");
}

static void audio_io_log(file_t *f, const char *operation, uint64_t offset, uint32_t requested, int64_t result)
{
    static unsigned records;
    if (audio_file_trace(f) && records++ < 512)
        void_log("[audio-io] handle=%08X %s offset=%llu request=%u result=%lld error=%u",
            f->fd, operation, (unsigned long long)offset, requested, (long long)result, (unsigned)void_get_last_error());
}

static int file_restore_kept_times(file_t *f)
{
    if (!f->keep_times) return 0;
    struct timespec times[2] = { {0, UTIME_OMIT}, {0, UTIME_OMIT} };
    for (unsigned i = 0; i < 2; ++i) if (f->keep_times & (1u << i)) times[i] = f->kept_times[i];
    int result;
    do { result = futimens(f->fd, times); } while (result && errno == EINTR);
    return result;
}

static void listing_forget_parent(const char *path, int subtree);

static int file_stat(const file_t *f, struct stat *st)
{
    if (f->packed) {
        memset(st, 0, sizeof *st);
        st->st_mode = S_IFREG | 0444;
        st->st_size = (off_t)f->packed_size;
        return 0;
    }
    if (f->is_dir && f->fd < 0) {
        memset(st, 0, sizeof *st);
        st->st_mode = S_IFDIR | 0755;
        return 0;
    }
    return fstat(f->fd, st);
}

static void file_destroy(void_object *o)
{
    file_t *f = (file_t *)o->data;
    pthread_mutex_lock(&f->lock);
    if (f->fd >= 0 && !f->std_stream) { file_restore_kept_times(f); close(f->fd); }
    if (f->delta) { close(f->delta->base_fd); free(f->delta); }
    if (f->delete_on_close && f->host_path) unlink(f->host_path);
    /* A written file's size and times changed under its cached listing. */
    if (f->host_path && (f->delete_on_close || (f->granted_access & 0x50000006u)))
        listing_forget_parent(f->host_path, 0);
    free(f->host_path);
    pthread_mutex_unlock(&f->lock);
    pthread_mutex_destroy(&f->lock);
}

static uint32_t file_handle(int fd, const char *host_path, int std_stream)
{
    void_object *o = void_object_new(VOID_OBJ_FILE, sizeof(file_t));
    file_t *f = (file_t *)o->data;
    f->fd = fd;
    f->std_stream = std_stream;
    f->host_path = host_path ? strdup(host_path) : NULL;
    pthread_mutex_init(&f->lock, NULL);
    o->destroy = file_destroy;
    o->signaled = 1;
    return void_handle_new(o);
}

static file_t *file_get(uint32_t h)
{
    void_object *o = void_handle_get(h, VOID_OBJ_FILE);
    return o ? (file_t *)o->data : NULL;
}

int void_file_host_fd(uint32_t h)
{
    file_t *f = file_get(h);
    return f && !f->delta ? f->fd : -1;
}

static uint32_t std_handles[3];

void void_files_std_init(void)
{
    std_handles[0] = file_handle(0, NULL, 1);
    std_handles[1] = file_handle(1, NULL, 2);
    std_handles[2] = file_handle(2, NULL, 3);
}

#define GENERIC_READ_ 0x80000000u
#define GENERIC_WRITE_ 0x40000000u
#define FILE_WRITE_DATA_ 0x0002u
#define FILE_APPEND_DATA_ 0x0004u
#define CREATE_NEW_ 1u
#define CREATE_ALWAYS_ 2u
#define OPEN_EXISTING_ 3u
#define OPEN_ALWAYS_ 4u
#define TRUNCATE_EXISTING_ 5u
#define FILE_FLAG_DELETE_ON_CLOSE_ 0x04000000u
#define FILE_FLAG_BACKUP_SEMANTICS_ 0x02000000u

/* Only this guest writer's CreateFile is redirected. Other guest threads still
 * read the committed PCS while the original serializer yields for file I/O. */
static _Thread_local char mobile_cache_pending[2048];
static int mobile_cache_flush_busy;

extern int void_machine_is_owned(void);

static int mobile_cache_string(uint32_t string, uint32_t maximum)
{
    if (!void_vm_readable(string, 12)) return 0;
    uint32_t data = MEM32(string), count = MEM32(string + 4), capacity = MEM32(string + 8);
    if (!count || count > maximum || count > capacity || !void_vm_readable(data, count * 2)) return 0;
    for (uint32_t i = 0; i < count; ++i)
        if ((MEM16(data + i * 2) == 0) != (i == count - 1)) return 0;
    return 1;
}

int void_bioshock_mobile_cache(uint32_t cache)
{
    static const char name[] = "ShaderCache.pcs";
    if (!void_machine_is_owned() || !mobile_shader_cache("\\ShaderCache.pcs") ||
        !void_vm_readable(cache, 40) || MEM32(cache + 32) ||
        !mobile_cache_string(cache + 20, sizeof name) || MEM32(cache + 24) != sizeof name) return 0;
    uint32_t text = MEM32(cache + 20);
    for (unsigned i = 0; i < sizeof name; ++i) {
        unsigned a = MEM16(text + i * 2), b = (unsigned char)name[i];
        if (a >= 'A' && a <= 'Z') a += 'a' - 'A';
        if (b >= 'A' && b <= 'Z') b += 'a' - 'A';
        if (a != b) return 0;
    }
    return 1;
}

static unsigned mobile_cache_compact(uint32_t count, unsigned char out[5])
{
    unsigned used = 1;
    out[0] = (unsigned char)(count & 63); count >>= 6;
    if (count) out[0] |= 64;
    while (count) {
        out[used++] = (unsigned char)((count & 127) | (count > 127 ? 128 : 0)); count >>= 7;
    }
    return used;
}

static uint32_t mobile_cache_snapshot(uint32_t cache, uint32_t *serialized)
{
    uint32_t records = MEM32(cache), count = MEM32(cache + 4);
    if (!count || count > 100000 || count > MEM32(cache + 8) || !void_vm_readable(records, count * 36)) return 0;
    uint64_t bytes = 40 + (uint64_t)count * 36 + ((MEM32(cache + 24) * 2 + 3) & ~3u), stream = 4;
    for (uint32_t i = 0; i < count; ++i) {
        uint32_t record = records + i * 36, value = MEM32(record + 32);
        if (!mobile_cache_string(record + 4, 2048) || !void_vm_readable(value, 12)) return 0;
        uint32_t size = MEM32(value), code = MEM32(value + 8), chars = MEM32(record + 8);
        if (!size || (size & 3) || size > 1024 * 1024 || !void_vm_readable(code, size)) return 0;
        unsigned char compact[5];
        bytes += ((chars * 2 + 3) & ~3u) + 12 + size;
        stream += mobile_cache_compact(chars, compact) + chars * 2 + 24 + size;
        if (bytes > 128u * 1024 * 1024 || stream > 128u * 1024 * 1024) return 0;
    }
    uint32_t snapshot = void_galloc((uint32_t)bytes);
    if (!snapshot) return 0;
    memcpy(GPTR(snapshot), GPTR(cache), 40);
    uint32_t rows = snapshot + 40, cursor = rows + count * 36;
    MEM32(snapshot) = rows; MEM32(snapshot + 8) = count;
    MEM32(snapshot + 12) = 0; MEM32(snapshot + 16) = 0; /* Save0 iterates the array, not hash buckets. */
    uint32_t name_bytes = MEM32(cache + 24) * 2;
    memcpy(GPTR(cursor), GPTR(MEM32(cache + 20)), name_bytes);
    MEM32(snapshot + 20) = cursor; MEM32(snapshot + 28) = MEM32(cache + 24);
    cursor += (name_bytes + 3) & ~3u;
    for (uint32_t i = 0; i < count; ++i) {
        uint32_t from = records + i * 36, to = rows + i * 36, value = MEM32(from + 32);
        uint32_t key_bytes = MEM32(from + 8) * 2, size = MEM32(value);
        memcpy(GPTR(to), GPTR(from), 36);
        memcpy(GPTR(cursor), GPTR(MEM32(from + 4)), key_bytes);
        MEM32(to + 4) = cursor; MEM32(to + 12) = MEM32(from + 8);
        cursor += (key_bytes + 3) & ~3u;
        memcpy(GPTR(cursor), GPTR(value), 12); MEM32(to + 32) = cursor;
        memcpy(GPTR(cursor + 12), GPTR(MEM32(value + 8)), size);
        MEM32(cursor + 8) = cursor + 12; cursor += 12 + size;
    }
    *serialized = (uint32_t)stream;
    return snapshot;
}

static int mobile_cache_equal(int fd, off_t *offset, const void *expected, size_t size)
{
    unsigned char buffer[4096];
    while (size) {
        size_t part = size < sizeof buffer ? size : sizeof buffer;
        ssize_t got = pread(fd, buffer, part, *offset);
        if (got < 0 && errno == EINTR) continue;
        if (got != (ssize_t)part || memcmp(buffer, expected, part)) return 0;
        *offset += (off_t)part; expected = (const unsigned char *)expected + part; size -= part;
    }
    return 1;
}

static int mobile_cache_verify(int fd, uint32_t snapshot, uint32_t expected_bytes)
{
    struct stat st;
    if (fstat(fd, &st) || st.st_size != expected_bytes) return 0;
    uint32_t version = 13, count = MEM32(snapshot + 4), records = MEM32(snapshot);
    off_t offset = 0;
    if (!mobile_cache_equal(fd, &offset, &version, 4)) return 0;
    for (uint32_t i = 0; i < count; ++i) {
        uint32_t row = records + i * 36, chars = MEM32(row + 8), value = MEM32(row + 32);
        unsigned char compact[5];
        unsigned length = mobile_cache_compact(chars, compact);
        if (!mobile_cache_equal(fd, &offset, compact, length) ||
            !mobile_cache_equal(fd, &offset, GPTR(MEM32(row + 4)), chars * 2) ||
            !mobile_cache_equal(fd, &offset, GPTR(row + 16), 16) ||
            !mobile_cache_equal(fd, &offset, GPTR(value), 8) ||
            !mobile_cache_equal(fd, &offset, GPTR(MEM32(value + 8)), MEM32(value))) return 0;
    }
    return offset == st.st_size;
}

void void_bioshock_mobile_cache_flush(uint32_t cache)
{
    int saved_errno = errno;
    if (!void_bioshock_mobile_cache(cache) || !MEM32(cache + 36) || !MEM32(cache + 4)) return;
    static unsigned skipped;
    if (__atomic_exchange_n(&mobile_cache_flush_busy, 1, __ATOMIC_ACQUIRE)) {
        if (__atomic_fetch_add(&skipped, 1, __ATOMIC_RELAXED) < 4) void_log("[mobile-cache] flush busy; original clear continues");
        errno = saved_errno; return;
    }
    uint32_t last_error = void_get_last_error(), bytes = 0, snapshot = mobile_cache_snapshot(cache, &bytes);
    int parent = 0, fd = -1, pending_created = 0, committed = 0;
    char *target = snapshot ? to_host_ex("ShaderCache.pcs", 0, &parent, NULL, NULL) : NULL;
    if (target && parent && strlen(target) + sizeof ".pending-XXXXXX" <= sizeof mobile_cache_pending) {
        snprintf(mobile_cache_pending, sizeof mobile_cache_pending, "%s.pending-XXXXXX", target);
        fd = mkstemp(mobile_cache_pending);
        if (fd >= 0) {
            pending_created = 1;
            void_regs saved;
            void_regs_save(&saved);
#ifdef RECOMP_X87_WINDOW
            double window[RECOMP_X87_SLOTS]; int64_t integers[RECOMP_X87_SLOTS]; int sp = g_fp_sp;
            memcpy(window, g_st_win, sizeof window); memcpy(integers, g_st_i64_win, sizeof integers);
#endif
            uint32_t compression = 0;
            (void)void_call_guest_this(0x10AF7060u, snapshot, 1, &compression);
#ifdef RECOMP_X87_WINDOW
            g_fp_sp = sp;
            memcpy(g_st_win, window, sizeof window); memcpy(g_st_i64_win, integers, sizeof integers);
#endif
            void_regs_load(&saved);
            /* EAX is not success: the original writer also returns 1 on open failure. */
            if (mobile_cache_verify(fd, snapshot, bytes) && !fsync(fd)) {
                int close_error = close(fd); fd = -1;
                if (!close_error && !rename(mobile_cache_pending, target)) committed = 1;
            }
        }
    }
    if (fd >= 0) close(fd);
    if (!committed && pending_created) unlink(mobile_cache_pending);
    mobile_cache_pending[0] = 0;
    if (!committed && __atomic_fetch_add(&skipped, 1, __ATOMIC_RELAXED) < 4)
        void_log("[mobile-cache] flush skipped; committed PCS preserved");
    free(target);
    if (snapshot) void_gfree(snapshot); /* Interior pointers belong to this one blob; never Clear(snapshot). */
    void_set_last_error(last_error); errno = saved_errno;
    __atomic_store_n(&mobile_cache_flush_busy, 0, __ATOMIC_RELEASE);
}

static void mobile_cache_redirect_write(char **host, const char *normalized, int write)
{
    if (!*host || !write || !mobile_cache_pending[0]) return;
    const char *name = strrchr(normalized, '\\');
    if (mobile_shader_cache(normalized) &&
        name && !strcasecmp(name + 1, "ShaderCache.pcs")) {
        free(*host); *host = strdup(mobile_cache_pending);
    }
}

/* Host side of CreateFile: runs with the guest machine released, so it only
 * touches host state; *error is the thread's next last-error value. */
static uint32_t create_file(const char *path, uint32_t access, uint32_t disposition, uint32_t flags, uint32_t *error)
{
    *error = 0;
    if (!path || !*path) { *error = ERROR_PATH_NOT_FOUND_; return WIN_INVALID_HANDLE; }
    if (!strncasecmp(path, "\\\\.\\", 4) || !strcasecmp(path, "CONOUT$") || !strcasecmp(path, "CONIN$")) {
        void_log("[void] CreateFile(%s): device paths are not supported", path);
        *error = ERROR_FILE_NOT_FOUND_;
        return WIN_INVALID_HANDLE;
    }
    int write = (access & (GENERIC_WRITE_ | FILE_WRITE_DATA_ | FILE_APPEND_DATA_ | 0x10000000u)) != 0;
    int read = (access & (GENERIC_READ_ | 0x0001u | 0x10000000u)) != 0 || !write;
    int oflags = O_CLOEXEC | (write ? (read ? O_RDWR : O_WRONLY) : O_RDONLY);
    switch (disposition) {
    case CREATE_NEW_: oflags |= O_CREAT | O_EXCL; break;
    case CREATE_ALWAYS_: oflags |= O_CREAT | O_TRUNC; break;
    case OPEN_EXISTING_: break;
    case OPEN_ALWAYS_: oflags |= O_CREAT; break;
    case TRUNCATE_EXISTING_: oflags |= O_TRUNC; break;
    default:
        *error = ERROR_INVALID_PARAMETER_;
        return WIN_INVALID_HANDLE;
    }
    int parent_ok = 0, leaf = -1;
    int plain_read = !write && disposition == OPEN_EXISTING_;
    char normalized[1024] = "";
    char *host = to_host_ex(path, plain_read ? RESOLVE_OVERLAY : 0, &parent_ok, &leaf, normalized);
    int bulk = plain_read && bulk_file(normalized);
    mobile_cache_redirect_write(&host, normalized, write);
    const char *language = voice_language();
    char suffix[9] = "";
    if (strcmp(language, "int")) snprintf(suffix,sizeof suffix,"_%s.bik",language);
    int replacement = host && (mobile_shader_cache(normalized) || (overlay_root[0] && under(host, overlay_root)) ||
        (plain_read && leaf && suffix[0] && strcasestr(host,suffix) &&
         (strcasestr(host, "/Content/BinkMovies/") || (under(host, data_root) && strstr(host, "/bink-voice/")))));
    const zm_entry *archived = NULL;
    int archived_kind = replacement ? 0 : zip_lookup(path, &archived, plain_read);
    if (archived_kind == 1 && disposition == CREATE_NEW_) {
        free(host);
        *error = ERROR_FILE_EXISTS_;
        return WIN_INVALID_HANDLE;
    }
    if (archived_kind && (write || (oflags & O_TRUNC))) {
        if (zip_copy_to_sidecar(path, host)) {
            *error = void_errno_to_win32(errno);
            free(host);
            return WIN_INVALID_HANDLE;
        }
        archived_kind = 0;
    }
    if (archived_kind == 1 && !write && !(oflags & O_TRUNC)) {
        VLOG("[void] CreateFile(%s) -> archive entry, %llu bytes", path, (unsigned long long)archived->size);
        *error = disposition == OPEN_ALWAYS_ ? ERROR_ALREADY_EXISTS_ : 0;
        uint32_t h = file_handle(dup(zm_fd(game_zip)), host, 0);
        file_t *f = file_get(h);
        f->granted_access = access;
        f->packed = 1;
        f->bulk = bulk;
        f->bias = archived->offset;
        f->packed_size = archived->size;
        return h;
    }
    if (plain_read && archived_kind == 2 && (flags & FILE_FLAG_BACKUP_SEMANTICS_)) {
        uint32_t h = file_handle(-1, host, 0);
        file_t *f = file_get(h);
        f->granted_access = access;
        f->is_dir = 1;
        free(host);
        return h;
    }
    if (plain_read && !replacement) {
        const char *packed_path = host && under(host, game_root) ? host : path;
        obb_file packed;
        if (!void_obb_lookup(packed_path, &packed)) {
            uint32_t h = file_handle(packed.fd, host, 0);
            file_t *f = file_get(h);
            f->granted_access = access;
            f->packed = 1;
            f->bulk = bulk;
            f->bias = packed.bias;
            f->packed_size = packed.size;
            free(host);
            return h;
        }
        if (host && (flags & FILE_FLAG_BACKUP_SEMANTICS_)) {
            struct stat st;
            if (!void_obb_stat(packed_path, &st) && S_ISDIR(st.st_mode)) {
                uint32_t h = file_handle(-1, host, 0);
                file_t *f = file_get(h);
                f->granted_access = access;
                f->is_dir = 1;
                free(host);
                return h;
            }
        }
    }
    char delta_path[1024];
    obb_file patch;
    if (plain_read && leaf <= 0 && voice_delta_path(path, delta_path) && game_asset_source(delta_path, &patch)) {
        voice_delta *delta = voice_delta_open(delta_path, &patch);
        if (!delta) {
            close(patch.fd);
            free(host);
            *error = 13u; /* ERROR_INVALID_DATA */
            void_log("[voice-delta] invalid delta or English base for %s", path);
            return WIN_INVALID_HANDLE;
        }
        uint32_t h = file_handle(patch.fd, path, 0);
        file_t *f = file_get(h);
        f->granted_access = access;
        f->packed = 1;
        f->packed_size = delta->size;
        f->delta = delta;
        free(host);
        void_log("[voice-delta] %s (%u bytes from %llu-byte delta)", path, delta->size,
                 (unsigned long long)patch.size);
        return h;
    }
    if (!host) { *error = ERROR_PATH_NOT_FOUND_; return WIN_INVALID_HANDLE; }
    if (write || (oflags & (O_CREAT | O_TRUNC))) archive_changed(host);
    /* A listed directory answers the loose-file probes without a lookup. */
    char parent[2048];
    snprintf(parent, sizeof parent, "%s", host);
    char *slash = strrchr(parent, '/');
    if (slash) *slash = 0;
    int listed = slash && cacheable(parent);
    if (listed && (!parent_ok || (!leaf && !(oflags & O_CREAT)))) {
        *error = parent_ok ? ERROR_FILE_NOT_FOUND_ : ERROR_PATH_NOT_FOUND_;
        VLOG("[void] CreateFile(%s -> %s) failed: not listed", path, host);
        free(host);
        return WIN_INVALID_HANDLE;
    }
    if ((oflags & O_CREAT) && !write) oflags = (oflags & ~O_ACCMODE) | O_RDWR;
    /* Open first: OPEN_ALWAYS/CREATE_ALWAYS still report an existing file. */
    int existed = 0, created = 0, fd = -1;
    if (oflags & O_CREAT) {
        if (!(oflags & O_EXCL)) {
            fd = open(host, oflags & ~(O_CREAT | O_TRUNC), 0664);
            if (fd >= 0) {
                existed = 1;
                if ((oflags & O_TRUNC) && ftruncate(fd, 0)) { int e = errno; close(fd); fd = -1; errno = e; }
            } else if (errno == ENOENT) {
                fd = open(host, oflags | O_EXCL, 0664);
                created = fd >= 0;
            }
        } else {
            fd = open(host, oflags, 0664);
            created = fd >= 0;
        }
    } else {
        fd = open(host, oflags, 0664);
    }
    if (fd < 0 && (errno == EISDIR || errno == EACCES) && (flags & FILE_FLAG_BACKUP_SEMANTICS_))
        fd = open(host, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd < 0) {
        int e = errno;
        uint32_t code = void_errno_to_win32(e);
        if (e == ENOENT && !parent_ok) code = ERROR_PATH_NOT_FOUND_;
        if (e == EISDIR) code = ERROR_ACCESS_DENIED_;
        VLOG("[void] CreateFile(%s -> %s) failed: %s", path, host, strerror(e));
        free(host);
        *error = code;
        return WIN_INVALID_HANDLE;
    }
    struct stat st;
    int is_dir = !fstat(fd, &st) && S_ISDIR(st.st_mode);
    if (is_dir && !(flags & FILE_FLAG_BACKUP_SEMANTICS_)) {
        close(fd);
        free(host);
        *error = ERROR_ACCESS_DENIED_;
        return WIN_INVALID_HANDLE;
    }
    if (write || created) listing_forget(parent);
    uint32_t h = file_handle(fd, host, 0);
    file_t *f = file_get(h);
    f->granted_access = access;
    f->is_dir = is_dir;
    f->bulk = bulk;
    if (flags & FILE_FLAG_DELETE_ON_CLOSE_) f->delete_on_close = 1;
    if (replacement && plain_read) {
        static unsigned shader_reads;
        if (__atomic_fetch_add(&shader_reads, 1, __ATOMIC_RELAXED) < 8)
            void_log("[port-overlay] read %s -> %s", normalized, host);
    }
    VLOG("[void] CreateFile(%s) -> %s handle %08X", path, host, h);
    free(host);
    *error = (existed && (disposition == CREATE_ALWAYS_ || disposition == OPEN_ALWAYS_)) ? ERROR_ALREADY_EXISTS_ : 0;
    return h;
}

#define K32 "kernel32.dll"

static uint32_t create_file_guest(char *path)
{
    uint32_t access = ARG(1), disposition = ARG(4), flags = ARG(5), error = 0;
    uint64_t start = void_perf_now_ns();
    char mapped[1024];
    int plain_read = disposition == OPEN_EXISTING_ &&
        !(access & (GENERIC_WRITE_ | FILE_WRITE_DATA_ | FILE_APPEND_DATA_ | 0x10000000u));
    const char *read_path = plain_read ? voice_bank_path(path, mapped) : path;
    /* Cached loose-file misses do no I/O; do not hand the whole guest to another thread for each probe. */
    if (path && *path && strncasecmp(path, "\\\\.\\", 4) &&
        strcasecmp(path, "CONOUT$") && strcasecmp(path, "CONIN$") &&
        plain_read) {
        int parent_ok = 0, leaf = 1;
        char *known = to_host_ex(read_path, RESOLVE_OVERLAY | RESOLVE_CACHED_ONLY, &parent_ok, &leaf, NULL);
        int missing = known && !leaf;
        free(known);
        /* A stored ZIP played in place: its entries are not in the sidecar's listings. Once the sidecar has a
         * folder of the path (Content holds the unpacked .ini files and the converted texture pack), the listing
         * alone called every map and package below it missing (Core.Errors.FileNotFound for Entry.bsm). */
        char delta_path[1024];
        if (missing && (zip_lookup(read_path, NULL, 1) || voice_delta_path(read_path, delta_path))) missing = 0;
        if (missing) {
            free(path);
            void_set_last_error(parent_ok ? ERROR_FILE_NOT_FOUND_ : ERROR_PATH_NOT_FOUND_);
            void_perf_add(VOID_PERF_FILE_OPEN, void_perf_now_ns() - start, 0);
            return WIN_INVALID_HANDLE;
        }
    }
    void_blocking_begin();
    char movie[1024];
    int aliased = plain_read && (bink_voice_alias(read_path, movie) || flash_pc_movie(read_path, movie));
    uint32_t h = create_file(aliased ? movie : read_path, access, disposition, flags, &error);
    if (aliased && h == WIN_INVALID_HANDLE) h = create_file(read_path, access, disposition, flags, &error);
    const char *ext = strrchr(read_path, '.');
    if (ext && !strcasecmp(ext, ".bik"))
        void_log("[bink] file %s%s%s -> handle %08X error %u", read_path, aliased ? " via " : "",
            aliased ? movie : "", h, error);
    uint64_t elapsed = void_perf_now_ns() - start;
    void_blocking_end();
    void_perf_add(VOID_PERF_FILE_OPEN, elapsed, 0);
    free(path);
    void_set_last_error(error);
    return h;
}

WINAPI_EXPORT(K32, CreateFileA, 7) { return create_file_guest(void_astr_utf8(ARG(0))); }
WINAPI_EXPORT(K32, CreateFileW, 7) { return create_file_guest(void_wstr_utf8(ARG(0))); }

static void console_write(file_t *f, const uint8_t *data, uint32_t n)
{
    for (uint32_t i = 0; i < n; ++i) {
        char c = (char)data[i];
        if (c == '\n' || f->line_len == sizeof f->line - 1) {
            f->line[f->line_len] = 0;
            void_log("[guest-%s] %s", f->std_stream == 3 ? "stderr" : "stdout", f->line);
            f->line_len = 0;
            if (c == '\n') continue;
        }
        if (c != '\r') f->line[f->line_len++] = c;
    }
}

WINAPI_EXPORT(K32, ReadFile, 5)
{
    uint32_t h = ARG(0), buffer = ARG(1), size = ARG(2), read_out = ARG(3), overlapped = ARG(4);
    file_t *f = file_get(h);
    if (read_out) MEM32(read_out) = 0;
    if (!f || f->is_dir) { void_set_last_error(ERROR_INVALID_HANDLE_); return 0; }
    if (f->std_stream) return 1;
    if (size && !void_vm_writable(buffer, size)) { void_set_last_error(998u); return 0; }  /* ERROR_NOACCESS */
    uint64_t explicit_position = overlapped ? (uint64_t)MEM32(overlapped + 8) | ((uint64_t)MEM32(overlapped + 12) << 32) : 0;
    /* A cold sub-page header can block on FUSE just like a full streaming read. */
    int blocking = size && (f->bulk || size >= 16384);
    /* Keep this file alive if another guest closes its handle while we read. */
    uint32_t retained = blocking ? void_handle_dup(h) : 0;
    if (f->packed) {
        /* A guest may be holding the machine while waiting for f->lock in seek. */
        uint64_t start = void_perf_now_ns();
        if (blocking) void_blocking_begin();
        pthread_mutex_lock(&f->lock);
        uint64_t position = overlapped ? explicit_position : f->position;
        if (position > f->packed_size) size = 0;
        else if (size > f->packed_size - position) size = (uint32_t)(f->packed_size - position);
        ssize_t got = 0;
        size_t total = 0;
        while (total < size) {
            uint8_t *target = (uint8_t *)GPTR(buffer) + total;
            ssize_t part = f->delta ? voice_delta_pread(f->delta, f->fd, target, size - total, position + total) :
                pread(f->fd, target, size - total, (off_t)(f->bias + position + total));
            if (part < 0) { if (errno == EINTR) continue; got = -1; break; }
            if (!part) break;
            total += (size_t)part;
        }
        if (got >= 0) got = (ssize_t)total;
        int e = errno;
        if (got > 0 && !overlapped) f->position += (uint64_t)got;
        pthread_mutex_unlock(&f->lock);
        uint64_t elapsed = void_perf_now_ns() - start;
        if (blocking) void_blocking_end();
        void_perf_add(VOID_PERF_FILE_READ, elapsed, got > 0 ? (uint64_t)got : 0);
        audio_io_log(f, "read", position, size, got);
        if (retained) void_handle_close(retained);
        if (got < 0) { void_set_last_error(void_errno_to_win32(e)); return 0; }
        if (read_out) MEM32(read_out) = (uint32_t)got;
        if (overlapped) { MEM32(overlapped) = 0; MEM32(overlapped + 4) = (uint32_t)got; }
        return 1;
    }
    ssize_t got;
    int fd = f->fd;
    uint64_t start = void_perf_now_ns();
    if (blocking) void_blocking_begin();
    if (overlapped) {
        do { got = pread(fd, GPTR(buffer), size, (off_t)explicit_position); } while (got < 0 && errno == EINTR);
    } else {
        size_t total = 0;
        got = 0;
        while (total < size) {
            ssize_t r = read(fd, (uint8_t *)GPTR(buffer) + total, size - total);
            if (r < 0) { if (errno == EINTR) continue; got = -1; break; }
            if (r == 0) break;
            total += (size_t)r;
        }
        if (got >= 0) got = (ssize_t)total;
    }
    int e = errno;
    uint64_t elapsed = void_perf_now_ns() - start;
    if (blocking) void_blocking_end();
    if (file_restore_kept_times(f) && got >= 0) { got = -1; e = errno; }
    if (retained) void_handle_close(retained);
    void_perf_add(VOID_PERF_FILE_READ, elapsed, got > 0 ? (uint64_t)got : 0);
    if (got < 0) { void_set_last_error(void_errno_to_win32(e)); return 0; }
    if (read_out) MEM32(read_out) = (uint32_t)got;
    if (overlapped) { MEM32(overlapped) = 0; MEM32(overlapped + 4) = (uint32_t)got; }
    return 1;
}

WINAPI_EXPORT(K32, WriteFile, 5)
{
    uint32_t h = ARG(0), buffer = ARG(1), size = ARG(2), written_out = ARG(3), overlapped = ARG(4);
    file_t *f = file_get(h);
    if (written_out) MEM32(written_out) = 0;
    if (!f || f->is_dir) { void_set_last_error(ERROR_INVALID_HANDLE_); return 0; }
    if (f->std_stream) {
        console_write(f, (const uint8_t *)GPTR(buffer), size);
        if (written_out) MEM32(written_out) = size;
        return 1;
    }
    if (f->packed) { void_set_last_error(ERROR_ACCESS_DENIED_); return 0; }
    /* A write to the shared storage (FUSE) can stall for 100+ ms: the game's own log writer held the machine that
     * long in WriteFile (hold backtraces, POCO, 2026-11-25). The write runs with the machine released, as reads do;
     * the kernel orders writes and the file position of one descriptor, and the handle is kept alive meanwhile. */
    int blocking = size != 0;
    uint32_t retained = blocking ? void_handle_dup(h) : 0;
    int fd = f->fd;
    off_t offset = overlapped ? (off_t)MEM32(overlapped + 8) | ((off_t)MEM32(overlapped + 12) << 32) : 0;
    if (blocking) void_blocking_begin();
    ssize_t put;
    if (overlapped) put = pwrite(fd, GPTR(buffer), size, offset);
    else put = write(fd, GPTR(buffer), size);
    int e = errno;
    if (blocking) void_blocking_end();
    if (file_restore_kept_times(f) && put >= 0) { put = -1; e = errno; }
    if (retained) void_handle_close(retained);
    if (put < 0) { void_set_last_error(void_errno_to_win32(e)); return 0; }
    if (written_out) MEM32(written_out) = (uint32_t)put;
    return 1;
}

static off_t file_seek(file_t *f, int64_t distance, int whence)
{
    if (!f->packed) return lseek(f->fd, (off_t)distance, whence);
    pthread_mutex_lock(&f->lock);
    int64_t base = whence == SEEK_SET ? 0 : whence == SEEK_CUR ? (int64_t)f->position : (int64_t)f->packed_size;
    int64_t next;
    if (__builtin_add_overflow(base, distance, &next) || next < 0) {
        pthread_mutex_unlock(&f->lock);
        return -1;
    }
    f->position = (uint64_t)next;
    audio_io_log(f, "seek", (uint64_t)base, (uint32_t)distance, next);
    pthread_mutex_unlock(&f->lock);
    return (off_t)next;
}

WINAPI_EXPORT(K32, SetFilePointer, 4)
{
    file_t *f = file_get(ARG(0));
    if (!f) { void_set_last_error(ERROR_INVALID_HANDLE_); return 0xFFFFFFFFu; }
    int64_t distance = ARG(2) ? (int64_t)(((uint64_t)MEM32(ARG(2)) << 32) | ARG(1)) : (int64_t)(int32_t)ARG(1);
    int whence = ARG(3) == 0 ? SEEK_SET : ARG(3) == 1 ? SEEK_CUR : SEEK_END;
    if (f->std_stream) return 0;
    off_t pos = file_seek(f, distance, whence);
    if (pos < 0) { void_set_last_error(131u); return 0xFFFFFFFFu; }   /* ERROR_NEGATIVE_SEEK */
    if (ARG(2)) MEM32(ARG(2)) = (uint32_t)((uint64_t)pos >> 32);
    void_set_last_error(0);
    return (uint32_t)pos;
}

WINAPI_EXPORT(K32, SetFilePointerEx, 5)
{
    file_t *f = file_get(ARG(0));
    if (!f) { void_set_last_error(ERROR_INVALID_HANDLE_); return 0; }
    int64_t distance = (int64_t)(((uint64_t)ARG(2) << 32) | ARG(1));
    int whence = ARG(4) == 0 ? SEEK_SET : ARG(4) == 1 ? SEEK_CUR : SEEK_END;
    off_t pos = file_seek(f, distance, whence);
    if (pos < 0) { void_set_last_error(131u); return 0; }
    if (ARG(3)) { MEM32(ARG(3)) = (uint32_t)pos; MEM32(ARG(3) + 4) = (uint32_t)((uint64_t)pos >> 32); }
    return 1;
}

WINAPI_EXPORT(K32, GetFileSize, 2)
{
    file_t *f = file_get(ARG(0));
    struct stat st;
    if (!f || file_stat(f, &st)) { void_set_last_error(ERROR_INVALID_HANDLE_); return 0xFFFFFFFFu; }
    if (ARG(1)) MEM32(ARG(1)) = (uint32_t)((uint64_t)st.st_size >> 32);
    void_set_last_error(0);
    return (uint32_t)st.st_size;
}

WINAPI_EXPORT(K32, GetFileSizeEx, 2)
{
    file_t *f = file_get(ARG(0));
    struct stat st;
    if (!f || file_stat(f, &st)) { void_set_last_error(ERROR_INVALID_HANDLE_); return 0; }
    MEM32(ARG(1)) = (uint32_t)st.st_size;
    MEM32(ARG(1) + 4) = (uint32_t)((uint64_t)st.st_size >> 32);
    return 1;
}

WINAPI_EXPORT(K32, GetFileType, 1)
{
    file_t *f = file_get(ARG(0));
    if (!f) { void_set_last_error(ERROR_INVALID_HANDLE_); return 0; }
    return f->std_stream ? 2u : 1u;    /* FILE_TYPE_CHAR / FILE_TYPE_DISK */
}

WINAPI_EXPORT(K32, FlushFileBuffers, 1) { return file_get(ARG(0)) ? 1u : 0u; }

WINAPI_EXPORT(K32, SetEndOfFile, 1)
{
    file_t *f = file_get(ARG(0));
    if (!f) { void_set_last_error(ERROR_INVALID_HANDLE_); return 0; }
    if (f->packed) { void_set_last_error(ERROR_ACCESS_DENIED_); return 0; }
    off_t pos = lseek(f->fd, 0, SEEK_CUR);
    if (pos < 0 || ftruncate(f->fd, pos)) { void_set_last_error(void_errno_to_win32(errno)); return 0; }
    if (file_restore_kept_times(f)) { void_set_last_error(void_errno_to_win32(errno)); return 0; }
    return 1;
}

WINAPI_EXPORT(K32, LockFile, 5) { return 1; }
WINAPI_EXPORT(K32, UnlockFile, 5) { return 1; }
WINAPI_EXPORT(K32, LockFileEx, 6) { return 1; }

WINAPI_EXPORT(K32, GetStdHandle, 1)
{
    switch (ARG(0)) {
    case 0xFFFFFFF6u: return std_handles[0];
    case 0xFFFFFFF5u: return std_handles[1];
    case 0xFFFFFFF4u: return std_handles[2];
    default: void_set_last_error(ERROR_INVALID_HANDLE_); return WIN_INVALID_HANDLE;
    }
}

WINAPI_EXPORT(K32, SetStdHandle, 2)
{
    switch (ARG(0)) {
    case 0xFFFFFFF6u: std_handles[0] = ARG(1); return 1;
    case 0xFFFFFFF5u: std_handles[1] = ARG(1); return 1;
    case 0xFFFFFFF4u: std_handles[2] = ARG(1); return 1;
    default: return 0;
    }
}

static uint64_t unix_to_filetime(time_t t, long ns)
{
    return (uint64_t)t * 10000000ull + (uint64_t)ns / 100u + 116444736000000000ull;
}

/* The first 36 bytes are shared by GET_FILEEX_INFO_STANDARD and find data. */
static void fill_file_metadata(const struct stat *st, uint32_t out)
{
    /* As GetFileAttributes reports them: ARCHIVE, READONLY without write bits. */
    MEM32(out) = S_ISDIR(st->st_mode) ? 0x10u : (st->st_mode & 0222) ? 0x20u : 0x21u;
    MEM64(out + 4) = unix_to_filetime(st->st_ctim.tv_sec, st->st_ctim.tv_nsec);
    MEM64(out + 12) = unix_to_filetime(st->st_atim.tv_sec, st->st_atim.tv_nsec);
    MEM64(out + 20) = unix_to_filetime(st->st_mtim.tv_sec, st->st_mtim.tv_nsec);
    MEM32(out + 28) = S_ISDIR(st->st_mode) ? 0 : (uint32_t)((uint64_t)st->st_size >> 32);
    MEM32(out + 32) = S_ISDIR(st->st_mode) ? 0 : (uint32_t)st->st_size;
}

/* The host stat of a guest path, with the machine released; 0 on success,
 * else the Win32 error. */
static uint32_t stat_guest_path_lookup(const char *path, struct stat *st)
{
    if (!path) return ERROR_PATH_NOT_FOUND_;
    char mapped[1024];
    path = voice_bank_path(path, mapped);
    int cached_parent = 0;
    char cached_norm[1024];
    char *cached = to_host_ex(path,RESOLVE_OVERLAY|RESOLVE_CACHED_ONLY,&cached_parent,NULL,cached_norm);
    if (cached) {
        int result = cached_parent ? cached_stat_mode(cached,st,0) : -1;
        int error = cached_parent ? errno : ENOENT;
        free(cached);
        if (!result) return 0;
        if (error == ENOENT) {
            const zm_entry *entry = NULL;
            int kind = mobile_shader_cache(cached_norm) ? 0 : zip_lookup(path, &entry, 1);
            if (kind) { zip_stat(kind == 1 ? entry : NULL,st); return 0; }
            char delta_path[1024];
            if (!voice_delta_path(path, delta_path)) return cached_parent ? ERROR_FILE_NOT_FOUND_ : ERROR_PATH_NOT_FOUND_;
        }
    }
    void_blocking_begin();
    int parent_ok = 0, leaf = -1;
    char normalized[1024];
    char *host = to_host_ex(path, RESOLVE_OVERLAY, &parent_ok, &leaf, normalized);
    uint32_t error = 0;
    if (!host) error = ERROR_PATH_NOT_FOUND_;
    else if (!parent_ok) error = ERROR_PATH_NOT_FOUND_;
    else if (cached_stat(host, st)) error = errno == ENOENT ? ERROR_FILE_NOT_FOUND_ : void_errno_to_win32(errno);
    if ((error == ERROR_PATH_NOT_FOUND_ || error == ERROR_FILE_NOT_FOUND_) && !(host && mobile_shader_cache(normalized))) {
        const zm_entry *archived = NULL;
        int kind = zip_lookup(path, &archived, 1);
        if (kind) { zip_stat(kind == 1 ? archived : NULL, st); error = 0; }
        else if (voice_delta_stat(path, st)) error = 0;
    }
    free(host);
    void_blocking_end();
    return error;
}

/* The game's configuration probes each .ini/.int name in five folders before every read (UE2's GConfig):
 * 223,000 failing GetFileAttributes per session on the POCO, ~80,000 in each level load, the same few dozen
 * paths again and again. A miss is remembered by its exact guest path until any listing changes (this host's
 * writes, renames and deletions, or inotify on the profile folders), so a repeat answers without the path
 * conversion, the listing lookup and the archive and voice-delta checks. Only "not found" answers are kept. */
#define STAT_MISS_SLOTS 1024u
#define STAT_MISS_PATH 200u
static struct stat_miss { uint32_t generation, error; char path[STAT_MISS_PATH]; } stat_misses[STAT_MISS_SLOTS];
static pthread_mutex_t stat_miss_lock = PTHREAD_MUTEX_INITIALIZER;

static uint32_t stat_miss_slot(const char *path, size_t length)
{
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < length; ++i) h = (h ^ (uint8_t)path[i]) * 16777619u;
    return h & (STAT_MISS_SLOTS - 1);
}

static uint32_t stat_guest_path(const char *path, struct stat *st)
{
    size_t length = path ? strlen(path) : 0;
    if (!length || length >= STAT_MISS_PATH) return stat_guest_path_lookup(path, st);
    struct stat_miss *slot = &stat_misses[stat_miss_slot(path, length)];
    /* External edits of the watched profile folders reach the generation only through their inotify queue, which
     * the lookup drains; a hit must drain it too or a file the launcher creates would stay "missing" (Grok's
     * review, 2026-11-25). The drain is one non-blocking read when nothing changed. */
    pthread_mutex_lock(&listing_lock);
    listing_watch_drain();
    pthread_mutex_unlock(&listing_lock);
    uint32_t generation = __atomic_load_n(&listing_generation, __ATOMIC_ACQUIRE);
    pthread_mutex_lock(&stat_miss_lock);
    uint32_t cached = slot->error && slot->generation == generation + 1u && !strcmp(slot->path, path) ? slot->error : 0;
    pthread_mutex_unlock(&stat_miss_lock);
    if (cached) return cached;
    uint32_t error = stat_guest_path_lookup(path, st);
    if ((error == ERROR_FILE_NOT_FOUND_ || error == ERROR_PATH_NOT_FOUND_) &&
        __atomic_load_n(&listing_generation, __ATOMIC_ACQUIRE) == generation) {
        pthread_mutex_lock(&stat_miss_lock);
        memcpy(slot->path, path, length + 1);
        slot->error = error;
        slot->generation = generation + 1u;   /* 0 stays "empty" */
        pthread_mutex_unlock(&stat_miss_lock);
    }
    return error;
}

static uint32_t attributes_of(const char *path)
{
    struct stat st;
    uint32_t error = stat_guest_path(path, &st);
    if (error) {
        void_set_last_error(error);
        VLOG("[void] GetFileAttributes(%s): missing", path ? path : "(null)");
        return 0xFFFFFFFFu;
    }
    if (S_ISDIR(st.st_mode)) return 0x10u;
    return (st.st_mode & 0222) ? 0x20u : 0x21u;   /* FILE_ATTRIBUTE_ARCHIVE (| READONLY) */
}

WINAPI_EXPORT(K32, GetFileAttributesA, 1)
{
    char *path = void_astr_utf8(ARG(0));
    uint32_t r = attributes_of(path);
    free(path);
    return r;
}

WINAPI_EXPORT(K32, GetFileAttributesW, 1)
{
    char *path = void_wstr_utf8(ARG(0));
    uint32_t r = attributes_of(path);
    free(path);
    return r;
}

static uint32_t attributes_ex(int wide)
{
    if (ARG(1) != 0 || !void_vm_writable(ARG(2), 36)) { void_set_last_error(ERROR_INVALID_PARAMETER_); return 0; }
    char *path = wide ? void_wstr_utf8(ARG(0)) : void_astr_utf8(ARG(0));
    struct stat st;
    uint32_t error = stat_guest_path(path, &st);
    if (!error) fill_file_metadata(&st, ARG(2));
    else {
        void_set_last_error(error);
        VLOG("[void] GetFileAttributesEx(%s): error %u", path ? path : "(null)", error);
    }
    free(path);
    return !error;
}
WINAPI_EXPORT(K32, GetFileAttributesExA, 3) { return attributes_ex(0); }
WINAPI_EXPORT(K32, GetFileAttributesExW, 3) { return attributes_ex(1); }

WINAPI_EXPORT(K32, GetFileTime, 4)
{
    file_t *file = file_get(ARG(0));
    struct stat st;
    if (!file || file_stat(file, &st)) { void_set_last_error(ERROR_INVALID_HANDLE_); return 0; }
    for (uint32_t i = 1; i < 4; ++i) {
        if (ARG(i) && !void_vm_writable(ARG(i), 8)) { void_set_last_error(ERROR_INVALID_PARAMETER_); return 0; }
    }
    if (ARG(1)) MEM64(ARG(1)) = unix_to_filetime(st.st_ctim.tv_sec, st.st_ctim.tv_nsec);
    if (ARG(2)) MEM64(ARG(2)) = unix_to_filetime(st.st_atim.tv_sec, st.st_atim.tv_nsec);
    if (ARG(3)) MEM64(ARG(3)) = unix_to_filetime(st.st_mtim.tv_sec, st.st_mtim.tv_nsec);
    return 1;
}

WINAPI_EXPORT(K32, GetFileInformationByHandle, 2)
{
    file_t *file = file_get(ARG(0));
    uint32_t out = ARG(1);
    struct stat st;
    if (!file || file_stat(file, &st)) { void_set_last_error(ERROR_INVALID_HANDLE_); return 0; }
    if (!void_vm_writable(out, 52)) { void_set_last_error(ERROR_INVALID_PARAMETER_); return 0; }
    MEM32(out) = S_ISDIR(st.st_mode) ? 0x10u : 0x20u;
    MEM64(out + 4) = unix_to_filetime(st.st_ctim.tv_sec, st.st_ctim.tv_nsec);
    MEM64(out + 12) = unix_to_filetime(st.st_atim.tv_sec, st.st_atim.tv_nsec);
    MEM64(out + 20) = unix_to_filetime(st.st_mtim.tv_sec, st.st_mtim.tv_nsec);
    MEM32(out + 28) = (uint32_t)st.st_dev;
    MEM32(out + 32) = S_ISDIR(st.st_mode) ? 0 : (uint32_t)((uint64_t)st.st_size >> 32);
    MEM32(out + 36) = S_ISDIR(st.st_mode) ? 0 : (uint32_t)st.st_size;
    MEM32(out + 40) = (uint32_t)st.st_nlink;
    MEM32(out + 44) = (uint32_t)((uint64_t)st.st_ino >> 32);
    MEM32(out + 48) = (uint32_t)st.st_ino;
    return 1;
}

static uint32_t delete_file(const char *path)
{
    char *host = to_host(path, NULL, NULL);
    int ok = host && !unlink(host);
    if (!ok) void_set_last_error(host ? void_errno_to_win32(errno) : ERROR_PATH_NOT_FOUND_);
    if (host) listing_forget_parent(host, 0);
    free(host);
    return ok;
}

WINAPI_EXPORT(K32, DeleteFileA, 1) { char *p = void_astr_utf8(ARG(0)); uint32_t r = delete_file(p); free(p); return r; }
WINAPI_EXPORT(K32, DeleteFileW, 1) { char *p = void_wstr_utf8(ARG(0)); uint32_t r = delete_file(p); free(p); return r; }

/* The random map generator removes its working folder after saving a map. */
static uint32_t remove_directory(const char *path)
{
    char *host = path ? to_host(path, NULL, NULL) : NULL;
    int ok = host && !rmdir(host);
    if (!ok) void_set_last_error(host ? void_errno_to_win32(errno) : ERROR_PATH_NOT_FOUND_);
    if (host) listing_forget_parent(host, 1);
    VLOG("[void] RemoveDirectory(%s) = %d", path ? path : "(null)", ok);
    free(host);
    return ok;
}

WINAPI_EXPORT(K32, RemoveDirectoryA, 1) { char *p = void_astr_utf8(ARG(0)); uint32_t r = remove_directory(p); free(p); return r; }
WINAPI_EXPORT(K32, RemoveDirectoryW, 1) { char *p = void_wstr_utf8(ARG(0)); uint32_t r = remove_directory(p); free(p); return r; }

/* Only READONLY has a representation here (the owner write bit); other
 * attributes are accepted for an existing file, as on FAT-like volumes. */
static uint32_t set_file_attributes(const char *path, uint32_t attributes)
{
    char *host = path ? to_host(path, NULL, NULL) : NULL;
    struct stat st;
    if (!host || stat(host, &st)) {
        void_set_last_error(host ? void_errno_to_win32(errno) : ERROR_PATH_NOT_FOUND_);
        free(host);
        return 0;
    }
    if (!S_ISDIR(st.st_mode)) {
        mode_t mode = (attributes & 1u) ? (st.st_mode & ~(mode_t)0222) : (st.st_mode | 0200);
        /* Shared storage may refuse chmod; the attribute is advisory there. */
        if ((mode & 07777) != (st.st_mode & 07777) && chmod(host, mode & 07777))
            VLOG("[void] SetFileAttributes(%s): chmod: %s", path, strerror(errno));
    }
    listing_forget_parent(host, 0);
    free(host);
    return 1;
}

WINAPI_EXPORT(K32, SetFileAttributesA, 2) { char *p = void_astr_utf8(ARG(0)); uint32_t r = set_file_attributes(p, ARG(1)); free(p); return r; }
WINAPI_EXPORT(K32, SetFileAttributesW, 2) { char *p = void_wstr_utf8(ARG(0)); uint32_t r = set_file_attributes(p, ARG(1)); free(p); return r; }

/* CopyFile uses the same virtual drive/mount resolution as CreateFile. Never
 * pass a guest path directly to POSIX, even when it resembles a host path. */
static uint32_t copy_file(const char *from, const char *to, int fail_if_exists)
{
    int source_parent = 0, dest_parent = 0, in = -1, out = -1;
    uint32_t error = 0;
    char *a = to_host(from, &source_parent, NULL);
    char *b = to_host(to, &dest_parent, NULL);
    struct stat source, dest;
    if (!a || !b) { error = ERROR_PATH_NOT_FOUND_; goto done; }
    do { in = open(a, O_RDONLY | O_NONBLOCK | O_CLOEXEC); } while (in < 0 && errno == EINTR);
    if (in < 0) {
        error = errno == ENOENT && !source_parent ? ERROR_PATH_NOT_FOUND_ : void_errno_to_win32(errno);
        goto done;
    }
    if (fstat(in, &source)) { error = void_errno_to_win32(errno); goto done; }
    if (!S_ISREG(source.st_mode)) { error = ERROR_ACCESS_DENIED_; goto done; }
    /* O_EXCL makes fail-if-exists atomic. Without it, defer truncation until
     * identity and read-only checks have accepted the opened destination. */
    do { out = open(b, O_WRONLY | O_CREAT | O_CLOEXEC | (fail_if_exists ? O_EXCL : 0), 0664); }
    while (out < 0 && errno == EINTR);
    if (out < 0) {
        error = errno == ENOENT && !dest_parent ? ERROR_PATH_NOT_FOUND_ : void_errno_to_win32(errno);
        goto done;
    }
    if (fstat(out, &dest)) { error = void_errno_to_win32(errno); goto done; }
    if (!S_ISREG(dest.st_mode) || !(dest.st_mode & 0222)) { error = ERROR_ACCESS_DENIED_; goto done; }
    if (source.st_dev == dest.st_dev && source.st_ino == dest.st_ino) {
        error = 32u;                   /* ERROR_SHARING_VIOLATION (also hard links) */
        goto done;
    }
    int result;
    do { result = ftruncate(out, 0); } while (result < 0 && errno == EINTR);
    if (result) { error = void_errno_to_win32(errno); goto done; }
    uint8_t buffer[16384];
    for (;;) {
        ssize_t got;
        do { got = read(in, buffer, sizeof buffer); } while (got < 0 && errno == EINTR);
        if (got < 0) { error = void_errno_to_win32(errno); goto done; }
        if (!got) break;
        ssize_t offset = 0;
        while (offset < got) {
            ssize_t put = write(out, buffer + offset, (size_t)(got - offset));
            if (put < 0 && errno == EINTR) continue;
            if (put <= 0) { error = put ? void_errno_to_win32(errno) : 29u; goto done; } /* ERROR_WRITE_FAULT */
            offset += put;
        }
    }
    /* POSIX permissions represent the supported read-only attribute. Preserve
     * timestamps too; Windows security descriptors/alternate streams have no
     * representation in this host's file model. */
    if ((source.st_mode & 0777) != (dest.st_mode & 0777) && fchmod(out, source.st_mode & 0777)) {
        error = void_errno_to_win32(errno); goto done;
    }
    struct timespec times[2] = { source.st_atim, source.st_mtim };
    if (futimens(out, times)) { error = void_errno_to_win32(errno); goto done; }
done:
    /* Linux/Android close releases the descriptor even on EINTR: do not retry. */
    if (out >= 0 && close(out) && !error) error = void_errno_to_win32(errno);
    if (in >= 0) close(in);
    if (b) listing_forget_parent(b, 0);
    VLOG("[void] CopyFile(%s -> %s, fail_if_exists=%d): %s (%u)",
         from ? from : "(null)", to ? to : "(null)", fail_if_exists, error ? "failed" : "copied", error);
    free(a); free(b);
    if (error) void_set_last_error(error);
    return !error;
}

/* Validate before the shared converters read guest memory. This host supports
 * MAX_PATH paths; longer names fail rather than truncating into another file. */
static int copy_path_valid(uint32_t address, int wide)
{
    if (!address) { void_set_last_error(ERROR_PATH_NOT_FOUND_); return 0; }
    uint32_t unit = wide ? 2u : 1u;
    for (uint32_t i = 0; i < WIN_MAX_PATH; ++i) {
        uint64_t at = (uint64_t)address + i * unit;
        if (at + unit > UINT64_C(0x100000000) || !void_vm_readable((uint32_t)at, unit)) {
            void_set_last_error(998u); return 0; /* ERROR_NOACCESS */
        }
        if (!(wide ? MEM16((uint32_t)at) : MEM8((uint32_t)at))) {
            if (!i) { void_set_last_error(ERROR_PATH_NOT_FOUND_); return 0; }
            return 1;
        }
    }
    void_set_last_error(206u); return 0; /* ERROR_FILENAME_EXCED_RANGE */
}

static uint32_t copy_file_guest(int wide)
{
    if (!copy_path_valid(ARG(0), wide) || !copy_path_valid(ARG(1), wide)) return 0;
    char *a = wide ? void_wstr_utf8(ARG(0)) : void_astr_utf8(ARG(0));
    char *b = wide ? void_wstr_utf8(ARG(1)) : void_astr_utf8(ARG(1));
    uint32_t result = 0;
    if (!a || !b) void_set_last_error(ERROR_NOT_ENOUGH_MEMORY_);
    else result = copy_file(a, b, ARG(2) != 0);
    free(a); free(b);
    return result;
}

WINAPI_EXPORT(K32, CopyFileA, 3) { return copy_file_guest(0); }
WINAPI_EXPORT(K32, CopyFileW, 3) { return copy_file_guest(1); }

static uint32_t move_file(const char *from, const char *to)
{
    char *a = to_host(from, NULL, NULL), *b = to_host(to, NULL, NULL);
    struct stat st;
    int ok = 0;
    if (a && b) {
        if (!stat(b, &st)) void_set_last_error(ERROR_ALREADY_EXISTS_);
        else if (rename(a, b)) void_set_last_error(void_errno_to_win32(errno));
        else ok = 1;
        listing_forget_parent(a, 1);
        listing_forget_parent(b, 1);
    }
    free(a); free(b);
    return ok;
}

WINAPI_EXPORT(K32, MoveFileA, 2)
{
    char *a = void_astr_utf8(ARG(0)), *b = void_astr_utf8(ARG(1));
    uint32_t r = move_file(a, b);
    free(a); free(b);
    return r;
}

WINAPI_EXPORT(K32, MoveFileW, 2)
{
    char *a = void_wstr_utf8(ARG(0)), *b = void_wstr_utf8(ARG(1));
    uint32_t r = move_file(a, b);
    free(a); free(b);
    return r;
}

static uint32_t create_directory(const char *path)
{
    char *host = path ? to_host(path, NULL, NULL) : NULL;
    uint32_t ok = 0;
    if (host) {
        if (!mkdir(host, 0775)) ok = 1;
        else void_set_last_error(errno == EEXIST ? ERROR_ALREADY_EXISTS_ : void_errno_to_win32(errno));
        listing_forget_parent(host, 0);
    } else {
        void_set_last_error(ERROR_PATH_NOT_FOUND_);
    }
    if (path && (strcasestr(path, "MEMORY") || strcasestr(path, "SaveSlot")))
        void_log("[save-directory] path=%s host=%s result=%u", path, host ? host : "(unresolved)", ok);
    VLOG("[void] CreateDirectory(%s) = %u", path ? path : "(null)", ok);
    free(host);
    return ok;
}

WINAPI_EXPORT(K32, CreateDirectoryW, 2) { char *p = void_wstr_utf8(ARG(0)); uint32_t r = create_directory(p); free(p); return r; }
WINAPI_EXPORT(K32, CreateDirectoryA, 2) { char *p = void_astr_utf8(ARG(0)); uint32_t r = create_directory(p); free(p); return r; }

/* GetFullPathName: the normalized absolute Windows path. */
static uint32_t full_path(const char *in, uint32_t buffer, uint32_t capacity, uint32_t file_part, int wide)
{
    char norm[1024];
    if (!in || !normalize(in, norm, sizeof norm)) { void_set_last_error(ERROR_INVALID_PARAMETER_); return 0; }
    /* Keep a trailing separator the caller supplied. */
    size_t in_len = strlen(in);
    if (in_len && (in[in_len - 1] == '\\' || in[in_len - 1] == '/') && strlen(norm) > 3) strcat(norm, "\\");
    uint32_t written = wide ? void_utf8_to_gw(norm, buffer, capacity) : void_utf8_to_ga(norm, buffer, capacity);
    uint32_t units = 0;
    if (wide) { if (written + 1 > capacity) return written; units = written; }
    else { if (written + 1 > capacity) return written; units = written; }
    if (file_part) {
        uint32_t last = 0;
        for (uint32_t i = 0; i < units; ++i) {
            uint32_t c = wide ? MEM16(buffer + 2 * i) : MEM8(buffer + i);
            if (c == '\\') last = i + 1;
        }
        MEM32(file_part) = last < units ? buffer + (wide ? 2 * last : last) : 0;
    }
    return units;
}

WINAPI_EXPORT(K32, GetFullPathNameA, 4)
{
    char *p = void_astr_utf8(ARG(0));
    uint32_t r = full_path(p, ARG(2), ARG(1), ARG(3), 0);
    free(p);
    return r;
}

WINAPI_EXPORT(K32, GetFullPathNameW, 4)
{
    char *p = void_wstr_utf8(ARG(0));
    uint32_t r = full_path(p, ARG(2), ARG(1), ARG(3), 1);
    free(p);
    return r;
}

static const char temp_dir[] = "C:\\Users\\Player\\AppData\\Local\\Temp\\";

WINAPI_EXPORT(K32, GetTempPathA, 2)
{
    uint32_t n = void_utf8_to_ga(temp_dir, ARG(1), ARG(0));
    return n;
}

WINAPI_EXPORT(K32, GetTempPathW, 2)
{
    uint32_t n = void_utf8_to_gw(temp_dir, ARG(1), ARG(0));
    return n;
}

static uint32_t temp_file_name(const char *dir, const char *prefix, uint32_t unique, char *out, size_t cap)
{
    static uint32_t counter;
    uint32_t number = unique ? unique : (uint32_t)(void_tick_ms() + ++counter) & 0xFFFFu;
    for (int attempt = 0; attempt < 65536; ++attempt) {
        if (!number) number = 1;
        snprintf(out, cap, "%s%s%.3s%X.tmp", dir, (dir[0] && dir[strlen(dir) - 1] == '\\') ? "" : "\\", prefix ? prefix : "", number);
        if (unique) return number;
        char *host = to_host(out, NULL, NULL);
        int fd = host ? open(host, O_CREAT | O_EXCL | O_WRONLY | O_CLOEXEC, 0664) : -1;
        if (fd >= 0) listing_forget_parent(host, 0);
        free(host);
        if (fd >= 0) { close(fd); return number; }
        number = (number + 1) & 0xFFFFu;
    }
    return 0;
}

WINAPI_EXPORT(K32, GetTempFileNameA, 4)
{
    char *dir = void_astr_utf8(ARG(0)), *prefix = void_astr_utf8(ARG(1));
    char name[1024];
    uint32_t r = temp_file_name(dir ? dir : "", prefix, ARG(2), name, sizeof name);
    if (r) void_utf8_to_ga(name, ARG(3), WIN_MAX_PATH);
    free(dir); free(prefix);
    return r;
}

WINAPI_EXPORT(K32, GetTempFileNameW, 4)
{
    char *dir = void_wstr_utf8(ARG(0)), *prefix = void_wstr_utf8(ARG(1));
    char name[1024];
    uint32_t r = temp_file_name(dir ? dir : "", prefix, ARG(2), name, sizeof name);
    if (r) void_utf8_to_gw(name, ARG(3), WIN_MAX_PATH);
    free(dir); free(prefix);
    return r;
}

/* ----------------------------------------------------------- drive space */
static char *volume_path(const char *path)
{
    if (!path) path = "C:\\";
    if (!*path || (path[1] == ':' && (path[0] | 0x20) != 'c')) {
        void_set_last_error(ERROR_PATH_NOT_FOUND_); return NULL;
    }
    if (path[0] == '\\' && path[1] == '\\') { void_set_last_error(53u); return NULL; } /* ERROR_BAD_NETPATH */
    char *host = to_host(path, NULL, NULL);
    struct stat st;
    if (!host || stat(host, &st)) {
        void_set_last_error(host ? void_errno_to_win32(errno) : ERROR_PATH_NOT_FOUND_);
        free(host); return NULL;
    }
    if (!S_ISDIR(st.st_mode)) { void_set_last_error(267u); free(host); return NULL; } /* ERROR_DIRECTORY */
    return host;
}

static uint32_t drive_type(int wide)
{
    char *path = ARG(0) ? (wide ? void_wstr_utf8(ARG(0)) : void_astr_utf8(ARG(0))) : NULL;
    size_t length = path ? strlen(path) : 0;
    if (path && length > 2 && path[length - 1] != '\\' && path[length - 1] != '/') { free(path); return 1; }
    char *host = volume_path(path);
    uint32_t result = host ? 3 : 1;     /* DRIVE_FIXED / DRIVE_NO_ROOT_DIR */
    free(host); free(path);
    return result;
}
WINAPI_EXPORT(K32, GetDriveTypeA, 1) { return drive_type(0); }
WINAPI_EXPORT(K32, GetDriveTypeW, 1) { return drive_type(1); }
WINAPI_EXPORT(K32, GetLogicalDrives, 0) { return 1u << 2; }

static uint32_t logical_drive_strings(int wide)
{
    if (ARG(0) < 5) return 5;
    if (!void_vm_writable(ARG(1), wide ? 10 : 5)) { void_set_last_error(ERROR_INVALID_PARAMETER_); return 0; }
    if (wide) { void_utf8_to_gw("C:\\", ARG(1), ARG(0)); MEM16(ARG(1) + 8) = 0; }
    else { void_utf8_to_ga("C:\\", ARG(1), ARG(0)); MEM8(ARG(1) + 4) = 0; }
    return 4;
}
WINAPI_EXPORT(K32, GetLogicalDriveStringsA, 2) { return logical_drive_strings(0); }
WINAPI_EXPORT(K32, GetLogicalDriveStringsW, 2) { return logical_drive_strings(1); }

static int volume_space(int wide, struct statvfs *space)
{
    char *path = ARG(0) ? (wide ? void_wstr_utf8(ARG(0)) : void_astr_utf8(ARG(0))) : NULL;
    char *host = volume_path(path);
    int ok = host && !statvfs(host, space);
    if (host && !ok) void_set_last_error(void_errno_to_win32(errno));
    free(host); free(path);
    return ok;
}

static uint32_t disk_free_space(int wide)
{
    for (uint32_t i = 1; i < 5; ++i) {
        if (!void_vm_writable(ARG(i), 4)) { void_set_last_error(ERROR_INVALID_PARAMETER_); return 0; }
    }
    struct statvfs space;
    if (!volume_space(wide, &space)) return 0;
    uint64_t unit = space.f_frsize ? space.f_frsize : space.f_bsize;
    uint64_t total = space.f_blocks, available = space.f_bavail;
    /* A larger virtual cluster keeps the legacy DWORD counts representable. */
    while ((total > UINT32_MAX || available > UINT32_MAX) && unit <= UINT32_MAX / 2u) {
        unit *= 2; total /= 2; available /= 2;
    }
    if (!unit || unit > UINT32_MAX || total > UINT32_MAX || available > UINT32_MAX) {
        void_set_last_error(534u); return 0;        /* ERROR_ARITHMETIC_OVERFLOW */
    }
    uint32_t sector = unit >= 512 && !(unit % 512) ? 512 : (uint32_t)unit;
    MEM32(ARG(1)) = (uint32_t)unit / sector;
    MEM32(ARG(2)) = sector;
    MEM32(ARG(3)) = (uint32_t)available;
    MEM32(ARG(4)) = (uint32_t)total;
    return 1;
}
WINAPI_EXPORT(K32, GetDiskFreeSpaceA, 5) { return disk_free_space(0); }
WINAPI_EXPORT(K32, GetDiskFreeSpaceW, 5) { return disk_free_space(1); }

static uint32_t disk_free_space_ex(int wide)
{
    for (uint32_t i = 1; i < 4; ++i) {
        if (ARG(i) && !void_vm_writable(ARG(i), 8)) { void_set_last_error(ERROR_INVALID_PARAMETER_); return 0; }
    }
    struct statvfs space;
    if (!volume_space(wide, &space)) return 0;
    uint64_t unit = space.f_frsize ? space.f_frsize : space.f_bsize;
    if (!unit || space.f_blocks > UINT64_MAX / unit || space.f_bavail > UINT64_MAX / unit || space.f_bfree > UINT64_MAX / unit) {
        void_set_last_error(534u); return 0;
    }
    if (ARG(1)) MEM64(ARG(1)) = (uint64_t)space.f_bavail * unit;
    if (ARG(2)) MEM64(ARG(2)) = (uint64_t)space.f_blocks * unit;
    if (ARG(3)) MEM64(ARG(3)) = (uint64_t)space.f_bfree * unit;
    return 1;
}
WINAPI_EXPORT(K32, GetDiskFreeSpaceExA, 4) { return disk_free_space_ex(0); }
WINAPI_EXPORT(K32, GetDiskFreeSpaceExW, 4) { return disk_free_space_ex(1); }

/* ------------------------------------------------------------ find files */
/* Matches with their metadata, gathered once (machine released) by FindFirst. */
typedef struct {
    char *name;
    struct stat st;
} found_t;

typedef struct {
    found_t *found;
    int count, next;
} find_t;

static void find_destroy(void_object *o)
{
    find_t *f = (find_t *)o->data;
    for (int i = 0; i < f->count; ++i) free(f->found[i].name);
    free(f->found);
}

static void fill_find_data(const found_t *found, uint32_t out, int wide)
{
    memset(GPTR(out), 0, wide ? 592 : 320);
    fill_file_metadata(&found->st, out);
    if (wide) void_utf8_to_gw(found->name, out + 44, 260);
    else void_utf8_to_ga(found->name, out + 44, 260);
}

static int find_matches(const char *pattern, const char *name)
{
    /* "*.*" matches every name, as on Windows; "x." matches extensionless names. */
    if (!strcmp(pattern, "*") || !strcmp(pattern, "*.*")) return 1;
    if (!fnmatch(pattern, name, FNM_CASEFOLD)) return 1;
    size_t n = strlen(pattern);
    return n && pattern[n - 1] == '.' && !strchr(name, '.');
}

/* Names of one host directory matching the pattern, skipping ones already
 * gathered (the overlay's come first). Below a drive root Windows lists "."
 * and ".." first, as readdir does for uncached directories. */
static void find_gather(find_t *f, const char *dir, const char *pattern, int dots)
{
    char **names = NULL;
    int count = 0;
    if (cacheable(dir)) {
        pthread_mutex_lock(&listing_lock);
        dir_listing *l = listing_get(dir, 1);
        if (l) {
            names = (char **)malloc(sizeof *names * (size_t)(l->count + 3));
            static const char *const dot_names[] = {".", ".."};
            for (int i = 0; i < 2 && dots; ++i)
                if (find_matches(pattern, dot_names[i])) names[count++] = strdup(dot_names[i]);
            for (int i = 0; i < l->count; ++i)
                if (find_matches(pattern, l->entries[i].name)) names[count++] = strdup(l->entries[i].name);
        }
        pthread_mutex_unlock(&listing_lock);
    } else {
        DIR *d = opendir(dir);
        if (d) {
            int capacity = 0;
            struct dirent *e;
            while ((e = readdir(d))) {
                if (!find_matches(pattern, e->d_name)) continue;
                if (!dots && (!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))) continue;
                if (count == capacity) {
                    capacity = capacity ? capacity * 2 : 32;
                    names = (char **)realloc(names, sizeof *names * (size_t)capacity);
                }
                names[count++] = strdup(e->d_name);
            }
            closedir(d);
        }
    }
    struct stat dot_st = {0};
    int dot_state = 0;                            /* the directory's own stat: 1 ok, -1 failed */
    for (int i = 0; i < count; ++i) {
        int duplicate = 0;
        for (int j = 0; j < f->count && !duplicate; ++j) duplicate = !strcasecmp(f->found[j].name, names[i]);
        char host[4096];
        snprintf(host, sizeof host, "%s/%s", dir, names[i]);
        struct stat st;
        /* "." and ".." are no listing entries: they report the directory itself. */
        if (!strcmp(names[i], ".") || !strcmp(names[i], "..")) {
            if (!duplicate && !dot_state) dot_state = cached_stat(dir, &dot_st) ? -1 : 1;
            if (duplicate || dot_state < 0) { free(names[i]); continue; }
            st = dot_st;
        } else if (duplicate || cached_stat(host, &st)) { free(names[i]); continue; }
        f->found = (found_t *)realloc(f->found, sizeof *f->found * (size_t)(f->count + 1));
        f->found[f->count].name = names[i];
        f->found[f->count++].st = st;
    }
    free(names);
}

typedef struct {
    find_t *found;
    const char *pattern;
    size_t prefix;
    int before;
    char directory[512];
} zip_children;

static int zip_child(const zm_entry *e, void *user)
{
    zip_children *c = (zip_children *)user;
    if ((e->flags & ZM_SIDECAR) || e->length <= c->prefix) return 0;
    const char *name = e->path + c->prefix;
    size_t n = e->length - c->prefix, k = 0;
    while (k < n && name[k] != '/' && name[k] != '\\') ++k;
    int directory = k < n;
    char child[512];
    if (!k || k >= sizeof child) return 0;
    memcpy(child, name, k);
    child[k] = 0;
    if (directory && !strcasecmp(child, c->directory)) return 0;
    if (directory) snprintf(c->directory, sizeof c->directory, "%s", child);
    if (!find_matches(c->pattern, child)) return 0;
    find_t *f = c->found;
    for (int j = 0; j < c->before; ++j) if (!strcasecmp(f->found[j].name, child)) return 0;
    f->found = (found_t *)realloc(f->found, sizeof *f->found * (size_t)(f->count + 1));
    f->found[f->count].name = strdup(child);
    zip_stat(directory ? NULL : e, &f->found[f->count++].st);
    return 0;
}

static void zip_gather(find_t *f, const char *norm_dir, const char *pattern, int dots)
{
    const char *folder = norm_dir + sizeof "C:\\Games\\BioShock" - 1;
    while (*folder == '\\') ++folder;
    zip_children c = { f, pattern, *folder ? strlen(folder) + 1 : 0, 0, "" };
    static const char *const dot_names[] = {".", ".."};
    for (int i = 0; i < 2 && dots; ++i) {
        int known = 0;
        for (int j = 0; j < f->count && !known; ++j) known = !strcmp(f->found[j].name, dot_names[i]);
        if (known || !find_matches(pattern, dot_names[i])) continue;
        f->found = (found_t *)realloc(f->found, sizeof *f->found * (size_t)(f->count + 1));
        f->found[f->count].name = strdup(dot_names[i]);
        zip_stat(NULL, &f->found[f->count++].st);
    }
    c.before = f->count;
    zm_list(game_zip, folder, zip_child, &c);
}

static void voice_delta_gather(find_t *f, const char *folder, const char *pattern)
{
    if (strcasecmp(folder, voice_sounds)) return;
    for (unsigned i = 0; i < sizeof voice_banks / sizeof *voice_banks; ++i) {
        char name[32], path[128];
        snprintf(name, sizeof name, "streams_%s_audio.rus_fsb", voice_banks[i]);
        if (!find_matches(pattern, name)) continue;
        int exists = 0;
        for (int j = 0; j < f->count && !exists; ++j) exists = !strcasecmp(name, f->found[j].name);
        if (exists) continue;
        snprintf(path, sizeof path, "%s\\%s", voice_sounds, name);
        struct stat st;
        if (!voice_delta_stat(path, &st)) continue;
        f->found = realloc(f->found, sizeof *f->found * (size_t)(f->count + 1));
        f->found[f->count++] = (found_t){.name = strdup(name), .st = st};
    }
}

static uint32_t find_first(int wide)
{
    char *pattern_path = wide ? void_wstr_utf8(ARG(0)) : void_astr_utf8(ARG(0));
    uint32_t out = ARG(1);
    if (!pattern_path) { void_set_last_error(ERROR_PATH_NOT_FOUND_); return WIN_INVALID_HANDLE; }
    if (!void_vm_writable(out, wide ? 592 : 320)) { free(pattern_path); void_set_last_error(ERROR_INVALID_PARAMETER_); return WIN_INVALID_HANDLE; }
    size_t input_length = strlen(pattern_path);
    if (!input_length || pattern_path[input_length - 1] == '\\' || pattern_path[input_length - 1] == '/') {
        free(pattern_path); void_set_last_error(ERROR_FILE_NOT_FOUND_); return WIN_INVALID_HANDLE;
    }
    char norm[1024];
    char mapped[1024];
    if (!normalize(voice_bank_path(pattern_path, mapped), norm, sizeof norm)) { free(pattern_path); void_set_last_error(ERROR_PATH_NOT_FOUND_); return WIN_INVALID_HANDLE; }
    char *slash = strrchr(norm, '\\');
    char pattern[512];
    snprintf(pattern, sizeof pattern, "%s", slash ? slash + 1 : norm);
    if (slash) *slash = 0;
    if (strlen(norm) == 2) strcat(norm, "\\");
    uint64_t start = void_perf_now_ns();
    void_blocking_begin();
    int parent_ok = 0;
    char *host_dir = to_host(norm, &parent_ok, NULL);
    char *extra_dir = overlay_dir(norm);
    struct stat dir_st;
    int dir_ok = host_dir && parent_ok && !cached_stat(host_dir, &dir_st) && S_ISDIR(dir_st.st_mode);
    find_t gathered = {0};
    int dots = strlen(norm) > 3;                  /* not "c:\\" */
    if (extra_dir) find_gather(&gathered, extra_dir, pattern, dots);
    if (dir_ok) find_gather(&gathered, host_dir, pattern, dots);
    int archived = zip_lookup(norm, NULL, 0) == 2;
    if (archived) zip_gather(&gathered, norm, pattern, dots);
    voice_delta_gather(&gathered, norm, pattern);
    dir_ok |= archived;
    free(host_dir); free(extra_dir);
    void_blocking_end();
    void_perf_add(VOID_PERF_FIND, void_perf_now_ns() - start, 0);
    if (!dir_ok && !gathered.count) {
        VLOG("[void] FindFirstFile(%s): no directory", pattern_path);
        free(pattern_path);
        void_set_last_error(ERROR_PATH_NOT_FOUND_);
        return WIN_INVALID_HANDLE;
    }
    VLOG("[void] FindFirstFile(%s): %d matches", pattern_path, gathered.count);
    free(pattern_path);
    if (!gathered.count) {
        void_set_last_error(ERROR_FILE_NOT_FOUND_);
        return WIN_INVALID_HANDLE;
    }
    void_object *o = void_object_new(VOID_OBJ_FIND, sizeof(find_t));
    find_t *f = (find_t *)o->data;
    o->destroy = find_destroy;
    *f = gathered;
    fill_find_data(&f->found[f->next++], out, wide);
    return void_handle_new(o);
}
WINAPI_EXPORT(K32, FindFirstFileA, 2) { return find_first(0); }
WINAPI_EXPORT(K32, FindFirstFileW, 2) { return find_first(1); }

static uint32_t find_next(int wide)
{
    void_object *o = void_handle_get(ARG(0), VOID_OBJ_FIND);
    if (!o) { void_set_last_error(ERROR_INVALID_HANDLE_); return 0; }
    if (!void_vm_writable(ARG(1), wide ? 592 : 320)) { void_set_last_error(ERROR_INVALID_PARAMETER_); return 0; }
    find_t *f = (find_t *)o->data;
    if (f->next < f->count) {
        fill_find_data(&f->found[f->next++], ARG(1), wide);
        return 1;
    }
    void_set_last_error(ERROR_NO_MORE_FILES_);
    return 0;
}
WINAPI_EXPORT(K32, FindNextFileA, 2) { return find_next(0); }
WINAPI_EXPORT(K32, FindNextFileW, 2) { return find_next(1); }

WINAPI_EXPORT(K32, FindClose, 1)
{
    if (!void_handle_get(ARG(0), VOID_OBJ_FIND)) { void_set_last_error(ERROR_INVALID_HANDLE_); return 0; }
    void_handle_close(ARG(0));
    return 1;
}

/* ------------------------------------------------------------- mappings */
typedef struct {
    int fd; uint64_t size; uint32_t protect;
    int archive, packed;
    uint64_t bias;
    uint64_t identity, cached_offset;
    uint32_t cached_size;
    uint8_t *cached;
} mapping_t;
static uint64_t mapping_identity;
#define ARCHIVE_READ_AHEAD 65536u

static void mapping_destroy(void_object *o)
{
    mapping_t *m = (mapping_t *)o->data;
    if (m->fd >= 0) close(m->fd);
    free(m->cached);
}

static int voice_delta_mapping(const file_t *file)
{
    const voice_delta *delta = file->delta;
    int fd = (int)syscall(__NR_memfd_create, "bioshock-voice", 1u /* MFD_CLOEXEC */);
    if (fd < 0) return -1;
    if (!ftruncate(fd, delta->size)) {
        void *bytes = mmap(NULL, delta->size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        if (bytes != MAP_FAILED) {
            ssize_t got = voice_delta_pread(delta, file->fd, bytes, delta->size, 0);
            int error = errno;
            munmap(bytes, delta->size);
            if (got == delta->size) return fd;
            errno = error;
        }
    }
    int error = errno;
    close(fd);
    errno = error;
    return -1;
}

WINAPI_EXPORT(K32, CreateFileMappingA, 6)
{
    uint32_t file = ARG(0), protect = ARG(2);
    uint64_t size = ((uint64_t)ARG(3) << 32) | ARG(4);
    int fd, archive = 0, packed = 0;
    uint64_t bias = 0;
    if (file == WIN_INVALID_HANDLE) {
        /* A mapping backed by the paging file: anonymous shared memory. The
         * temporary file on the profile's (FUSE) storage cost 20-30 ms of
         * create, resize and unlink with the guest machine held (FMOD makes
         * one per stream; the audio producer waited for it). That path stays
         * as the fallback, with the machine released for its I/O. */
        if (!size) { void_set_last_error(ERROR_INVALID_PARAMETER_); return 0; }
        fd = (int)syscall(__NR_memfd_create, "bioshock-mapping", 1u /* MFD_CLOEXEC */);
        if (fd >= 0 && ftruncate(fd, (off_t)size)) { close(fd); fd = -1; }
        if (fd < 0) {
            char path[1100];
            snprintf(path, sizeof path, "%s/Temp/.mapping-XXXXXX", data_root);
            int error = 0, stage = 0;
            void_blocking_begin();
            fd = mkstemp(path);
            if (fd < 0) { error = errno; stage = 1; }
            else if (ftruncate(fd, (off_t)size)) { error = errno; stage = 2; unlink(path); close(fd); fd = -1; }
            else if (unlink(path)) { error = errno; stage = 3; close(fd); fd = -1; }
            void_blocking_end();
            if (fd < 0) {
                void_log("[mapping] temporary %s failed path=%s error=%s",
                         stage == 1 ? "create" : stage == 2 ? "resize" : "unlink", path, strerror(error));
                void_set_last_error(stage == 1 ? ERROR_INVALID_PARAMETER_ : stage == 2 ? ERROR_NOT_ENOUGH_MEMORY_ :
                                    void_errno_to_win32(error));
                return 0;
            }
        }
    } else {
        file_t *f = file_get(file);
        if (!f) { void_set_last_error(ERROR_INVALID_HANDLE_); return 0; }
        packed = f->packed;
        if (packed && (protect & 0xFFu) != 0x02u) { void_set_last_error(ERROR_ACCESS_DENIED_); return 0; }
        archive = archive_path(f->host_path) || packed;
        if (archive && (protect & 0xFFu) != 0x02u) archive_changed(f->host_path);
        bias = f->bias;
        struct stat st;
        if (file_stat(f, &st)) { void_set_last_error(ERROR_INVALID_HANDLE_); return 0; }
        if (f->delta) {
            uint32_t retained = void_handle_dup(file);
            if (!retained) { void_set_last_error(ERROR_NOT_ENOUGH_MEMORY_); return 0; }
            void_blocking_begin();
            fd = voice_delta_mapping(f);
            int error = errno;
            void_blocking_end();
            void_handle_close(retained);
            errno = error;
        } else fd = dup(f->fd);
        if (fd < 0) { void_set_last_error(void_errno_to_win32(errno)); return 0; }
        if (!size) size = (uint64_t)st.st_size;
        else if (size > (uint64_t)st.st_size && (protect & 0xFFu) == 0x04u) {
            if (ftruncate(fd, (off_t)size)) { close(fd); void_set_last_error(ERROR_ACCESS_DENIED_); return 0; }
        }
    }
    void_object *o = void_object_new(VOID_OBJ_MAPPING, sizeof(mapping_t));
    mapping_t *m = (mapping_t *)o->data;
    m->fd = fd;
    m->size = size;
    m->protect = protect;
    m->archive = archive;
    m->packed = packed;
    m->bias = bias;
    m->identity = ++mapping_identity;
    o->destroy = mapping_destroy;
    void_set_last_error(0);
    return void_handle_new(o);
}

WINAPI_EXPORT(K32, OpenFileMappingA, 3)
{
    void_set_last_error(ERROR_FILE_NOT_FOUND_);
    return 0;
}

WINAPI_EXPORT(K32, MapViewOfFile, 5)
{
    void_object *o = void_handle_get(ARG(0), VOID_OBJ_MAPPING);
    if (!o) { void_set_last_error(ERROR_INVALID_HANDLE_); return 0; }
    mapping_t *m = (mapping_t *)o->data;
    uint32_t access = ARG(1);
    uint64_t offset = ((uint64_t)ARG(2) << 32) | ARG(3);
    uint64_t size = ARG(4) ? ARG(4) : m->size - offset;
    if (offset > m->size || size > 0x7FFF0000u || !size) { void_set_last_error(ERROR_INVALID_PARAMETER_); return 0; }
    int writable = (access & 2u) != 0 || (access & 1u) != 0;
    /* FILE_MAP_ALL_ACCESS (0xF001F) includes the COPY bit as well as WRITE.
     * Windows defines it as FILE_MAP_WRITE, so WRITE takes precedence: treating
     * every bit-0 request as private silently loses H5's mapped save payload. */
    int shared = !(access & 1u) || (access & 2u);
    if (m->packed && writable && shared) { void_set_last_error(ERROR_ACCESS_DENIED_); return 0; }
    if (m->archive && writable && shared)
        __atomic_store_n(&archive_cache_disabled, 1, __ATOMIC_RELEASE);
    uint64_t start = void_perf_now_ns();
    uint32_t va = 0;
    int read_fd = -1;
    /* A read-only window of a read-only mapping (the .pak archives) is read
     * in one call: on shared storage each page fault of a file mapping is a
     * separate FUSE round trip taken with the guest machine held. */
    if (!writable && (m->protect & 0xFFu) == 0x02u && size <= (16u << 20)) {
        int cached = m->archive && size <= ARCHIVE_READ_AHEAD &&
            !__atomic_load_n(&archive_cache_disabled, __ATOMIC_ACQUIRE);
        if (cached && m->cached && offset >= m->cached_offset &&
            offset - m->cached_offset <= m->cached_size && size <= m->cached_size - (offset - m->cached_offset)) {
            va = void_vm_alloc(0, (uint32_t)size, 0x3000u, 0x04u);
            if (va) memcpy(GPTR(va), m->cached + (offset - m->cached_offset), (size_t)size);
        }
        if (!va) {
            /* Its own descriptor: the mapping may be closed by another thread meanwhile. */
            read_fd = dup(m->fd);
            va = read_fd >= 0 ? void_vm_alloc(0, (uint32_t)size, 0x3000u, 0x04u) : 0;
            if (va) {
                uint64_t identity = m->identity;
                uint32_t handle = ARG(0);
                uint64_t read_size = cached ? (m->size - offset < ARCHIVE_READ_AHEAD ? m->size - offset : ARCHIVE_READ_AHEAD) : size;
                if (read_size < size) read_size = size;
                uint8_t *block = cached ? malloc((size_t)read_size) : NULL;
                if (!block) read_size = size;
                uint8_t *target = block ? block : GPTR(va);
                void_blocking_begin();
                uint64_t done = 0;
                int failed = 0;
                while (done < read_size) {
                    ssize_t got = pread(read_fd, target + done, (size_t)(read_size - done), (off_t)(m->bias + offset + done));
                    if (got < 0 && errno == EINTR) continue;
                    if (got < 0) { failed = 1; break; }
                    if (got == 0) break;          /* past the end: zero-filled, as a mapping */
                    done += (uint64_t)got;
                }
                void_blocking_end();
                if (block && !failed) {
                    memset(block + done, 0, (size_t)(read_size - done));
                    memcpy(GPTR(va), block, (size_t)size);
                    /* The original handle can have been closed and reused during I/O. */
                    void_object *current = void_handle_get(handle, VOID_OBJ_MAPPING);
                    mapping_t *live = current ? current->data : NULL;
                    if (live && live->identity == identity && !__atomic_load_n(&archive_cache_disabled, __ATOMIC_ACQUIRE)) {
                        free(live->cached); live->cached = block; block = NULL;
                        live->cached_offset = offset; live->cached_size = (uint32_t)read_size;
                    }
                }
                free(block);
                if (failed) {
                    /* A read error is not data: let the real mapping report it. */
                    void_vm_free(va, 0, 0x8000u);
                    va = 0;
                }
            }
        }
        if (va) {
            uint32_t old = 0;
            void_vm_protect(va, (uint32_t)size, 0x02u, &old);
        }
    }
    if (!va) va = void_vm_map_file(read_fd >= 0 ? read_fd : m->fd, m->bias + offset, (uint32_t)size, writable, shared);
    if (read_fd >= 0) close(read_fd);
    void_perf_add(VOID_PERF_FILE_MAP, void_perf_now_ns() - start, size);
    VLOG("[void] MapViewOfFile(%08X, off %llX, %llX) = %08X", ARG(0), (unsigned long long)offset,
        (unsigned long long)size, va);
    return va;
}

WINAPI_EXPORT(K32, UnmapViewOfFile, 1) { return (uint32_t)void_vm_unmap_file(ARG(0)); }

/* Shared views write through the page cache; msync only orders the write-back. */
WINAPI_EXPORT(K32, FlushViewOfFile, 2)
{
    uint32_t address = ARG(0), size = ARG(1);
    if (!address || !void_vm_readable(address, 1)) { void_set_last_error(487u); return 0; }  /* ERROR_INVALID_ADDRESS */
    uint32_t page = address & ~(GUEST_PAGE - 1);
    uint32_t span = size ? size + (address - page) : GUEST_PAGE;
    span = (span + GUEST_PAGE - 1) & ~(GUEST_PAGE - 1);
    if (msync(GPTR(page), span, MS_ASYNC)) VLOG("[void] FlushViewOfFile(%08X): %s", address, strerror(errno));
    return 1;
}

static int filetime_to_unix(uint64_t value, struct timespec *out)
{
    /* The high bit denotes negative NT relative times, not an absolute FILETIME.
     * The documented -1 sentinel is handled by SetFileTime before conversion. */
    if (value > INT64_MAX) return 0;
    int64_t seconds = (int64_t)(value / 10000000u) - INT64_C(11644473600);
    out->tv_sec = (time_t)seconds;
    out->tv_nsec = (long)(value % 10000000u) * 100;
    return (int64_t)out->tv_sec == seconds;
}

WINAPI_EXPORT(K32, SetFileTime, 4)
{
    file_t *f = file_get(ARG(0));
    if (!f || f->fd < 0 || f->std_stream) { void_set_last_error(ERROR_INVALID_HANDLE_); return 0; }
    /* GENERIC_WRITE and GENERIC_ALL include FILE_WRITE_ATTRIBUTES (0x100). */
    if (f->delta || !(f->granted_access & (GENERIC_WRITE_ | 0x10000000u | 0x100u))) {
        void_set_last_error(ERROR_ACCESS_DENIED_); return 0;
    }
    uint64_t requested[3] = {0, 0, 0};
    for (unsigned i = 0; i < 3; ++i) {
        uint32_t address = ARG(i + 1);
        if (address) {
            if (!void_vm_readable(address, 8)) { void_set_last_error(998u); return 0; } /* ERROR_NOACCESS */
            memcpy(&requested[i], GPTR(address), 8);
        }
    }
    struct stat st;
    if (file_stat(f, &st)) { void_set_last_error(void_errno_to_win32(errno)); return 0; }
    /* POSIX cannot change creation time. H5's saving path passes the unchanged
     * GetFileTime creation value; accept that, and report other requests rather
     * than claiming to have changed an unrelated ctime field. */
    if (requested[0] && requested[0] != UINT64_MAX &&
        requested[0] != unix_to_filetime(st.st_ctim.tv_sec, st.st_ctim.tv_nsec)) {
        void_set_last_error(ERROR_NOT_SUPPORTED_); return 0;
    }
    struct timespec times[2] = { {0, UTIME_OMIT}, {0, UTIME_OMIT} };
    struct timespec kept[2] = { f->kept_times[0], f->kept_times[1] };
    unsigned mask = f->keep_times;
    int update = 0;
    for (unsigned i = 0; i < 2; ++i) {
        uint64_t value = requested[i + 1];
        if (!value) continue;          /* NULL and a zero FILETIME leave it alone. */
        if (value == UINT64_MAX) {
            kept[i] = i ? st.st_mtim : st.st_atim;
            mask |= 1u << i;
        } else {
            if (!filetime_to_unix(value, &times[i])) { void_set_last_error(ERROR_INVALID_PARAMETER_); return 0; }
            mask &= ~(1u << i);
            update = 1;
        }
    }
    if (update) {
        int result;
        do { result = futimens(f->fd, times); } while (result && errno == EINTR);
        if (result) { void_set_last_error(void_errno_to_win32(errno)); return 0; }
    }
    f->kept_times[0] = kept[0]; f->kept_times[1] = kept[1]; f->keep_times = mask;
    return 1;
}

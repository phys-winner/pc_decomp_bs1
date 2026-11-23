#define _GNU_SOURCE
#include "data_obb.h"
#include "host.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <zlib.h>
#ifdef __ANDROID__
#include <android/asset_manager_jni.h>
#endif

#define OBB_MAX_PARTS 8
#define OBB_MAX_FILES 16384
#define OBB_READ_WHOLE_LIMIT (512u << 20)

typedef struct obb_part obb_part;
typedef struct {
    obb_part *part;
    char *path;
    char *folded;
    char *parent;
    uint64_t offset;
    uint64_t size;
    uint32_t crc;
} obb_entry;

struct obb_part {
    int fd;
    uint64_t base;
    uint64_t size;
    uint32_t first;
    uint32_t count;
    obb_part *next;
};

static obb_part *parts;
static obb_entry *entries;
static uint32_t entry_count;
static uint32_t entry_capacity;
static uint32_t part_count;

static uint16_t u16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static uint32_t u32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
static uint64_t u64(const uint8_t *p) { return (uint64_t)u32(p) | (uint64_t)u32(p + 4) << 32; }

static int zip64_value(const uint8_t *extra, size_t length, size_t *position, uint64_t *value)
{
    if (length < 4 || u16(extra) != 1) return -1;
    size_t available = u16(extra + 2);
    if (available > length - 4 || *position > available || available - *position < 8) return -1;
    *value = u64(extra + 4 + *position);
    *position += 8;
    return 0;
}

static int range64(uint64_t value, uint64_t size, uint64_t limit)
{
    return value <= limit && size <= limit - value;
}

static int read_at(int fd, uint64_t base, uint64_t offset, void *buffer, size_t size)
{
    if (!range64(base, offset, UINT64_MAX) || base + offset > INT64_MAX) return -1;
    size_t done = 0;
    while (done < size) {
        ssize_t got = pread(fd, (uint8_t *)buffer + done, size - done, (off_t)(base + offset + done));
        if (got < 0 && errno == EINTR) continue;
        if (got <= 0) return -1;
        done += (size_t)got;
    }
    return 0;
}

static char *normalized_path(const char *path, char out[1024])
{
    if (!path) return NULL;
    char work[1024];
    if (strlen(path) >= sizeof work) return NULL;
    strcpy(work, path);
    for (char *p = work; *p; ++p) if (*p == '\\') *p = '/';
    const char *relative = work;
    char selected_root[1024] = "";
    const char *selected = getenv("PORT_PART_GAME");
    if (selected && *selected && snprintf(selected_root, sizeof selected_root, "%s", selected) >= (int)sizeof selected_root)
        return NULL;
    const char *prefixes[] = { selected_root, "c:/games/bioshock", "/sdcard/bioshock", "/storage/emulated/0/bioshock" };
    for (unsigned i = 0; i < sizeof prefixes / sizeof *prefixes; ++i) {
        size_t n = strlen(prefixes[i]);
        if (n && !strncasecmp(relative, prefixes[i], n) && (!relative[n] || relative[n] == '/')) {
            relative += n;
            break;
        }
    }
    while (*relative == '/') ++relative;
    char prefix[8] = "";
    if (strncasecmp(relative, "game", 4) && strncasecmp(relative, "images", 6)) {
        strcpy(prefix, "game/");
    } else if (!strncasecmp(relative, "game/", 5) || !strncasecmp(relative, "images/", 7)) {
        prefix[0] = 0;
    } else if (!strcasecmp(relative, "game") || !strcasecmp(relative, "images")) {
        snprintf(out, 1024, "%s", relative);
        return out;
    } else {
        return NULL;
    }
    char *write = out;
    size_t remaining = 1024;
    int written = snprintf(write, remaining, "%s", prefix);
    if (written < 0 || (size_t)written >= remaining) return NULL;
    write += written;
    remaining -= (size_t)written;
    const char *p = relative;
    while (*p) {
        while (*p == '/') ++p;
        if (!*p) break;
        const char *end = strchr(p, '/');
        size_t length = end ? (size_t)(end - p) : strlen(p);
        if (length == 2 && !memcmp(p, "..", 2)) return NULL;
        if (length == 1 && !memcmp(p, ".", 1)) { p = end ? end + 1 : p + length; continue; }
        if (write != out && write[-1] != '/') {
            if (remaining < 2) return NULL;
            *write++ = '/';
            --remaining;
        }
        if (length >= remaining) return NULL;
        memcpy(write, p, length);
        write[length] = 0;
        write += length;
        remaining -= length;
        p = end ? end + 1 : p + length;
    }
    if (write == out && prefix[0]) return NULL;
    if (write > out && write[-1] == '/') write[-1] = 0;
    return out;
}

static char *parent_path(const char *path)
{
    const char *slash = strrchr(path, '/');
    return slash ? strndup(path, (size_t)(slash - path)) : strdup("");
}

static int parse_part(obb_part *part)
{
    if (part->size < 22) return -1;
    size_t tail_size = part->size < 65557 ? (size_t)part->size : 65557;
    uint8_t *tail = malloc(tail_size);
    if (!tail) return -1;
    uint64_t tail_offset = part->size - tail_size;
    int result = read_at(part->fd, part->base, tail_offset, tail, tail_size);
    uint64_t cd_offset = 0, cd_size = 0, count = 0;
    if (!result) {
        for (size_t i = tail_size - 22;; --i) {
            if (u32(tail + i) == 0x06054b50u && i + 22 + u16(tail + i + 20) == tail_size) {
                const uint8_t *e = tail + i;
                cd_size = u32(e + 12);
                cd_offset = u32(e + 16);
                count = u16(e + 10);
                if (cd_size == UINT32_MAX || cd_offset == UINT32_MAX || count == 0xFFFFu) {
                    if (i < 20 || u32(tail + i - 20) != 0x07064b50u) { result = -1; break; }
                    uint64_t zip64_offset = u64(tail + i - 20 + 8);
                    uint8_t z[56];
                    if (!range64(zip64_offset, sizeof z, part->size) || read_at(part->fd, part->base, zip64_offset, z, sizeof z) || u32(z) != 0x06064b50u) {
                        result = -1;
                        break;
                    }
                    count = u64(z + 32);
                    cd_size = u64(z + 40);
                    cd_offset = u64(z + 48);
                }
                break;
            }
            if (!i) { result = -1; break; }
        }
    }
    free(tail);
    if (result || count > OBB_MAX_FILES || !range64(cd_offset, cd_size, part->size)) return -1;
    void_log("[obb] directory count=%llu offset=%llu bytes=%llu", (unsigned long long)count, (unsigned long long)cd_offset, (unsigned long long)cd_size);
    part->first = entry_count;
    part->count = (uint32_t)count;
    uint8_t central[46];
    uint64_t position = cd_offset;
    for (uint32_t index = 0; index < count; ++index) {
        if (!range64(position, sizeof central, cd_offset + cd_size) || read_at(part->fd, part->base, position, central, sizeof central) ||
            u32(central) != 0x02014b50u) return -1;
        uint16_t flags = u16(central + 8), method = u16(central + 10), name_length = u16(central + 28), extra_length = u16(central + 30), comment_length = u16(central + 32);
        uint64_t next = position + 46ull + name_length + extra_length + comment_length;
        if (flags & 1 || method || next > cd_offset + cd_size) return -1;
        char *name = malloc((size_t)name_length + 1);
        if (!name) return -1;
        if (read_at(part->fd, part->base, position + 46, name, name_length)) { free(name); return -1; }
        name[name_length] = 0;
        if (strlen(name) != name_length || strchr(name, '\\')) { free(name); return -1; }
        uint64_t size = u32(central + 24), compressed = u32(central + 20), local = u32(central + 42), crc = u32(central + 16);
        uint8_t extra[64];
        size_t extra_read = extra_length < sizeof extra ? extra_length : sizeof extra;
        if (read_at(part->fd, part->base, position + 46 + name_length, extra, extra_read)) { free(name); return -1; }
        size_t used = 0;
        if (size == UINT32_MAX && zip64_value(extra, extra_read, &used, &size)) { free(name); return -1; }
        if (compressed == UINT32_MAX && zip64_value(extra, extra_read, &used, &compressed)) { free(name); return -1; }
        if (local == UINT32_MAX && zip64_value(extra, extra_read, &used, &local)) { free(name); return -1; }
        if (size != compressed || !range64(local, 30, part->size)) { free(name); return -1; }
        uint8_t header[30];
        if (read_at(part->fd, part->base, local, header, sizeof header) || u32(header) != 0x04034b50u ||
            u16(header + 26) != name_length || u16(header + 8) != method || (u16(header + 6) & 1)) { free(name); return -1; }
        uint16_t local_extra = u16(header + 28);
        uint64_t local_size = u32(header + 22), local_compressed = u32(header + 18);
        uint64_t data = local + 30ull + name_length + local_extra;
        if (local_size == UINT32_MAX || local_compressed == UINT32_MAX) {
            uint8_t zip64[24];
            if (local_extra < 20 || read_at(part->fd, part->base, local + 30 + name_length, zip64, sizeof zip64) ||
                u16(zip64) != 1 || u16(zip64 + 2) < 16) { free(name); return -1; }
            if (local_size == UINT32_MAX) local_size = u64(zip64 + 4);
            if (local_compressed == UINT32_MAX) local_compressed = u64(zip64 + 12);
        }
        if (local_size != size || local_compressed != compressed || !range64(data, size, part->size)) { free(name); return -1; }
        char normalized[1024];
        if (!normalized_path(name, normalized)) { free(name); return -1; }
        char *folded = strdup(normalized);
        char *parent = parent_path(normalized);
        if (!folded || !parent) { free(folded); free(parent); free(name); return -1; }
        for (char *p = folded; *p; ++p) if (*p >= 'A' && *p <= 'Z') *p = (char)(*p + 32);
        for (uint32_t i = part->first; i < entry_count; ++i) {
            if (!strcmp(entries[i].folded, folded)) { free(folded); free(parent); free(name); return -1; }
        }
        if (entry_count == entry_capacity) {
            uint32_t capacity = entry_capacity ? entry_capacity * 2 : 256;
            obb_entry *grown = realloc(entries, (size_t)capacity * sizeof *entries);
            if (!grown) { free(folded); free(parent); free(name); return -1; }
            entries = grown;
            entry_capacity = capacity;
        }
        entries[entry_count++] = (obb_entry){ part, name, folded, parent, data, size, crc };
        position = next;
    }
    return 0;
}

static const char *const container_names[] = { "main.1.org.bioshock.port.obb", "patch.1.org.bioshock.port.obb" };

static int append_part_fd(int fd, uint64_t base, uint64_t size, const char *name)
{
    struct stat st;
    obb_part *part = part_count < OBB_MAX_PARTS ? calloc(1, sizeof *part) : NULL;
    if (!part || fd < 0 || fstat(fd, &st) || st.st_size < 0 || (size != UINT64_MAX && !range64(base, size, (uint64_t)st.st_size))) {
        void_log("[obb] invalid container range %s", name);
        free(part);
        close(fd);
        void_obb_close();
        return -1;
    }
    part->fd = fd;
    part->base = base;
    part->size = size == UINT64_MAX ? (uint64_t)st.st_size : size;
    if (parse_part(part)) {
        void_log("[obb] invalid container %s", name);
        free(part);
        close(fd);
        void_obb_close();
        return -1;
    }
    part->next = parts;
    parts = part;
    ++part_count;
    return 0;
}

int void_obb_ready(void)
{
    return part_count == 2 && entry_count != 0;
}

#ifdef __ANDROID__
int void_obb_init_assets(JNIEnv *env, jobject assets)
{
    void_obb_close();
    AAssetManager *manager = AAssetManager_fromJava(env, assets);
    if (!manager) return -1;
    AAsset *opened[2] = { NULL, NULL };
    for (unsigned i = 0; i < 2; ++i) opened[i] = AAssetManager_open(manager, container_names[i], AASSET_MODE_RANDOM);
    if (!opened[0] && !opened[1]) return 0;
    if (!opened[0] || !opened[1]) {
        void_log("[obb] incomplete embedded main/patch set");
        goto fail;
    }
    for (unsigned i = 0; i < 2; ++i) {
        off64_t base = 0, size = 0;
        int asset_fd = AAsset_openFileDescriptor64(opened[i], &base, &size);
        if (asset_fd < 0 || base < 0 || size < 0) {
            void_log("[obb] embedded asset is not directly readable: %s", container_names[i]);
            goto fail;
        }
        int fd = dup(asset_fd);
        close(asset_fd);
        if (fd < 0 || append_part_fd(fd, (uint64_t)base, (uint64_t)size, container_names[i])) goto fail;
        AAsset_close(opened[i]);
        opened[i] = NULL;
    }
    void_log("[obb] direct-read embedded containers ready: %u files", entry_count);
    return 0;
fail:
    for (unsigned i = 0; i < 2; ++i) if (opened[i]) AAsset_close(opened[i]);
    void_obb_close();
    return -1;
}
#endif

int void_obb_init_external(const char *directory)
{
    void_obb_close();
    if (!directory) return 0;
    for (unsigned i = 0; i < 2; ++i) {
        char path[1200];
        int length = snprintf(path, sizeof path, "%s/%s", directory, container_names[i]);
        if (length < 0 || length >= (int)sizeof path) { void_obb_close(); return -1; }
        int fd = open(path, O_RDONLY | O_CLOEXEC);
        if (fd < 0) {
            if (i == 0 && errno == ENOENT) return 0;
            void_log("[obb] cannot open required container %s: %s", path, strerror(errno));
            void_obb_close();
            return -1;
        }
        if (append_part_fd(fd, 0, UINT64_MAX, path)) return -1;
    }
    void_log("[obb] direct-read external containers ready: %u files", entry_count);
    return 0;
}

static obb_entry *find_entry(const char *path)
{
    char normalized[1024];
    if (!normalized_path(path, normalized)) return NULL;
    for (uint32_t i = entry_count; i-- > 0;) {
        if (!strcasecmp(entries[i].folded, normalized)) return &entries[i];
    }
    return NULL;
}

static const char *relative_child(const obb_entry *entry, const char *directory, size_t length)
{
    if (!length) return entry->path;
    if (strncasecmp(entry->path, directory, length) || entry->path[length] != '/') return NULL;
    return entry->path + length + 1;
}

int void_obb_list(const char *directory, unsigned index, char *name, size_t capacity, int *is_directory, struct stat *st)
{
    if (!name || !capacity) return -1;
    char normalized[1024];
    if (!normalized_path(directory, normalized)) return -1;
    size_t length = strlen(normalized), seen = 0;
    for (uint32_t i = 0; i < entry_count; ++i) {
        const char *relative = relative_child(&entries[i], normalized, length);
        if (!relative || !*relative) continue;
        size_t child_length = strcspn(relative, "/");
        int duplicate = 0;
        for (uint32_t previous = 0; previous < i && !duplicate; ++previous) {
            const char *other = relative_child(&entries[previous], normalized, length);
            duplicate = other && strcspn(other, "/") == child_length && !strncasecmp(other, relative, child_length);
        }
        if (duplicate || seen++ != index) continue;
        if (child_length >= capacity) return -1;
        memcpy(name, relative, child_length);
        name[child_length] = 0;
        int directory_entry = relative[child_length] == '/';
        if (is_directory) *is_directory = directory_entry;
        if (st) {
            memset(st, 0, sizeof *st);
            st->st_mode = directory_entry ? S_IFDIR | 0755 : S_IFREG | 0444;
            if (!directory_entry) st->st_size = (off_t)entries[i].size;
        }
        return 0;
    }
    return 1;
}

int void_obb_lookup(const char *path, obb_file *out)
{
    if (!out) return -1;
    obb_entry *entry = find_entry(path);
    if (!entry) return -1;
    int fd = dup(entry->part->fd);
    if (fd < 0) return -1;
    *out = (obb_file){ fd, entry->part->base + entry->offset, entry->size, entry->offset, entry->crc, entry->path };
    return 0;
}

int void_obb_stat(const char *path, struct stat *out)
{
    if (!out) return -1;
    char normalized[1024];
    if (!normalized_path(path, normalized)) return -1;
    obb_entry *entry = find_entry(normalized);
    memset(out, 0, sizeof *out);
    if (entry) {
        out->st_mode = S_IFREG | 0444;
        out->st_size = (off_t)entry->size;
        return 0;
    }
    char child[512];
    if (!void_obb_list(normalized, 0, child, sizeof child, NULL, NULL)) {
        out->st_mode = S_IFDIR | 0755;
        return 0;
    }
    return -1;
}

int void_obb_read(const char *path, void **data, size_t *size)
{
    if (!data || !size) return -1;
    *data = NULL;
    *size = 0;
    obb_file file;
    if (void_obb_lookup(path, &file)) return -1;
    if (file.size > OBB_READ_WHOLE_LIMIT || file.size > SIZE_MAX - 1) { close(file.fd); return -1; }
    uint8_t *buffer = malloc((size_t)file.size + 1);
    if (!buffer) { close(file.fd); return -1; }
    uint64_t base = file.bias - file.archive_offset;
    int result = read_at(file.fd, base, file.archive_offset, buffer, (size_t)file.size);
    close(file.fd);
    if (!result && crc32(0, buffer, (uInt)file.size) != file.crc32) result = -1;
    if (result) { free(buffer); return -1; }
    buffer[file.size] = 0;
    *data = buffer;
    *size = (size_t)file.size;
    return 0;
}

void void_obb_close(void)
{
    for (uint32_t i = 0; i < entry_count; ++i) {
        free(entries[i].path);
        free(entries[i].folded);
        free(entries[i].parent);
    }
    free(entries);
    entries = NULL;
    entry_count = entry_capacity = 0;
    while (parts) {
        obb_part *next = parts->next;
        close(parts->fd);
        free(parts);
        parts = next;
    }
    part_count = 0;
}

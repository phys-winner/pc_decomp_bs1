/* The launcher's texture card for BioShock (libtexconv.so, org.bioshock.port.TextureConverter): every DXT1/3/5
 * Texture of the game's packages and bulk files, encoded once on the device into ASTC 4x4 or ETC2,
 * <cache>/Content/port-textures.tvt (UTX3 v2, android/texconv/ut3tx.h), where the cache root is the game folder
 * (or the launcher's separate cache root).  The game's files are a folder or a stored ZIP the launcher plays in
 * place (zipmount.h: its index, the archive's descriptor and the sidecar folder, which then is the game folder
 * and holds the pack).  The game reads the pack through android/host/texture_cache.c; the native Direct3D 10
 * renderer looks each mip level up by its own blocks (android/render10/r10_pack.c) on GPUs that cannot sample
 * the BC format (OpenGL ES on PowerVR, Adreno's and Mali's system drivers).
 *
 * A port of tools/bioshock_textures.py (the PC cooker, same readers and container), with two differences
 * that matter for the renderer's per-level lookups:
 *   - every .bsm package of Content/Maps is read, the localized maps (rus and the others) included, and the
 *     engine's script packages of Builds/Release (*.U), whose textures the game draws every frame;
 *   - a level whose blocks appear in chains with different lower levels keeps a one-level entry instead of
 *     being dropped (the PC cooker dropped 793 such keys: each one a texture the renderer had to decode).
 * The originals are only read.  The pack is written to port-textures.tvt.part and renamed when complete; a
 * failure or a cancel leaves no pack (an older complete one stays until a new one replaces it).
 *
 * TEXCONV_CLI builds a command-line tool for device checks:
 *   bioshock-tex <game> [--cache <folder>] [--archive <zip> --index <zmi>] scan | convert <family> <threads> | undo */
#include "texconv.h"
#include "ut3tx.h"
#include "../host/zipmount.h"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <dirent.h>
#include <fcntl.h>
#include <map>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <thread>
#include <unistd.h>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <zlib.h>
#ifndef TEXCONV_CLI
#include <jni.h>
#endif

namespace {
typedef std::vector<uint8_t> Bytes;
constexpr size_t MAX_PACKAGE = 256u << 20;
/* The original PC corpus (tools/bioshock_textures.py receipt of 9 November 2026): progress totals until the
 * conversion has found the real ones. */
constexpr int64_t CORPUS_TEXTURES = 20550, CORPUS_BYTES = 1890738776LL;
constexpr uint64_t ASTC_PACK_BYTES = 5196005632ULL;   /* the full ASTC pack with its raw copy */

std::atomic<bool> cancelled{false};
std::atomic<int64_t> done{0}, total{0}, bytes_done{0}, bytes_total{0};
std::mutex message_lock;
std::string message;

void set_message(const std::string &why)
{
    std::lock_guard<std::mutex> guard(message_lock);
    message = why;
}

void require(bool condition, const char *why)
{
    if (!condition) throw std::runtime_error(why);
}

void check_cancel()
{
    if (cancelled) throw std::runtime_error("Cancelled");
}

std::string lower(std::string s)
{
    for (char &c : s) c = (char)tolower((unsigned char)c);
    return s;
}

/* A child of a folder by name, case-insensitively (game data copied from Windows keeps any case). */
std::string child(const std::string &parent, const std::string &name)
{
    std::string exact = parent + "/" + name;
    struct stat st;
    if (!stat(exact.c_str(), &st)) return exact;
    DIR *dir = opendir(parent.c_str());
    if (!dir) return exact;
    std::string want = lower(name), found;
    while (dirent *e = readdir(dir))
        if (lower(e->d_name) == want) { found = parent + "/" + e->d_name; break; }
    closedir(dir);
    return found.empty() ? exact : found;
}

bool ends_with(const std::string &name, const std::string &lower_suffix)
{
    std::string low = lower(name);
    return low.size() > lower_suffix.size() && low.compare(low.size() - lower_suffix.size(), lower_suffix.size(), lower_suffix) == 0;
}

/* The names of a folder's entries ending in suffix. */
std::vector<std::string> folder_names(const std::string &folder, const char *suffix)
{
    std::vector<std::string> out;
    DIR *dir = opendir(folder.c_str());
    if (!dir) return out;
    std::string end = lower(suffix);
    while (dirent *e = readdir(dir))
        if (ends_with(e->d_name, end)) out.push_back(e->d_name);
    closedir(dir);
    return out;
}

/* The pack of a cache root (the game folder, an archive's sidecar folder or the launcher's separate root). */
std::string pack_path(const std::string &cache) { return child(child(cache, "Content"), TVTX_FILE); }

void make_folders(const std::string &path)
{
    struct stat st;
    if (!stat(path.c_str(), &st)) {
        if (!S_ISDIR(st.st_mode)) throw std::runtime_error(path + " is not a folder");
        return;
    }
    size_t slash = path.rfind('/');
    if (slash != std::string::npos && slash > 0) make_folders(path.substr(0, slash));
    if (mkdir(path.c_str(), 0755) && errno != EEXIST) throw std::runtime_error("cannot create " + path + ": " + strerror(errno));
}

Bytes read_file(const std::string &path, size_t limit)
{
    int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) throw std::runtime_error("cannot open " + path + ": " + strerror(errno));
    struct stat st;
    if (fstat(fd, &st) || st.st_size < 0 || (uint64_t)st.st_size > limit) { close(fd); throw std::runtime_error("unexpected size of " + path); }
    Bytes data((size_t)st.st_size);
    size_t at = 0;
    while (at < data.size()) {
        ssize_t n = pread(fd, data.data() + at, data.size() - at, (off_t)at);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) { close(fd); throw std::runtime_error("cannot read " + path); }
        at += (size_t)n;
    }
    close(fd);
    return data;
}

/* ------------------------------------------------------------------ the game's files */
/* A folder, or a stored ZIP the launcher plays in place (zipmount.h: its index of the archive, the archive's
 * read-only descriptor and the sidecar folder with the few files it unpacked).  As in the game's own file layer
 * (android/host/files.c), an archive entry wins over a sidecar file of the same path; the entries the launcher
 * unpacked (ZM_SIDECAR) and the files only the sidecar has are read from the sidecar. */
struct Source {
    std::string name;                 /* the file's name, for messages; empty: there is no such file */
    std::string host;                 /* a file of the folder or the sidecar; empty for an archive entry */
    const zm_entry *entry = nullptr;  /* a file stored in the archive, read through its descriptor */
    explicit operator bool() const { return !name.empty(); }
};

/* A bulk file: mapped, or read through the archive's descriptor when that cannot be mapped. */
struct Mapped {
    const uint8_t *data = nullptr;
    uint64_t size = 0;
    void *base = nullptr;             /* what munmap needs */
    size_t length = 0;
    const zm_mount *archive = nullptr;
    const zm_entry *entry = nullptr;
    std::string name;
    Mapped() = default;
    Mapped(const Mapped &) = delete;
    Mapped &operator=(const Mapped &) = delete;
    ~Mapped() { if (base && length) munmap(base, length); }

    /* n bytes from at: inside the mapping, or copied into scratch */
    const uint8_t *bytes(uint64_t at, size_t n, Bytes &scratch) const
    {
        if (data) return data + at;
        scratch.resize(n);
        for (size_t got = 0; got < n;) {
            long long r = zm_pread(archive, entry, scratch.data() + got, n - got, at + got);
            if (r <= 0) throw std::runtime_error("cannot read " + name + " from the game archive");
            got += (size_t)r;
        }
        return scratch.data();
    }
};

struct Game {
    std::string root;                 /* the game folder, or the archive's sidecar folder */
    zm_mount *archive = nullptr;

    Game(const std::string &folder, const std::string &index, int fd) : root(folder)
    {
        if (index.empty()) return;
        require(fd >= 0, "the game archive is not open");
        archive = zm_open(fd, index.c_str(), folder.c_str());
        require(archive != nullptr, "the game archive's index is unreadable or the archive has changed; select the archive again");
    }
    ~Game() { if (archive) zm_close(archive); }
    Game(const Game &) = delete;
    Game &operator=(const Game &) = delete;

    /* A relative path ("Content/Maps") in the folder, each component found ignoring case. */
    std::string host(const std::string &relative) const
    {
        std::string path = root;
        for (size_t at = 0; at < relative.size();) {
            size_t end = relative.find('/', at);
            if (end == std::string::npos) end = relative.size();
            if (end > at) path = child(path, relative.substr(at, end - at));
            at = end + 1;
        }
        return path;
    }

    Source entry_source(const zm_entry *e, const std::string &name) const
    {
        Source s;
        s.name = name;
        if (e->flags & ZM_SIDECAR) {
            char path[4096];
            require(zm_sidecar_path(archive, e, path, sizeof path) == 0, "invalid sidecar path in the game archive's index");
            s.host = path;
        } else {
            s.entry = e;
        }
        return s;
    }

    /* A file by its relative path ("Content/BulkContent/Catalog.bdc"); false when there is none. */
    Source find(const std::string &relative) const
    {
        std::string name = relative.substr(relative.rfind('/') + 1);
        if (archive)
            if (const zm_entry *e = zm_find(archive, relative.c_str())) return entry_source(e, name);
        Source s;
        struct stat st;
        std::string path = host(relative);
        if (!stat(path.c_str(), &st) && S_ISREG(st.st_mode)) { s.name = name; s.host = path; }
        return s;
    }

    /* The files of a folder ending in suffix (not those of its subfolders), by name ignoring case. */
    std::vector<Source> list(const std::string &folder, const char *suffix) const
    {
        std::map<std::string, Source> found;
        std::string end = lower(suffix);
        if (archive) {
            std::vector<const zm_entry *> entries;
            zm_list(archive, folder.c_str(), [](const zm_entry *e, void *user) {
                static_cast<std::vector<const zm_entry *> *>(user)->push_back(e);
                return 0;
            }, &entries);
            for (const zm_entry *e : entries) {
                if (e->length <= folder.size() + 1) continue;
                std::string name(e->path + folder.size() + 1, e->length - folder.size() - 1);
                if (name.find_first_of("/\\") == std::string::npos && ends_with(name, end)) found.emplace(lower(name), entry_source(e, name));
            }
        }
        std::string path = host(folder);
        for (const std::string &name : folder_names(path, suffix)) {
            Source s;
            s.name = name;
            s.host = path + "/" + name;
            found.emplace(lower(name), std::move(s));   /* an archive entry of the same name stays */
        }
        std::vector<Source> out;
        for (auto &item : found) out.push_back(std::move(item.second));
        return out;
    }

    Bytes read(const Source &s, size_t limit) const
    {
        if (!s.entry) return read_file(s.host, limit);
        if (s.entry->size > limit) throw std::runtime_error("unexpected size of " + s.name);
        Bytes data((size_t)s.entry->size);
        for (size_t at = 0; at < data.size();) {
            long long n = zm_pread(archive, s.entry, data.data() + at, data.size() - at, at);
            if (n <= 0) throw std::runtime_error("cannot read " + s.name + " from the game archive");
            at += (size_t)n;
        }
        return data;
    }

    std::unique_ptr<Mapped> map(const Source &s) const
    {
        std::unique_ptr<Mapped> m(new Mapped);
        m->name = s.name;
        if (s.entry) {
            m->size = s.entry->size;
            m->archive = archive;
            m->entry = s.entry;
            void *base = nullptr;
            size_t length = 0;
            void *p = m->size ? zm_map(archive, s.entry, 0, (size_t)m->size, &base, &length) : nullptr;
            if (p) { m->data = (const uint8_t *)p; m->base = base; m->length = length; }
            return m;
        }
        int fd = open(s.host.c_str(), O_RDONLY | O_CLOEXEC);
        if (fd < 0) throw std::runtime_error("cannot open " + s.host + ": " + strerror(errno));
        struct stat st;
        if (fstat(fd, &st) || st.st_size <= 0) { close(fd); throw std::runtime_error("cannot read " + s.host); }
        void *p = mmap(nullptr, (size_t)st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
        close(fd);
        if (p == MAP_FAILED) throw std::runtime_error("cannot map " + s.host);
        m->data = (const uint8_t *)p;
        m->size = (uint64_t)st.st_size;
        m->base = p;
        m->length = (size_t)st.st_size;
        return m;
    }
};

uint32_t le32(const uint8_t *p) { return tvtx_le32(p); }

struct Reader {
    const uint8_t *data;
    size_t size, position;
    Reader(const uint8_t *d, size_t n, size_t at = 0) : data(d), size(n), position(at) {}
    const uint8_t *take(size_t n)
    {
        require(n <= size - position && position <= size, "truncated package field");
        const uint8_t *p = data + position;
        position += n;
        return p;
    }
    uint32_t u32() { return le32(take(4)); }
    int32_t i32() { return (int32_t)u32(); }
    uint64_t u64() { const uint8_t *p = take(8); return (uint64_t)le32(p) | (uint64_t)le32(p + 4) << 32; }
    int32_t compact()
    {
        uint32_t value = *take(1);
        bool negative = value & 128, more = value & 64;
        uint32_t result = value & 63, shift = 6;
        while (more) {
            require(shift <= 27, "oversized compact index");
            value = *take(1);
            result |= (value & 127) << shift;
            shift += 7;
            more = value & 128;
        }
        require(result <= 0x7fffffffu, "compact index overflow");
        return negative ? -(int32_t)result : (int32_t)result;
    }
    std::string string()
    {
        int32_t count = compact();
        require(count >= 0 && count <= 4096, "invalid BioShock string length");
        if (!count) return std::string();
        const uint8_t *p = take((size_t)count * 2);
        require(p[2 * count - 2] == 0 && p[2 * count - 1] == 0, "unterminated BioShock string");
        std::string out;   /* UTF-16LE to UTF-8 (names and paths are ASCII; others only compare) */
        for (int32_t i = 0; i + 1 < count; ++i) {
            uint32_t c = p[2 * i] | p[2 * i + 1] << 8;
            if (c < 0x80) out += (char)c;
            else if (c < 0x800) { out += (char)(0xC0 | c >> 6); out += (char)(0x80 | (c & 63)); }
            else { out += (char)(0xE0 | c >> 12); out += (char)(0x80 | (c >> 6 & 63)); out += (char)(0x80 | (c & 63)); }
        }
        return out;
    }
};

/* ------------------------------------------------------------------ packages (PC v141) */
struct Export { std::string name, cls; int32_t class_ref; uint32_t offset, size; };
struct Package {
    Bytes data;
    std::vector<std::string> names;
    std::vector<Export> exports;
};

Package package(const Game &game, const Source &source)
{
    Bytes raw = game.read(source, 1u << 30);
    require(raw.size() >= 72 && le32(raw.data()) == 0x9e2a83c1u && le32(raw.data() + 4) == 0x38008du,
            "not a BioShock PC v141 package");
    Package p;
    if (le32(raw.data() + 8) == 1) {
        require(raw.size() <= MAX_PACKAGE, "package exceeds 256 MiB");
        p.data = std::move(raw);
    } else {
        /* [64] block count, [68] the block offsets, the first of which is where the blocks start */
        uint32_t count = le32(raw.data() + 64), start = le32(raw.data() + 68);
        require(count > 0 && count < 10000 && start == 68 + count * 4 && start <= raw.size(), "invalid package compression index");
        Reader r(raw.data(), raw.size(), start);
        p.data.assign(raw.begin(), raw.begin() + 64);
        uint8_t block[32769];
        for (uint32_t i = 0; i < count; ++i) {
            check_cancel();
            require(le32(raw.data() + 68 + 4 * i) == r.position, "incorrect compressed block offset");
            uint32_t size = r.u32();
            require(size > 0 && size <= (1u << 20), "oversized compressed block");
            const uint8_t *in = r.take(size);
            z_stream z{};
            require(inflateInit(&z) == Z_OK, "zlib init failed");
            z.next_in = const_cast<Bytef *>(in);
            z.avail_in = size;
            z.next_out = block;
            z.avail_out = sizeof block;
            int status = inflate(&z, Z_FINISH);
            size_t produced = sizeof block - z.avail_out;
            bool whole = status == Z_STREAM_END && z.avail_in == 0;
            inflateEnd(&z);
            require(whole && produced <= 32768, "invalid compressed package block");
            require(p.data.size() + produced <= MAX_PACKAGE, "package exceeds 256 MiB");
            p.data.insert(p.data.end(), block, block + produced);
        }
        require(r.position == raw.size(), "trailing compressed package bytes");
    }
    const uint8_t *h = p.data.data();
    uint32_t nc = le32(h + 12), no = le32(h + 16), ec = le32(h + 20), eo = le32(h + 24), ic = le32(h + 28), io = le32(h + 32);
    require(nc > 0 && nc <= 1000000 && ec > 0 && ec <= 1000000 && ic > 0 && ic <= 1000000, "package table count exceeds bound");
    require(64 < no && no < io && io < eo && eo < p.data.size(), "invalid package table offsets");
    Reader r(p.data.data(), p.data.size(), no);
    p.names.reserve(nc);
    for (uint32_t i = 0; i < nc; ++i) { p.names.push_back(r.string()); r.take(8); }
    require(r.position == io, "name table size mismatch");
    auto fname = [&]() {
        int32_t index = r.compact();
        uint32_t number = r.u32();
        require(index >= 0 && (size_t)index < p.names.size() && number <= 1000000, "invalid package name reference");
        return number ? p.names[(size_t)index] + std::to_string(number - 1) : p.names[(size_t)index];
    };
    std::vector<std::string> imports;
    for (uint32_t i = 0; i < ic; ++i) {
        fname(); fname();   /* class package, class */
        r.i32();            /* outer */
        imports.push_back(fname());
    }
    require(r.position == eo, "import table size mismatch");
    for (uint32_t i = 0; i < ec; ++i) {
        Export e;
        e.class_ref = r.compact();
        r.compact();        /* parent */
        int32_t outer = r.i32();
        r.i32();
        e.name = fname();
        r.take(8);          /* flags */
        int32_t size = r.compact();
        int32_t offset = size ? r.compact() : 0;
        r.take(4);
        require(e.class_ref >= -(int32_t)ic && e.class_ref <= (int32_t)ec && outer >= -(int32_t)ic && outer <= (int32_t)ec,
                "invalid export object reference");
        require(size >= 0 && (uint32_t)size <= no && (!size || (offset >= 64 && (uint32_t)offset < no && (uint64_t)offset + size <= no)),
                "export outside serialized payload");
        e.size = (uint32_t)size;
        e.offset = (uint32_t)offset;
        p.exports.push_back(std::move(e));
    }
    for (auto &e : p.exports)
        e.cls = e.class_ref < 0 ? imports[(size_t)(-e.class_ref - 1)] : e.class_ref ? p.exports[(size_t)e.class_ref - 1].name : "Class";
    return p;
}

struct Mip { uint32_t width, height, bytes, offset; };
struct Texture { std::string name; int format = -1; std::vector<Mip> mips; };

Texture texture(const Package &p, const Export &e)
{
    Reader r(p.data.data() + e.offset, e.size);
    require(r.u32() == 4 && r.u32() == 3, "unsupported texture property serialization");
    Texture t;
    t.name = e.name;
    int i = 0;
    for (; i < 4096; ++i) {
        int32_t index = r.compact();
        uint32_t number = r.u32();
        require(index >= 0 && (size_t)index < p.names.size() && number == 0, "invalid texture property name");
        const std::string &name = p.names[(size_t)index];
        if (name == "None") break;
        uint8_t info = *r.take(1);
        int kind = info & 15, code = info >> 4 & 7;
        require(kind >= 1 && kind <= 15, "invalid texture property type");
        if (kind == 10) {
            int32_t structure = r.compact();
            uint32_t extra = r.u32();
            require(structure >= 0 && (size_t)structure < p.names.size() && extra == 0, "invalid property structure");
        }
        static const uint32_t fixed[5] = { 1, 2, 4, 12, 16 };
        uint32_t size;
        if (code < 5) size = fixed[code];
        else if (code == 5) size = *r.take(1);
        else if (code == 6) { const uint8_t *s = r.take(2); size = (uint32_t)s[0] | (uint32_t)s[1] << 8; }
        else size = r.u32();
        int32_t element = (info & 128) && kind != 3 ? r.compact() : 0;
        const uint8_t *value = r.take(size);
        if (name == "Format" && element == 0) {
            require(kind == 1 || kind == 2, "invalid texture pixel format property");
            t.format = 0;
            for (uint32_t k = 0; k < size && k < 4; ++k) t.format |= value[k] << (8 * k);
        }
    }
    require(i < 4096, "texture property count exceeds bound");
    uint32_t marker = r.u32(), version = r.u32();
    require(marker == 4 && (version == 0 || version == 1), "unsupported texture bulk serialization");
    if (version) r.u64();
    int32_t count = r.compact();
    require(count >= 0 && count <= 16, "texture mip count exceeds bound");
    for (int32_t m = 0; m < count; ++m) {
        uint32_t mark = r.u32(), v = r.u32();
        require(mark == 4 && v <= 2, "unsupported texture mip serialization");
        if (v == 1) { r.take(12); r.take((size_t)r.compact()); }
        r.take(12);
        int32_t size = r.compact();
        require(size >= 0 && size <= (64 << 20), "oversized texture mip");
        Mip mip;
        mip.offset = e.offset + (uint32_t)r.position;
        mip.bytes = (uint32_t)size;
        r.take((size_t)size);
        mip.width = r.u32();
        mip.height = r.u32();
        const uint8_t *bits = r.take(2);
        require(mip.width > 0 && mip.width <= 16384 && mip.height > 0 && mip.height <= 16384 && bits[0] < 15 && bits[1] < 15 &&
                1u << bits[0] == mip.width && 1u << bits[1] == mip.height, "invalid mip dimensions");
        t.mips.push_back(mip);
    }
    require(r.position == r.size, "unparsed texture data");
    return t;
}

uint32_t fourcc_of(int format)
{
    switch (format) {
    case 3: return 0x31545844u;               /* DXT1 */
    case 7: return 0x33545844u;               /* DXT3 */
    case 8: case 12: return 0x35545844u;      /* DXT5, DXT5N */
    default: return 0;
    }
}

/* ------------------------------------------------------------------ the bulk catalog (BulkContent/Catalog.bdc) */
struct Item { const Mapped *file; uint32_t offset, size; };

struct Bulk {
    std::vector<std::unique_ptr<Mapped>> files;
    std::unordered_map<std::string, std::vector<Item>> items;

    explicit Bulk(const Game &game)
    {
        Source catalog_file = game.find("Content/BulkContent/Catalog.bdc");
        require((bool)catalog_file, "Content/BulkContent/Catalog.bdc is missing");
        Bytes catalog = game.read(catalog_file, 64u << 20);
        Reader r(catalog.data(), catalog.size());
        require(*r.take(1) == 0, "non-PC bulk catalog");
        r.take(12);
        int32_t count = r.compact();
        require(count > 0 && count <= 1024, "bulk catalog file count exceeds bound");
        for (int32_t i = 0; i < count; ++i) {
            r.take(8);
            std::string name = r.string();
            require(!name.empty() && name.find('/') == std::string::npos && name.find('\\') == std::string::npos &&
                    lower(name).size() > 4 && lower(name).compare(lower(name).size() - 4, 4, ".blk") == 0, "invalid bulk file name");
            Source blk = game.find("Content/BulkContent/" + name);
            if (!blk) throw std::runtime_error("Content/BulkContent/" + name + " is missing");
            std::unique_ptr<Mapped> file = game.map(blk);
            int32_t records = r.compact();
            require(records >= 0 && records <= 1000000, "bulk record count exceeds bound");
            for (int32_t k = 0; k < records; ++k) {
                std::string object = r.string();
                r.string();   /* package */
                uint32_t zero = r.u32(), offset = r.u32(), size = r.u32(), duplicate = r.u32();
                r.u32();      /* index */
                require(zero == 0 && size == duplicate && offset <= file->size && size <= file->size - offset, "invalid bulk range");
                items[object].push_back(Item{ file.get(), offset, size });
            }
            files.push_back(std::move(file));
        }
        require(r.position == r.size, "trailing bulk catalog bytes");
    }

    /* The mip chains of a texture: its embedded levels, completed from every bulk record of its name whose size
     * ends on a level boundary and agrees with the embedded levels; duplicates by content once. */
    std::vector<std::vector<Bytes>> chains(const Package &p, const Texture &t, uint32_t block) const
    {
        std::vector<uint32_t> sizes;
        std::vector<const uint8_t *> embedded;
        for (const Mip &m : t.mips) {
            uint32_t size = ((m.width + 3) / 4) * ((m.height + 3) / 4) * block;
            require(m.bytes == 0 || m.bytes == size, "mip storage does not match BCn dimensions");
            sizes.push_back(size);
            embedded.push_back(m.bytes ? p.data.data() + m.offset : nullptr);
        }
        std::vector<std::vector<Bytes>> out;
        size_t missing = 0;
        for (size_t i = 0; i < embedded.size(); ++i) if (!embedded[i]) missing = i + 1;
        if (!missing) {
            std::vector<Bytes> levels;
            for (size_t i = 0; i < embedded.size(); ++i) levels.emplace_back(embedded[i], embedded[i] + sizes[i]);
            out.push_back(std::move(levels));
            return out;
        }
        uint64_t required = 0;
        for (size_t i = 0; i < missing; ++i) required += sizes[i];
        auto found = items.find(t.name);
        if (found == items.end()) return out;
        std::unordered_set<uint64_t> seen;
        tvtx_crc_tables crc{};
        Bytes scratch;
        for (const Item &item : found->second) {
            uint64_t prefix = 0;
            bool boundary = false;
            for (uint32_t s : sizes) { prefix += s; if (prefix == item.size) boundary = true; }
            /* CachedBulkDataSize may include platform padding: exact level boundaries decide. */
            if (item.size < required || !boundary) continue;
            const uint8_t *raw = item.file->bytes(item.offset, item.size, scratch);
            std::vector<Bytes> levels;
            uint64_t at = 0;
            bool mismatch = false, broken = false;
            for (size_t i = 0; i < sizes.size(); ++i) {
                if (at + sizes[i] <= item.size) {
                    if (embedded[i] && memcmp(embedded[i], raw + at, sizes[i])) mismatch = true;
                    levels.emplace_back(raw + at, raw + at + sizes[i]);
                } else if (embedded[i]) {
                    levels.emplace_back(embedded[i], embedded[i] + sizes[i]);
                } else {
                    broken = true;   /* an external texture missing a lower level */
                    break;
                }
                at += sizes[i];
            }
            if (mismatch || broken) continue;
            uint64_t digest = 0, length = 0;
            for (const Bytes &l : levels) { digest = digest * 0x100000001B3ull ^ tvtx_key(&crc, l.data(), l.size()); length += l.size(); }
            if (seen.insert(digest ^ length * 0x9E3779B185EBCA87ull).second) out.push_back(std::move(levels));
        }
        return out;
    }
};

Bytes dds(const Texture &t, int format, const std::vector<Bytes> &levels)
{
    Bytes out(128, 0);
    auto set = [&](size_t at, uint32_t v) { memcpy(out.data() + at, &v, 4); };
    out[0] = 'D'; out[1] = 'D'; out[2] = 'S'; out[3] = ' ';
    set(4, 124); set(8, 0xa1007); set(12, t.mips[0].height); set(16, t.mips[0].width);
    set(20, (uint32_t)levels[0].size()); set(24, 0); set(28, (uint32_t)levels.size());
    set(76, 32); set(80, 4); set(84, fourcc_of(format)); set(108, 0x401008);
    for (const Bytes &l : levels) out.insert(out.end(), l.begin(), l.end());
    return out;
}

/* ------------------------------------------------------------------ output */
struct Output {
    int fd = -1;
    uint64_t at = sizeof(tvtx_header);
    explicit Output(const std::string &path)
    {
        fd = open(path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0644);
        if (fd < 0) throw std::runtime_error(std::string("cannot create texture pack: ") + strerror(errno));
        /* Android's emulated storage supports OFD record locks: a second converter cannot share the file. */
        struct flock lock {};
        lock.l_type = F_WRLCK;
        lock.l_whence = SEEK_SET;
        int result = fcntl(fd, F_OFD_SETLK, &lock);
        const char *operation = "lock";
        if (!result) { operation = "reset"; result = ftruncate(fd, 0); }
        if (result) {
            int reason = errno;
            close(fd);
            fd = -1;
            throw std::runtime_error(std::string("cannot ") + operation + " texture pack: " + strerror(reason));
        }
    }
    ~Output() { if (fd >= 0) close(fd); }
    void write(const void *data, size_t size, uint64_t offset)
    {
        const uint8_t *p = (const uint8_t *)data;
        while (size) {
            ssize_t n = pwrite(fd, p, size, (off_t)offset);
            if (n < 0 && errno == EINTR) continue;
            require(n > 0, "cannot write the texture pack (storage full?)");
            p += n; size -= (size_t)n; offset += (uint64_t)n;
        }
    }
    uint64_t append(const void *data, size_t size) { uint64_t offset = at; write(data, size, at); at += size; return offset; }
};

bool read_header(const std::string &path, tvtx_header &h, uint64_t &size)
{
    int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) return false;
    struct stat st;
    bool ok = !fstat(fd, &st) && st.st_size >= (off_t)sizeof h && pread(fd, &h, sizeof h, 0) == (ssize_t)sizeof h;
    close(fd);
    if (!ok) return false;
    size = (uint64_t)st.st_size;
    return h.magic == TVTX_MAGIC && h.version == TVTX_VERSION && (h.family == TVTX_ETC2 || h.family == TVTX_ASTC) &&
           h.count > 0 && h.count <= 2000000 && h.index_offset >= sizeof h && h.index_offset <= size &&
           h.payload_bytes == h.index_offset - sizeof h && (uint64_t)h.count * sizeof(utx_entry) == size - h.index_offset;
}

const char *no_maps(const Game &game)
{
    return game.archive ? "the game archive has no BioShock packages in Content/Maps"
                        : "Content/Maps has no BioShock packages (the conversion needs the game folder)";
}

std::vector<Source> texture_sources(const Game &game)
{
    auto sources = game.list("Content/Maps", ".bsm");
    require(!sources.empty(), no_maps(game));
    for (const char *folder : {"Content/System", "Builds/Release"}) {
        auto more = game.list(folder, ".u");
        sources.insert(sources.end(), more.begin(), more.end());
    }
    return sources;
}

std::vector<int64_t> scan(const std::string &folder, const std::string &cache, const std::string &index, int fd)
{
    Game game(folder, index, fd);
    const auto sources = texture_sources(game);
    Bulk bulk(game);
    int64_t textures = 0, bytes = 0, formats = 0;
    for (const Source &source : sources) {
        try {
            Package p = package(game, source);
            for (const Export &e : p.exports) {
                if (e.cls != "Texture") continue;
                const Texture t = texture(p, e);
                const uint32_t fourcc = fourcc_of(t.format);
                if (!fourcc || t.mips.empty()) continue;
                formats |= INT64_C(1) << t.format;
                ++textures;
                for (const Mip &mip : t.mips) {
                    const uint64_t expected = utx_level_size(fourcc, mip.width, mip.height);
                    if (mip.bytes ? mip.bytes != expected : !bulk.items.count(t.name))
                        throw std::runtime_error("invalid or missing mip for " + t.name);
                    bytes += (int64_t)expected;
                }
            }
        } catch (const std::exception &e) {
            throw std::runtime_error(source.name + ": " + e.what());
        }
    }
    tvtx_header h{};
    uint64_t size = 0;
    bool converted = read_header(pack_path(cache), h, size);
    return { textures, bytes, converted ? (int64_t)h.family : 0, converted ? (int64_t)h.count : 0,
             converted ? (int64_t)size : 0, formats };
}

/* One unique chain for the encoders: the DDS the codec reads, its levels back to back after the header. */
struct Job {
    uint32_t fourcc;
    std::vector<Mip> mips;
    std::vector<uint32_t> sizes;
    Bytes blob;
};

int convert(const std::string &folder, const std::string &cache, int family, int threads, const std::string &archive_index, int fd)
{
    require(family == TVTX_ETC2 || family == TVTX_ASTC, "unknown texture format");
    cancelled = false;
    done = bytes_done = 0;
    total = CORPUS_TEXTURES;
    bytes_total = CORPUS_BYTES;
    set_message("");
    Game game(folder, archive_index, fd);
    /* The maps, Content/System/AssetCatalog.u and the engine's script packages (the .U files of Builds/Release: the weapons
     * in hand, plasmids, ammo, fonts, the cursor and the HUD masks; the PC cooker missed them until 2026-11-21). */
    const auto sources = texture_sources(game);
    Bulk bulk(game);
    /* An archive's sidecar folder has only the files the launcher unpacked: Content may be missing there. */
    std::string pack_folder = child(cache, "Content");
    make_folders(pack_folder);
    std::string final_path = pack_path(cache), temporary = final_path + ".part";
    {
        /* The new pack is written beside an older one, which goes only when the new one is complete. */
        struct statvfs fs;
        uint64_t need = (family == TVTX_ASTC ? ASTC_PACK_BYTES : ASTC_PACK_BYTES * 17 / 20) + (256ull << 20);
        if (!statvfs(pack_folder.c_str(), &fs) && (uint64_t)fs.f_bavail * fs.f_frsize < need)
            throw std::runtime_error("not enough free space for the texture pack: " + std::to_string(need >> 20) + " MiB needed");
    }
    /* The codec reads it when a thread makes its encoder: astcenc's medium preset, as the PC pack. */
    if (!getenv("VOID_TEXCONV_ASTC_QUALITY")) setenv("VOID_TEXCONV_ASTC_QUALITY", "60", 1);
    Output output(temporary);
    std::mutex queue_lock, write_lock;
    std::condition_variable ready, room;
    std::deque<std::unique_ptr<Job>> queue;
    size_t queued_bytes = 0;
    bool finished = false;
    std::atomic<bool> broken{false};
    struct Indexed { utx_entry entry; uint64_t suffix; };
    std::unordered_map<uint64_t, Indexed> index;       /* key -> its first entry and the digest of its lower chain */
    std::unordered_set<uint64_t> conflicts;
    int64_t unique_chains = 0, raw_total = 0;
    uint32_t unsupported = 0, without_source = 0;
    tvtx_crc_tables writer_crc{};

    auto store = [&](const Job &job, const uint8_t *encoded, uint64_t encoded_size, uint32_t gl_format) {
        std::lock_guard<std::mutex> guard(write_lock);
        const uint8_t *raw = job.blob.data() + 128;
        uint64_t raw_bytes = job.blob.size() - 128;
        uint64_t raw_at = output.append(raw, (size_t)raw_bytes);
        uint64_t data_at = output.append(encoded, (size_t)encoded_size);
        uint64_t raw_left = raw_bytes, data_left = encoded_size;
        uint16_t dxt = (uint16_t)((job.fourcc >> 24) - '0');
        std::vector<uint64_t> keys(job.sizes.size());
        for (size_t l = 0, at = 0; l < job.sizes.size(); at += job.sizes[l], ++l)
            keys[l] = tvtx_key(&writer_crc, raw + at, job.sizes[l]);
        for (size_t l = 0; l < job.sizes.size(); ++l) {
            const Mip &m = job.mips[l];
            uint64_t key = keys[l] ^ ((((uint64_t)job.fourcc << 32) ^ ((uint64_t)m.width << 16) ^ m.height) * 0x9E3779B185EBCA87ull);
            uint64_t converted = tvtx_level_bytes(gl_format, m.width, m.height);
            /* The digest of this level's chain down to the smallest one: a key whose lower levels differ in another
             * chain keeps a one-level entry (r10_pack.c looks levels up one by one). */
            uint64_t suffix = 0;
            for (size_t k = l; k < keys.size(); ++k) suffix = suffix * 0x100000001B3ull ^ keys[k];
            utx_entry e{ key, raw_at, data_at, (uint32_t)raw_left, (uint32_t)data_left, gl_format, (uint16_t)m.width,
                         (uint16_t)m.height, (uint16_t)(job.sizes.size() - l), dxt, 0 };
            auto found = index.find(key);
            if (found == index.end()) {
                index.emplace(key, Indexed{ e, suffix });
            } else if (found->second.suffix != suffix && !conflicts.count(key)) {
                utx_entry &one = found->second.entry;
                one.levels = 1;
                one.raw_bytes = (uint32_t)utx_level_size(job.fourcc, one.width, one.height);
                one.data_bytes = (uint32_t)tvtx_level_bytes(one.gl_format, one.width, one.height);
                conflicts.insert(key);
            }
            raw_at += job.sizes[l]; raw_left -= job.sizes[l];
            data_at += converted; data_left -= converted;
        }
        require(data_left == 0 && raw_left == 0, "encoded mip chain size mismatch");
        ++unique_chains;
        raw_total += (int64_t)raw_bytes;
    };

    auto worker = [&]() {
        for (;;) {
            std::unique_ptr<Job> job;
            {
                std::unique_lock<std::mutex> guard(queue_lock);
                ready.wait(guard, [&] { return !queue.empty() || finished || broken || cancelled; });
                if (queue.empty() || broken || cancelled) { room.notify_all(); return; }
                job = std::move(queue.front());
                queue.pop_front();
                queued_bytes -= job->blob.size();
            }
            room.notify_all();
            try {
                uint8_t *encoded = nullptr;
                uint64_t encoded_size = 0;
                uint32_t gl_format = 0, flags = 0;
                require(texconv_convert_dds(job->blob.data(), job->blob.size(), family, &encoded, &encoded_size, &gl_format, &flags) == 0,
                        "texture encoder failed");
                std::unique_ptr<uint8_t, decltype(&texconv_free)> owner(encoded, texconv_free);
                store(*job, encoded, encoded_size, gl_format);
            } catch (const std::exception &e) {
                if (!broken.exchange(true)) set_message(e.what());
                ready.notify_all();
                room.notify_all();
                return;
            }
        }
    };
    int count = std::max(1, std::min(threads, 8));
    std::vector<std::thread> pool;
    for (int i = 0; i < count; ++i) pool.emplace_back(worker);
    auto stop = [&]() {
        { std::lock_guard<std::mutex> guard(queue_lock); finished = true; }
        ready.notify_all();
        for (auto &t : pool) if (t.joinable()) t.join();
    };
    try {
        std::unordered_set<uint64_t> seen;
        tvtx_crc_tables crc{};
        int64_t textures = 0;
        for (const Source &source : sources) {
            check_cancel();
            require(!broken, "texture conversion failed");
            const std::string &name = source.name;
            Package p;
            try { p = package(game, source); }
            catch (const std::exception &e) { throw std::runtime_error(name + ": " + e.what()); }
            for (const Export &e : p.exports) {
                if (e.cls != "Texture") continue;
                check_cancel();
                require(!broken, "texture conversion failed");
                Texture t;
                try { t = texture(p, e); }
                catch (const std::exception &ex) { throw std::runtime_error(name + ", " + e.name + ": " + ex.what()); }
                if (++textures > total) total = textures;
                uint32_t fourcc = fourcc_of(t.format);
                if (!fourcc || t.mips.empty()) { ++unsupported; done = textures; continue; }
                uint32_t block = fourcc == 0x31545844u ? 8 : 16;
                std::vector<std::vector<Bytes>> chains = bulk.chains(p, t, block);
                if (chains.empty()) ++without_source;
                for (auto &levels : chains) {
                    std::unique_ptr<Job> job(new Job);
                    job->fourcc = fourcc;
                    job->mips = t.mips;
                    for (const Bytes &l : levels) job->sizes.push_back((uint32_t)l.size());
                    job->blob = dds(t, t.format, levels);
                    levels.clear();
                    uint64_t digest = tvtx_key(&crc, job->blob.data(), job->blob.size()) ^ job->blob.size() * 0x9E3779B185EBCA87ull;
                    if (!seen.insert(digest).second) continue;
                    size_t bytes = job->blob.size();
                    bytes_done += (int64_t)(bytes - 128);
                    if (bytes_done > bytes_total) bytes_total = bytes_done.load();
                    std::unique_lock<std::mutex> guard(queue_lock);
                    /* At most 96 MiB of chains wait for the encoders. */
                    while (queued_bytes >= (96u << 20) && !broken && !cancelled)
                        room.wait_for(guard, std::chrono::milliseconds(200));
                    if (broken || cancelled) break;
                    queued_bytes += bytes;
                    queue.push_back(std::move(job));
                    ready.notify_one();
                }
                done = textures;
            }
        }
        stop();
        check_cancel();
        require(!broken, "texture conversion failed");
        require(!index.empty(), "no DXT textures found");
        std::vector<utx_entry> entries;
        entries.reserve(index.size());
        for (const auto &item : index) entries.push_back(item.second.entry);
        std::sort(entries.begin(), entries.end(), [](const utx_entry &a, const utx_entry &b) { return a.key < b.key; });
        tvtx_header h{};
        h.magic = TVTX_MAGIC;
        h.version = TVTX_VERSION;
        h.family = (uint32_t)family;
        h.count = (uint32_t)entries.size();
        h.index_offset = output.at;
        h.payload_bytes = output.at - sizeof h;
        h.source_bytes = (uint64_t)raw_total;
        h.sources = (uint32_t)unique_chains;
        h.skipped = unsupported + without_source;
        output.append(entries.data(), entries.size() * sizeof(utx_entry));
        output.write(&h, sizeof h, 0);
        require(fsync(output.fd) == 0, "cannot flush the texture pack");
        require(rename(temporary.c_str(), final_path.c_str()) == 0, "cannot commit the texture pack");
        close(output.fd);
        output.fd = -1;
        total = done.load();
        bytes_total = bytes_done.load();
        set_message("textures " + std::to_string(done.load()) + ", unique chains " + std::to_string(unique_chains) + ", level keys " +
                    std::to_string(entries.size()) + " (" + std::to_string(conflicts.size()) + " one-level), not DXT " +
                    std::to_string(unsupported) + ", without bulk data " + std::to_string(without_source));
        return 0;
    } catch (...) {
        bool user = cancelled.load();
        cancelled = true;   /* the encoders stop at their next job */
        stop();
        cancelled = user;
        unlink(temporary.c_str());
        throw;
    }
}

int remove_pack(const std::string &cache)
{
    std::string path = pack_path(cache);
    unlink((path + ".part").c_str());
    return unlink(path.c_str()) == 0 || errno == ENOENT ? 0 : -1;
}
}  // namespace

#ifdef TEXCONV_CLI
int main(int argc, char **argv)
{
    if (argc < 3) {
        fprintf(stderr, "usage: %s <game> [--cache <folder>] [--archive <zip> --index <zmi>] scan | convert <family 1 ETC2, 2 ASTC> "
                        "<threads> | undo\n", argv[0]);
        return 2;
    }
    std::string game = argv[1], cache = argv[1], index;
    int fd = -1, at = 2;
    for (; at + 1 < argc && !strncmp(argv[at], "--", 2); at += 2) {
        if (!strcmp(argv[at], "--cache")) cache = argv[at + 1];
        else if (!strcmp(argv[at], "--index")) index = argv[at + 1];
        else if (!strcmp(argv[at], "--archive")) {
            fd = open(argv[at + 1], O_RDONLY | O_CLOEXEC);
            if (fd < 0) { fprintf(stderr, "FAIL cannot open %s: %s\n", argv[at + 1], strerror(errno)); return 1; }
        } else { fprintf(stderr, "unknown option %s\n", argv[at]); return 2; }
    }
    if (at >= argc) return 2;
    argv += at - 1;
    argc -= at - 1;
    try {
        if (!strcmp(argv[1], "undo")) return remove_pack(cache) ? 1 : 0;
        if (!strcmp(argv[1], "scan")) {
            auto v = scan(game, cache, index, fd);
            printf("textures %lld bytes %lld converted %lld entries %lld size %lld formats 0x%llx\n",
                   (long long)v[0], (long long)v[1], (long long)v[2], (long long)v[3], (long long)v[4], (long long)v[5]);
            return 0;
        }
        if (!strcmp(argv[1], "convert") && argc >= 4) {
            std::atomic<bool> stop{false};
            std::thread progress([&] {
                while (!stop) {
                    fprintf(stderr, "progress %lld/%lld textures, %lld/%lld bytes\n", (long long)done.load(), (long long)total.load(),
                            (long long)bytes_done.load(), (long long)bytes_total.load());
                    for (int i = 0; i < 50 && !stop; ++i) usleep(100000);
                }
            });
            int result = -1;
            try { result = convert(game, cache, atoi(argv[2]), atoi(argv[3]), index, fd); } catch (...) { stop = true; progress.join(); throw; }
            stop = true;
            progress.join();
            std::lock_guard<std::mutex> guard(message_lock);
            printf("PASS %s\n", message.c_str());
            return result;
        }
    } catch (const std::exception &e) {
        fprintf(stderr, "FAIL %s\n", e.what());
        return 1;
    }
    return 2;
}
#else
namespace {
std::string java_string(JNIEnv *env, jstring value)
{
    require(value != nullptr, "missing game path");
    const char *s = env->GetStringUTFChars(value, nullptr);
    require(s != nullptr, "cannot read game path");
    std::string out(s);
    env->ReleaseStringUTFChars(value, s);
    return out;
}

/* An optional path: "" (no archive index) or the fallback (the cache root is the game folder). */
std::string java_string(JNIEnv *env, jstring value, const std::string &fallback)
{
    return value ? java_string(env, value) : fallback;
}
}

/* game: the game folder (an archive's sidecar folder); cache: the root of the pack; index and fd: the stored ZIP the
 * launcher plays in place (null and -1 for a folder), the descriptor open until the call returns. */
extern "C" JNIEXPORT jlongArray JNICALL Java_org_bioshock_port_TextureConverter_nativeScan(JNIEnv *env, jclass, jstring game,
                                                                                           jstring cache, jstring index, jint fd)
{
    try {
        std::string folder = java_string(env, game);
        auto values = scan(folder, java_string(env, cache, folder), java_string(env, index, std::string()), fd);
        jlongArray out = env->NewLongArray((jsize)values.size());
        if (out) env->SetLongArrayRegion(out, 0, (jsize)values.size(), (const jlong *)values.data());
        return out;
    } catch (const std::exception &e) { set_message(e.what()); return nullptr; }
}
extern "C" JNIEXPORT jint JNICALL Java_org_bioshock_port_TextureConverter_nativeConvert(JNIEnv *env, jclass, jstring game, jstring cache,
                                                                                       jint family, jint threads, jstring index, jint fd)
{
    try {
        std::string folder = java_string(env, game);
        return convert(folder, java_string(env, cache, folder), family, threads, java_string(env, index, std::string()), fd);
    } catch (const std::exception &e) {
        std::lock_guard<std::mutex> guard(message_lock);
        if (message.empty() || cancelled) message = e.what();
        return -1;
    }
}
extern "C" JNIEXPORT jint JNICALL Java_org_bioshock_port_TextureConverter_nativeUndo(JNIEnv *env, jclass, jstring game, jstring cache)
{
    try {
        std::string folder = java_string(env, game);
        return remove_pack(java_string(env, cache, folder));
    } catch (const std::exception &e) { set_message(e.what()); return -1; }
}
extern "C" JNIEXPORT void JNICALL Java_org_bioshock_port_TextureConverter_nativeCancel(JNIEnv *, jclass) { cancelled = true; }
extern "C" JNIEXPORT jlongArray JNICALL Java_org_bioshock_port_TextureConverter_nativeProgress(JNIEnv *env, jclass)
{
    jlong values[] = { done, total, bytes_done, bytes_total };
    jlongArray out = env->NewLongArray(4);
    if (out) env->SetLongArrayRegion(out, 0, 4, values);
    return out;
}
extern "C" JNIEXPORT jstring JNICALL Java_org_bioshock_port_TextureConverter_nativeMessage(JNIEnv *env, jclass)
{
    std::lock_guard<std::mutex> guard(message_lock);
    return env->NewStringUTF(message.c_str());
}
#endif

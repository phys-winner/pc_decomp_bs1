/* BioShock PC SaveGames container, as written by guest 109211B0.
 * The menu (10921910) opens every .bsb and reads the header and two strings.
 * Its reader's EOF error path can fault before the menu can drop the entry. */
#define _FILE_OFFSET_BITS 64
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <zlib.h>

enum {
    SAVE_HEADER_SIZE = 0x2028,
    PACKAGE_HEADER_SIZE = 64,
    PACKAGE_BLOCK_SIZE = 0x8000,
    PACKAGE_COMPRESSED_MAX = 0x10000,
};

typedef struct {
    FILE *stream;
    uint64_t remaining;
} save_reader;

static uint32_t save_u32(const unsigned char *bytes)
{
    return (uint32_t)bytes[0] | ((uint32_t)bytes[1] << 8) |
        ((uint32_t)bytes[2] << 16) | ((uint32_t)bytes[3] << 24);
}

static uint32_t save_png_u32(const unsigned char *bytes)
{
    return (uint32_t)bytes[3] | ((uint32_t)bytes[2] << 8) |
        ((uint32_t)bytes[1] << 16) | ((uint32_t)bytes[0] << 24);
}

static int save_read(save_reader *reader, void *buffer, size_t size)
{
    if (size > reader->remaining || fread(buffer, 1, size, reader->stream) != size) return 0;
    reader->remaining -= size;
    return 1;
}

static int save_thumbnail(save_reader *reader, uint32_t size)
{
    if (!size) return 1; /* The serializer allows saves without a screenshot. */
    if (size > reader->remaining) return 0;
    save_reader png = {reader->stream, size};
    unsigned char signature[8], chunk[8], buffer[4096];
    if (!save_read(&png, signature, sizeof signature) || memcmp(signature, "\x89PNG\r\n\x1a\n", 8)) return 0;
    int first = 1;
    while (save_read(&png, chunk, sizeof chunk)) {
        uint32_t length = save_png_u32(chunk);
        if ((uint64_t)length + 4 > png.remaining) return 0;
        if (first && (length != 13 || memcmp(chunk + 4, "IHDR", 4))) return 0;
        first = 0;
        uLong checksum = crc32(0, chunk + 4, 4);
        for (uint32_t left = length; left;) {
            unsigned count = left < sizeof buffer ? left : sizeof buffer;
            if (!save_read(&png, buffer, count)) return 0;
            checksum = crc32(checksum, buffer, count);
            left -= count;
        }
        unsigned char crc[4];
        if (!save_read(&png, crc, sizeof crc) || save_png_u32(crc) != checksum) return 0;
        if (!memcmp(chunk + 4, "IEND", 4)) {
            if (length || png.remaining) return 0;
            reader->remaining -= size;
            return 1;
        }
    }
    return 0;
}

/* FCompactIndex, guest 1093DE20. Save list counts, byte sizes and the v141
 * UTF-16 string lengths emitted by the writer are nonnegative. */
static int save_compact(save_reader *reader, uint32_t *result)
{
    unsigned char byte;
    if (!save_read(reader, &byte, 1) || (byte & 0x80)) return 0;
    uint32_t value = byte & 0x3f;
    unsigned continuation = byte & 0x40;
    for (unsigned shift = 6; continuation && shift <= 27; shift += 7) {
        if (!save_read(reader, &byte, 1) || (shift == 27 && byte > 0x0f)) return 0;
        value |= (uint32_t)(byte & 0x7f) << shift;
        continuation = byte & 0x80;
    }
    *result = value;
    return 1;
}

static int save_string(save_reader *reader, uint32_t limit, int filename)
{
    unsigned char text[260 * 2];
    uint32_t length;
    if (!save_compact(reader, &length) || length > limit) return 0;
    if (!length) return !filename;
    if (!save_read(reader, text, length * 2) || text[length * 2 - 2] || text[length * 2 - 1]) return 0;
    if (filename && ((length == 2 && !memcmp(text, ".\0", 2)) ||
                    (length == 3 && !memcmp(text, ".\0.\0", 4)))) return 0;
    for (uint32_t i = 0; i + 1 < length; ++i) {
        unsigned char low = text[i * 2], high = text[i * 2 + 1];
        if (!high && (!low || (filename && (low == '/' || low == '\\' || low == ':')))) return 0;
    }
    return !filename || length > 1;
}

static int save_package(save_reader *reader, uint32_t size, z_stream *inflater, int *read_error)
{
    if (size > reader->remaining || size <= PACKAGE_HEADER_SIZE) return 0;
    save_reader package = {reader->stream, size};
    unsigned char header[PACKAGE_HEADER_SIZE];
    if (!save_read(&package, header, sizeof header) || save_u32(header) != 0x9e2a83c1u) return 0;

    uint32_t flags = save_u32(header + 8);
    if (flags == 1) {
        /* Uncompressed PC packages have table offsets in the physical file. */
        if (save_u32(header + 16) >= size || save_u32(header + 24) >= size ||
            save_u32(header + 32) >= size) return 0;
        if (fseeko(package.stream, (off_t)package.remaining, SEEK_CUR)) { *read_error = errno; return 0; }
    } else {
        /* The save writer stores sequential size-prefixed, independent zlib
         * streams. Verify their checksums as well as their physical extent. */
        unsigned char compressed[PACKAGE_COMPRESSED_MAX], plain[PACKAGE_BLOCK_SIZE];
        uint64_t unpacked = PACKAGE_HEADER_SIZE;
        while (package.remaining) {
            unsigned char length_bytes[4];
            if (!save_read(&package, length_bytes, sizeof length_bytes)) return 0;
            uint32_t length = save_u32(length_bytes);
            if (!length || length > sizeof compressed || !save_read(&package, compressed, length)) return 0;
            if (inflateReset(inflater) != Z_OK) { *read_error = EIO; return 0; }
            inflater->next_in = compressed;
            inflater->avail_in = length;
            inflater->next_out = plain;
            inflater->avail_out = sizeof plain;
            int status = inflate(inflater, Z_FINISH);
            if (status == Z_MEM_ERROR) *read_error = ENOMEM;
            if (status != Z_STREAM_END || inflater->avail_in || !inflater->total_out) return 0;
            unpacked += inflater->total_out;
        }
        if (save_u32(header + 16) >= unpacked || save_u32(header + 24) >= unpacked ||
            save_u32(header + 32) >= unpacked) return 0;
    }
    reader->remaining -= size;
    return 1;
}

/* Stream starts at byte zero. NULL means the recognized container is intact.
 * The caller checks ferror() separately to distinguish a failed host read. */
const char *void_bioshock_save_damage(FILE *stream, uint64_t size, int *read_error)
{
    *read_error = 0;
    unsigned char header[24];
    save_reader reader = {stream, size};
    if (size < SAVE_HEADER_SIZE || !save_read(&reader, header, sizeof header)) return "truncated header";
    if (memcmp(header, "RGMH", 4) || save_u32(header + 4) != 1 || save_u32(header + 8) != SAVE_HEADER_SIZE)
        return "invalid header";
    uint64_t metadata = (uint64_t)SAVE_HEADER_SIZE + save_u32(header + 20);
    if (metadata >= size) return "truncated thumbnail or metadata";
    if (fseeko(stream, (off_t)SAVE_HEADER_SIZE, SEEK_SET)) { *read_error = errno; return "cannot seek to thumbnail"; }
    reader.remaining = size - SAVE_HEADER_SIZE;
    if (!save_thumbnail(&reader, save_u32(header + 20))) return "invalid thumbnail";

    /* 10921910 copies these strings into a 0x154-byte record at +0 and +0x54. */
    if (!save_string(&reader, 0x54 / 2, 1) || !save_string(&reader, (0x154 - 0x54) / 2, 0))
        return "invalid save names";
    uint32_t packages;
    if (!save_compact(&reader, &packages) || !packages || packages > reader.remaining / (PACKAGE_HEADER_SIZE + 2))
        return "invalid package count";
    for (uint32_t i = 0; i < packages; ++i)
        if (!save_string(&reader, 260, 1)) return "invalid package name";

    z_stream inflater = {0};
    if (inflateInit(&inflater) != Z_OK) { *read_error = ENOMEM; return "cannot initialize package reader"; }
    const char *damage = NULL;
    for (uint32_t i = 0; i < packages; ++i) {
        uint32_t length;
        if (!save_compact(&reader, &length) || !save_package(&reader, length, &inflater, read_error)) {
            damage = "truncated or invalid package";
            break;
        }
    }
    inflateEnd(&inflater);
    if (!damage && reader.remaining) damage = "extra data after packages";
    return damage;
}

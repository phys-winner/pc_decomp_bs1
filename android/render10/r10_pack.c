/* The launcher's converted texture pack for the native Direct3D 10 renderer.
 *
 * <game>/Content/port-textures.tvt (android/host/texture_cache.c) holds every BC1-3 mip level of the game's
 * packages and bulk files with its ASTC 4x4 or ETC2 copy, keyed by the level's own blocks (UTX3 v2, written by
 * the launcher's texture card through android/texconv/bioshock_tex.cpp or by tools/bioshock_textures.py on a PC).
 * A BC texture that the backend cannot sample (most mobile drivers have no BC, PowerVR's OpenGL ES has BC1
 * only) is created in the pack's GPU format, and every whole level the game writes is looked up: a hit uploads
 * the converted blocks.  The game thread decodes nothing, and the GPU stores a quarter (BC2/3) or an eighth
 * (BC1) of what decoded RGBA8 takes.
 *
 * Whatever the pack cannot follow - a level it lacks, a partial write, a copy it cannot express - first moves
 * the texture to the ordinary decoded storage: the levels it held are rewritten from the pack's copy of their
 * original blocks.  The picture never depends on the pack; only the speed does.  A texture that left the pack
 * stays decoded but keeps the original blocks of its levels, so copies between it and converted textures move
 * one level (decoded from the original blocks, or the pack's converted copy of them) and restore nobody: on the
 * RMX3371 restoring both sides of such copies spread the decoded storage through the texture pool, 48 restores
 * after four levels and 1006 after fifteen.
 *
 * D3DDrv10 (FD3DTexture2D10, Bioshock.exe 0x1099C700 / 0x1099CAA0 / 0x1099CCA0) keeps a pool of textures per
 * size and format (0x109ABA80).  A pooled texture is filled level by level with UpdateSubresource; streaming
 * creates a larger one, copies the lower levels from the old one with CopySubresourceRegion and writes the new
 * top levels.  Both stay converted: every level is its own key, and converted copies take their source's
 * blocks.  The uploaded bytes are the packages' own blocks (0x109B3F40 swaps bytes of RGBA8 formats only). */
#include "r10.h"
#include "../texconv/ut3tx.h"
#include <stdlib.h>

/* android/host/texture_cache.c */
int void_texture_cache_family(void);
const utx_entry *void_texture_cache_find(uint32_t format, uint32_t w, uint32_t h, uint32_t levels, const uint8_t *data,
                                         uint32_t pitch);
const uint8_t *void_texture_cache_raw(const utx_entry *e);
const uint8_t *void_texture_cache_data(const utx_entry *e);
void void_texture_cache_done(const uint8_t *p, size_t bytes);   /* read and copied: the pages may leave the mapping */

static struct {
    uint64_t textures, hits, misses, partial, copies, level_copies, restores, restored, converted_kb, decoded_kb, events;
} st;

static void pack_log(const char *what, const r10_resource *r, uint32_t sub)
{
    /* The first events, then at powers of two: a long session keeps a short log. */
    if (++st.events > 24 && (st.events & (st.events - 1))) return;
    R10_LOG("pack: %s %ux%u %s sub %u; textures %llu, hits %llu (%llu KiB converted for %llu KiB as RGBA8), "
            "misses %llu, partial writes %llu, copies %llu (%llu by level), restores %llu (%llu levels rewritten)",
            what, r->desc.width, r->desc.height, r->fmt ? r->fmt->name : "?", sub, (unsigned long long)st.textures,
            (unsigned long long)st.hits, (unsigned long long)st.converted_kb, (unsigned long long)st.decoded_kb,
            (unsigned long long)st.misses, (unsigned long long)st.partial, (unsigned long long)st.copies,
            (unsigned long long)st.level_copies, (unsigned long long)st.restores, (unsigned long long)st.restored);
}

int r10_pack_family(void)
{
    int family = void_texture_cache_family();
    return r10_dev && r10_dev->be && (r10_dev->be->pack_formats & family) ? family : 0;
}

static uint32_t dxt_fourcc(int bc) { return bc == 1 ? 0x31545844u : bc == 2 ? 0x33545844u : 0x35545844u; }

void r10_pack_prepare(r10_device *d, r10_resource *r)
{
    r->pack_eligible = r->converted = 0;
    r->pack_raw = NULL;
    const r10_format *f = r->fmt;
    if (!f || f->kind != R10_FMT_BC || f->bc < 1 || f->bc > 3 || !d->be->resource_store) return;
    if (d->be->samples && d->be->samples(f)) return;          /* the device samples it as it is */
    int family = void_texture_cache_family();
    if (!family || !(d->be->pack_formats & family)) return;
    /* What the game loads from its packages: sampled 2D textures (cubes and arrays included) it fills itself. */
    if (r->desc.dim != R10_DIM_TEXTURE2D || r->desc.samples > 1 || r->desc.cpu ||
        (r->desc.usage != D3D10_USAGE_DEFAULT && r->desc.usage != D3D10_USAGE_IMMUTABLE) ||
        (r->desc.bind & ~(uint32_t)D3D10_BIND_SHADER_RESOURCE) || (r->desc.misc & D3D10_RESOURCE_MISC_GENERATE_MIPS))
        return;
    r->pack_raw = (const uint8_t **)calloc(r->subresources ? r->subresources : 1, sizeof *r->pack_raw);
    if (!r->pack_raw) return;
    r->pack_eligible = r->converted = 1;
    if (!st.textures++) R10_LOG("pack: %s textures of the game are stored as %s from the converted pack", f->name,
                                family == R10_PACK_ASTC ? "ASTC 4x4" : "ETC2 RGBA8");
}

void r10_pack_release(r10_resource *r)
{
    free(r->pack_raw);
    r->pack_raw = NULL;
    r->pack_eligible = r->converted = 0;
}

/* Back to the decoded storage, the levels it held rewritten from the pack's copy of their blocks; `skip` is a
 * level the caller overwrites whole right after (UINT32_MAX: none). */
static void pack_restore(r10_device *d, r10_resource *r, uint32_t skip, const char *why)
{
    if (!r->converted) return;
    r->converted = 0;
    ++st.restores;
    if (!d->be->resource_store(d, r)) R10_LOG("pack: %ux%u %s: the decoded storage could not be created", r->desc.width,
                                              r->desc.height, r->fmt->name);
    for (uint32_t s = 0; s < r->subresources; ++s) {
        const uint8_t *raw = r->pack_raw ? r->pack_raw[s] : NULL;
        if (!raw || s == skip) continue;
        uint32_t w, h, depth;
        r10_subresource_size(r, s, &w, &h, &depth);
        uint32_t pitch = r10_format_row_pitch(r->fmt, w);
        d->be->resource_update(d, r, s, 0, 0, 0, w, h, 1, raw, pitch, pitch * r10_format_rows(r->fmt, h));
        void_texture_cache_done(raw, (size_t)pitch * r10_format_rows(r->fmt, h));
        ++st.restored;
    }
    /* A texture that left the pack stays decoded (a pooled one too): it is never half converted.  It keeps the
     * original blocks of its levels (r10_texture_write and r10_texture_copy follow what later lands in them). */
    pack_log(why, r, skip);
}

/* The pack's entry of a whole level given as the game writes it, NULL when the pack lacks it. */
static const utx_entry *pack_find(const r10_resource *r, uint32_t w, uint32_t h, const uint8_t *data, uint32_t row, int log_miss)
{
    const r10_format *f = r->fmt;
    uint32_t bw = (w + 3) / 4, bh = (h + 3) / 4, tight = bw * f->bytes;
    /* The game's row pitch of levels narrower than a block is not the block row (0x109B3F40); the key covers
     * the level's blocks back to back. */
    const uint8_t *level = data;
    uint8_t *packed = NULL;
    if (bh > 1 && row != tight) {
        packed = (uint8_t *)malloc((size_t)tight * bh);
        if (!packed) return NULL;
        for (uint32_t i = 0; i < bh; ++i) memcpy(packed + (size_t)i * tight, data + (size_t)i * row, tight);
        level = packed;
    }
    const utx_entry *e = void_texture_cache_find(dxt_fourcc(f->bc), w, h, 1, level, tight);
    if (e) void_texture_cache_done(void_texture_cache_raw(e), (size_t)tight * bh);   /* the lookup's comparison read it */
    if (!e && log_miss) {
        /* The first misses name the level's key and blocks: tools/bioshock_textures.py --find-key places them. */
        static unsigned logged;
        static tvtx_crc_tables crc;
        if (logged++ < 32) {
            uint64_t key = utx_key(&crc, dxt_fourcc(f->bc), w, h, level, (size_t)tight * bh);
            R10_LOG("pack: level not in the pack: key %016llx %ux%u %s, blocks %02x%02x%02x%02x%02x%02x%02x%02x", (unsigned long long)key,
                    w, h, f->name, level[0], level[1], level[2], level[3], level[4], level[5], level[6], level[7]);
        }
    }
    free(packed);
    return e;
}

/* A whole level: the pack's blocks for it, 1 when uploaded. */
static int pack_upload(r10_device *d, r10_resource *r, uint32_t sub, uint32_t w, uint32_t h, const utx_entry *e);
static int pack_write(r10_device *d, r10_resource *r, uint32_t sub, uint32_t w, uint32_t h, const uint8_t *data, uint32_t row)
{
    const utx_entry *e = pack_find(r, w, h, data, row, 1);
    if (!e || !pack_upload(d, r, sub, w, h, e)) return 0;
    ++st.hits;
    st.converted_kb += ((uint64_t)((w + 3) / 4) * ((h + 3) / 4) * R10_PACK_BLOCK_BYTES) >> 10;
    st.decoded_kb += ((uint64_t)w * h * 4) >> 10;
    if (st.hits <= 4 || !(st.hits & (st.hits - 1))) pack_log("hit", r, sub);
    return 1;
}

/* The converted blocks of a pack entry into a level of a converted texture, which then holds that level's original. */
static int pack_upload(r10_device *d, r10_resource *r, uint32_t sub, uint32_t w, uint32_t h, const utx_entry *e)
{
    uint32_t bw = (w + 3) / 4, bh = (h + 3) / 4;
    const uint8_t *blocks = void_texture_cache_data(e);
    uint32_t count = bw * bh, bytes = count * R10_PACK_BLOCK_BYTES;
    uint8_t *expanded = NULL;
    if (e->gl_format == TVTX_GL_ETC2_RGB8) {
        /* Opaque ETC2 levels: the RGBA8 EAC storage takes them with an alpha block of base 255, multiplier 0. */
        static const uint8_t opaque[8] = { 0xFF, 0, 0, 0, 0, 0, 0, 0 };
        expanded = (uint8_t *)malloc(bytes);
        if (!expanded) return 0;
        for (uint32_t i = 0; i < count; ++i) {
            memcpy(expanded + 16 * (size_t)i, opaque, 8);
            memcpy(expanded + 16 * (size_t)i + 8, blocks + 8 * (size_t)i, 8);
        }
        blocks = expanded;
    } else if (e->gl_format != TVTX_GL_ASTC_4x4 && e->gl_format != TVTX_GL_ETC2_RGBA8) {
        return 0;
    }
    d->be->resource_update(d, r, sub, 0, 0, 0, w, h, 1, blocks, bw * R10_PACK_BLOCK_BYTES, bytes);
    void_texture_cache_done(void_texture_cache_data(e), e->gl_format == TVTX_GL_ETC2_RGB8 ? (size_t)count * 8 : bytes);
    free(expanded);
    r->pack_raw[sub] = void_texture_cache_raw(e);
    return 1;
}

void r10_texture_write(r10_device *d, r10_resource *r, uint32_t sub, uint32_t x, uint32_t y, uint32_t z, uint32_t w,
                       uint32_t h, uint32_t depth, const void *data, uint32_t row_pitch, uint32_t slice_pitch)
{
    if (r->pack_raw && sub < r->subresources) {
        uint32_t lw, lh, ld;
        r10_subresource_size(r, sub, &lw, &lh, &ld);
        int whole = !x && !y && !z && w == lw && h == lh && depth == 1;
        if (r->converted) {
            if (whole && pack_write(d, r, sub, lw, lh, (const uint8_t *)data, row_pitch)) return;
            if (whole) ++st.misses; else ++st.partial;
            pack_restore(d, r, whole ? sub : UINT32_MAX, whole ? "miss" : "partial write");
            r->pack_raw[sub] = NULL;
        } else {
            /* A texture that left the pack: what the level holds now, when the pack has it. */
            const utx_entry *e = whole ? pack_find(r, lw, lh, (const uint8_t *)data, row_pitch, 0) : NULL;
            r->pack_raw[sub] = e ? void_texture_cache_raw(e) : NULL;
        }
    }
    d->be->resource_update(d, r, sub, x, y, z, w, h, depth, data, row_pitch, slice_pitch);
}

void r10_texture_copy(r10_device *d, r10_resource *dst, uint32_t dst_sub, uint32_t x, uint32_t y, uint32_t z,
                      r10_resource *src, uint32_t src_sub, const uint32_t *box)
{
    if ((src->pack_raw || dst->pack_raw) && dst_sub < dst->subresources && src_sub < src->subresources &&
        dst->desc.dim != R10_DIM_BUFFER && src->desc.dim != R10_DIM_BUFFER) {
        uint32_t sw, sh, sd, dw, dh, dd;
        r10_subresource_size(src, src_sub, &sw, &sh, &sd);
        r10_subresource_size(dst, dst_sub, &dw, &dh, &dd);
        int whole = !x && !y && !z && sw == dw && sh == dh &&
                    (!box || (!box[0] && !box[1] && !box[2] && box[3] == sw && box[4] == sh && box[5] <= 1));
        /* The original blocks of the copied level, and whether the destination takes them as they are. */
        const uint8_t *raw = whole && src->pack_raw ? src->pack_raw[src_sub] : NULL;
        int same = raw && src->fmt && dst->fmt && src->fmt->kind == R10_FMT_BC && dst->fmt->kind == R10_FMT_BC &&
                   src->fmt->bc == dst->fmt->bc && dst->desc.usage != D3D10_USAGE_STAGING;
        if (src->converted && dst->converted && whole) {
            /* Both hold the pack's blocks: the copy keeps them, and the level remembers its source's original. */
            dst->pack_raw[dst_sub] = src->pack_raw[src_sub];
            ++st.copies;
        } else if (src->converted && !dst->converted && same) {
            /* Into a decoded texture: the level's original blocks, decoded there; the source stays converted. */
            uint32_t pitch = r10_format_row_pitch(dst->fmt, dw);
            d->be->resource_update(d, dst, dst_sub, 0, 0, 0, dw, dh, 1, raw, pitch, pitch * r10_format_rows(dst->fmt, dh));
            void_texture_cache_done(raw, (size_t)pitch * r10_format_rows(dst->fmt, dh));
            if (dst->pack_raw) dst->pack_raw[dst_sub] = raw;
            ++st.level_copies;
            return;
        } else if (!src->converted && dst->converted && same) {
            /* From a decoded texture whose level the pack has: the pack's converted copy of it. */
            const utx_entry *e = pack_find(dst, dw, dh, raw, r10_format_row_pitch(dst->fmt, dw), 0);
            if (e && pack_upload(d, dst, dst_sub, dw, dh, e)) {
                ++st.level_copies;
                return;
            }
            pack_restore(d, dst, dst_sub, "copy into a converted texture");
        } else {
            pack_restore(d, src, UINT32_MAX, "copy from a converted texture");
            pack_restore(d, dst, whole ? dst_sub : UINT32_MAX, "copy into a converted texture");
        }
        /* A decoded destination that was in the pack follows what the copy puts into the level. */
        if (dst->pack_raw && !dst->converted) dst->pack_raw[dst_sub] = whole && src->pack_raw ? src->pack_raw[src_sub] : NULL;
    }
    d->be->copy(d, dst, dst_sub, x, y, z, src, src_sub, box);
}

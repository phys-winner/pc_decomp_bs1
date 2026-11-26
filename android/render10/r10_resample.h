/* Resampling of 8-bit four-channel texels for D3DX10LoadTextureFromTexture (r10_reflect.c).
 *
 * The research camera (Bioshock.exe 0x109B75A0) loads the subject's rectangle of the tonemapped back buffer
 * (2944x1840 on the Lenovo TB373FU) into a 256x256 R8G8B8A8 staging texture with D3DX10_FILTER_LINEAR and maps it.
 * D3DX scales between the boxes; a copy of the whole source into the smaller texture wrote past its memory.
 *
 * Every axis is separable: a destination texel takes the source texels it covers, weighted by coverage, when the
 * axis shrinks (the photo: up to 11 source texels a texel), the two nearest bilinearly when it grows, and the one
 * under its centre for D3DX10_FILTER_POINT / NONE. The helpers are pure (tested on the host by
 * tests/test_photo_resample.py). */
#ifndef R10_RESAMPLE_H
#define R10_RESAMPLE_H

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* D3DX10_FILTER_FLAG: the low byte selects the filter (NONE 1, POINT 2, LINEAR 3, TRIANGLE 4, BOX 5). */
static inline int r10_resample_point(uint32_t filter)
{
    uint32_t kind = filter & 0xFFu;
    return filter != 0xFFFFFFFFu && (kind == 1u || kind == 2u);
}

typedef struct r10_resample_axis {
    uint32_t taps;       /* weights per destination texel */
    uint32_t *first;     /* first source texel of each destination texel */
    float *weight;       /* taps per destination texel, summing to 1 */
} r10_resample_axis;

static inline void r10_resample_axis_free(r10_resample_axis *a)
{
    free(a->first);
    free(a->weight);
    a->first = NULL;
    a->weight = NULL;
}

/* 1 when made: ns source texels onto nd destination texels. */
static inline int r10_resample_axis_make(r10_resample_axis *a, uint32_t ns, uint32_t nd, int point)
{
    memset(a, 0, sizeof *a);
    if (!ns || !nd) return 0;
    double scale = (double)ns / (double)nd;
    a->taps = point ? 1u : scale > 1.0 ? (uint32_t)scale + 2u : 2u;
    a->first = (uint32_t *)malloc((size_t)nd * sizeof *a->first);
    a->weight = (float *)calloc((size_t)nd * a->taps, sizeof *a->weight);
    if (!a->first || !a->weight) { r10_resample_axis_free(a); return 0; }
    for (uint32_t i = 0; i < nd; ++i) {
        float *w = a->weight + (size_t)i * a->taps;
        if (point) {
            uint32_t s = (uint32_t)(((double)i + 0.5) * scale);
            a->first[i] = s < ns ? s : ns - 1u;
            w[0] = 1.0f;
            continue;
        }
        if (scale > 1.0) {
            double lo = (double)i * scale, hi = lo + scale;
            if (hi > (double)ns) hi = (double)ns;
            uint32_t s0 = (uint32_t)lo;
            if (s0 >= ns) s0 = ns - 1u;
            a->first[i] = s0;
            double sum = 0.0;
            for (uint32_t k = 0; k < a->taps && s0 + k < ns; ++k) {
                double c0 = (double)(s0 + k), c1 = c0 + 1.0;
                double cover = (c1 < hi ? c1 : hi) - (c0 > lo ? c0 : lo);
                if (cover > 0.0) { w[k] = (float)cover; sum += cover; }
            }
            if (sum <= 0.0) { w[0] = 1.0f; continue; }
            for (uint32_t k = 0; k < a->taps; ++k) w[k] = (float)(w[k] / sum);
        } else {
            double c = ((double)i + 0.5) * scale - 0.5;
            if (c < 0.0) c = 0.0;
            uint32_t s0 = (uint32_t)c;
            if (s0 + 1u >= ns) {
                a->first[i] = ns - 1u;
                w[0] = 1.0f;
            } else {
                float f = (float)(c - (double)s0);
                a->first[i] = s0;
                w[0] = 1.0f - f;
                w[1] = f;
            }
        }
    }
    return 1;
}

/* The sw x sh texels at src (row pitch src_pitch) onto dw x dh texels at dst (row pitch dst_pitch). swap_rb:
 * the destination's byte order has R and B exchanged (R8G8B8A8 <-> B8G8R8A8); opaque_in: the source's fourth
 * byte is undefined (X8), read as 255; opaque_out: the destination's fourth byte is written 255. 1 when done. */
static inline int r10_resample_rgba8(const uint8_t *src, uint32_t sw, uint32_t sh, size_t src_pitch, uint8_t *dst,
                                     uint32_t dw, uint32_t dh, size_t dst_pitch, int point, int swap_rb, int opaque_in,
                                     int opaque_out)
{
    if (!src || !dst || !sw || !sh || !dw || !dh) return 0;
    r10_resample_axis ax, ay;
    if (!r10_resample_axis_make(&ax, sw, dw, point)) return 0;
    if (!r10_resample_axis_make(&ay, sh, dh, point)) { r10_resample_axis_free(&ax); return 0; }
    /* Rows first: every source row the destination reads, at the destination width. */
    float *rows = (float *)malloc((size_t)sh * dw * 4u * sizeof *rows);
    if (!rows) { r10_resample_axis_free(&ax); r10_resample_axis_free(&ay); return 0; }
    for (uint32_t y = 0; y < sh; ++y) {
        const uint8_t *line = src + (size_t)y * src_pitch;
        float *out = rows + (size_t)y * dw * 4u;
        for (uint32_t x = 0; x < dw; ++x) {
            const float *w = ax.weight + (size_t)x * ax.taps;
            uint32_t s0 = ax.first[x];
            float acc[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
            for (uint32_t k = 0; k < ax.taps && s0 + k < sw; ++k) {
                if (w[k] == 0.0f) continue;
                const uint8_t *p = line + (size_t)(s0 + k) * 4u;
                acc[0] += w[k] * p[0];
                acc[1] += w[k] * p[1];
                acc[2] += w[k] * p[2];
                acc[3] += w[k] * (opaque_in ? 255.0f : (float)p[3]);
            }
            memcpy(out + (size_t)x * 4u, acc, sizeof acc);
        }
    }
    for (uint32_t y = 0; y < dh; ++y) {
        const float *w = ay.weight + (size_t)y * ay.taps;
        uint32_t s0 = ay.first[y];
        uint8_t *line = dst + (size_t)y * dst_pitch;
        for (uint32_t x = 0; x < dw; ++x) {
            float acc[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
            for (uint32_t k = 0; k < ay.taps && s0 + k < sh; ++k) {
                if (w[k] == 0.0f) continue;
                const float *p = rows + ((size_t)(s0 + k) * dw + x) * 4u;
                for (int c = 0; c < 4; ++c) acc[c] += w[k] * p[c];
            }
            uint8_t texel[4];
            for (int c = 0; c < 4; ++c) {
                float v = acc[c] + 0.5f;
                texel[c] = (uint8_t)(v <= 0.0f ? 0u : v >= 255.0f ? 255u : (unsigned)v);
            }
            if (swap_rb) { uint8_t t = texel[0]; texel[0] = texel[2]; texel[2] = t; }
            if (opaque_out) texel[3] = 255u;
            memcpy(line + (size_t)x * 4u, texel, 4);
        }
    }
    free(rows);
    r10_resample_axis_free(&ax);
    r10_resample_axis_free(&ay);
    return 1;
}

/* D3D10_BOX (left, top, front, right, bottom, back) clipped to a w x h level; whole level when box is NULL.
 * 1 when the rectangle is not empty. */
static inline int r10_resample_rect(const uint32_t *box, uint32_t w, uint32_t h, uint32_t rect[4])
{
    uint32_t l = 0, t = 0, r = w, b = h;
    if (box) {
        l = box[0]; t = box[1]; r = box[3]; b = box[4];
        if (r > w) r = w;
        if (b > h) b = h;
    }
    rect[0] = l; rect[1] = t; rect[2] = r > l ? r - l : 0u; rect[3] = b > t ? b - t : 0u;
    return rect[2] && rect[3];
}

#endif

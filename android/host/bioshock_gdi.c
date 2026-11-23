#include "host.h"

typedef struct {
    uint32_t handle, data, width, pitch, bpp;
    int32_t height;
} gdi_bitmap;

static gdi_bitmap bitmaps[64];
static uint32_t next_bitmap = 0x710000u;

static gdi_bitmap *bitmap_new(uint32_t width, int32_t height, uint32_t bpp, const void *pixels, uint32_t bytes)
{
    uint32_t pitch = ((width * bpp + 31u) >> 5) * 4u;
    uint32_t rows = height < 0 ? (uint32_t)(-(int64_t)height) : (uint32_t)height;
    uint64_t size = (uint64_t)pitch * rows;
    if (!width || !rows || size > (256u << 20)) return NULL;
    for (unsigned i = 0; i < 64; ++i) {
        if (bitmaps[i].handle) continue;
        uint32_t data = void_galloc((uint32_t)size);
        if (!data) return NULL;
        if (pixels) memcpy(GPTR(data), pixels, (size_t)size);
        bitmaps[i] = (gdi_bitmap){ ++next_bitmap, data, width, pitch, bpp, height };
        return &bitmaps[i];
    }
    return NULL;
}

static gdi_bitmap *bitmap_get(uint32_t handle)
{
    for (unsigned i = 0; i < 64; ++i) if (bitmaps[i].handle == handle) return &bitmaps[i];
    return NULL;
}

WINAPI_EXPORT("gdi32.dll", CreateDIBitmap, 6)
{
    uint32_t header = ARG(1), init = ARG(2), pixels = ARG(3);
    if (!header || !void_vm_readable(header, 40)) return 0;
    int32_t width = (int32_t)MEM32(header + 4), height = (int32_t)MEM32(header + 8);
    uint32_t bpp = MEM16(header + 14);
    if (width <= 0 || !height || MEM16(header + 12) != 1 ||
        (bpp != 1 && bpp != 4 && bpp != 8 && bpp != 16 && bpp != 24 && bpp != 32)) return 0;
    uint32_t pitch = (((uint32_t)width * bpp + 31u) >> 5) * 4u;
    uint32_t rows = height < 0 ? (uint32_t)(-(int64_t)height) : (uint32_t)height;
    uint64_t size = (uint64_t)pitch * rows;
    if (size > (256u << 20) || ((init & 4u) && (!pixels || !void_vm_readable(pixels, (uint32_t)size)))) return 0;
    gdi_bitmap *bitmap = bitmap_new((uint32_t)width, height, bpp, (init & 4u) ? GPTR(pixels) : NULL, (uint32_t)size);
    return bitmap ? bitmap->handle : 0;
}

WINAPI_EXPORT("gdi32.dll", CreateDIBSection, 6)
{
    uint32_t info = ARG(1), out = ARG(3);
    if (out && void_vm_writable(out, 4)) MEM32(out) = 0;
    if (!info || !out || ARG(4) || !void_vm_readable(info, 40) || !void_vm_writable(out, 4)) return 0;
    int32_t width = (int32_t)MEM32(info + 4), height = (int32_t)MEM32(info + 8);
    uint32_t bpp = MEM16(info + 14);
    if (width <= 0 || !height || MEM16(info + 12) != 1 || bpp != 32) return 0;
    gdi_bitmap *bitmap = bitmap_new((uint32_t)width, height, bpp, NULL, 0);
    if (!bitmap) return 0;
    MEM32(out) = bitmap->data;
    return bitmap->handle;
}

WINAPI_EXPORT("gdi32.dll", GetObjectA, 3)
{
    gdi_bitmap *bitmap = bitmap_get(ARG(0));
    if (!bitmap || !ARG(2) || ARG(1) < 24 || !void_vm_writable(ARG(2), 24)) return 0;
    uint32_t out = ARG(2);
    memset(GPTR(out), 0, 24);
    MEM32(out + 4) = bitmap->width;
    MEM32(out + 8) = (uint32_t)bitmap->height;
    MEM32(out + 12) = bitmap->pitch;
    MEM16(out + 16) = 1;
    MEM16(out + 18) = (uint16_t)bitmap->bpp;
    MEM32(out + 20) = bitmap->data;
    return 24;
}

WINAPI_EXPORT("gdi32.dll", GetDIBits, 7)
{
    gdi_bitmap *bitmap = bitmap_get(ARG(1));
    uint32_t start = ARG(2), count = ARG(3), out = ARG(4), info = ARG(5);
    if (!bitmap || !info || !void_vm_writable(info, 40)) return 0;
    uint32_t rows = bitmap->height < 0 ? (uint32_t)(-(int64_t)bitmap->height) : (uint32_t)bitmap->height;
    if (start > rows || count > rows - start) count = rows - start;
    MEM32(info + 4) = (int32_t)bitmap->width;
    MEM32(info + 8) = bitmap->height;
    MEM16(info + 12) = 1;
    MEM16(info + 14) = (uint16_t)bitmap->bpp;
    MEM32(info + 20) = bitmap->pitch * rows;
    if (!out) return rows;
    if (!void_vm_writable(out, bitmap->pitch * count)) return 0;
    memcpy(GPTR(out), GPTR(bitmap->data + start * bitmap->pitch), bitmap->pitch * count);
    return count;
}

WINAPI_EXPORT("gdi32.dll", CreateCompatibleDC, 1) { return 0x720001u; }
WINAPI_EXPORT("gdi32.dll", DeleteDC, 1) { return 1; }
WINAPI_EXPORT("gdi32.dll", PatBlt, 6) { return 1; }
WINAPI_EXPORT("gdi32.dll", StretchBlt, 11) { return 1; }
WINAPI_EXPORT("gdi32.dll", GetPixel, 3) { return 0; }
WINAPI_EXPORT("gdi32.dll", SetPixel, 4) { return 0; }
WINAPI_EXPORT("gdi32.dll", CreateCompatibleBitmap, 3)
{
    if ((int32_t)ARG(1) <= 0 || (int32_t)ARG(2) <= 0) return 0;
    gdi_bitmap *bitmap = bitmap_new(ARG(1), (int32_t)ARG(2), 32, NULL, 0);
    return bitmap ? bitmap->handle : 0;
}
WINAPI_EXPORT("gdi32.dll", SetStretchBltMode, 2) { return 4; }
WINAPI_EXPORT("gdi32.dll", SetDeviceGammaRamp, 2) { return 1; }
WINAPI_EXPORT("gdi32.dll", CreateSolidBrush, 1) { return 0x730001u; }
WINAPI_EXPORT("gdi32.dll", CreateFontW, 14) { return 0x730002u; }
WINAPI_EXPORT("gdi32.dll", TextOutW, 5) { return 1; }
WINAPI_EXPORT("gdi32.dll", GetTextExtentPoint32A, 4)
{
    uint32_t count = void_gstrlen(ARG(1));
    if (ARG(2) > count) count = ARG(2);
    MEM32(ARG(3)) = count * 8u;
    MEM32(ARG(3) + 4) = 16u;
    return 1;
}

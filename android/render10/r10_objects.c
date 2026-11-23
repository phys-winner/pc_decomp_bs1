/* Direct3D 10 resources, views, state objects, shaders, input layouts and queries as guest COM objects. */
#include "r10.h"
#include "r10_shaderlib.h"
#include <errno.h>
#include <stdlib.h>
#include <strings.h>
#include <time.h>

/* ------------------------------------------------------------------ GUIDs */
#define GUID_D3D10(a) { (uint8_t)(a), (uint8_t)((a) >> 8), (uint8_t)((a) >> 16), (uint8_t)((a) >> 24), 0x2C, 0x34, 0x06, 0x41, \
                        0xA1, 0x9F, 0x4F, 0x27, 0x04, 0xF6, 0x89, 0xF0 }
static const uint8_t IID_IUnknown_[16] = { 0, 0, 0, 0, 0, 0, 0, 0, 0xC0, 0, 0, 0, 0, 0, 0, 0x46 };
static const uint8_t IID_DeviceChild[16] = GUID_D3D10(0x9B7E4C00u);
static const uint8_t IID_Resource[16] = GUID_D3D10(0x9B7E4C01u);
static const uint8_t IID_Buffer[16] = GUID_D3D10(0x9B7E4C02u);
static const uint8_t IID_Texture1D[16] = GUID_D3D10(0x9B7E4C03u);
static const uint8_t IID_Texture2D[16] = GUID_D3D10(0x9B7E4C04u);
static const uint8_t IID_Texture3D[16] = GUID_D3D10(0x9B7E4C05u);
static const uint8_t IID_SRV[16] = GUID_D3D10(0x9B7E4C07u);
static const uint8_t IID_RTV[16] = GUID_D3D10(0x9B7E4C08u);
static const uint8_t IID_DSV[16] = GUID_D3D10(0x9B7E4C09u);
static const uint8_t IID_VS[16] = GUID_D3D10(0x9B7E4C0Au);
static const uint8_t IID_Layout[16] = GUID_D3D10(0x9B7E4C0Bu);
static const uint8_t IID_Sampler[16] = GUID_D3D10(0x9B7E4C0Cu);
static const uint8_t IID_Async[16] = GUID_D3D10(0x9B7E4C0Du);
static const uint8_t IID_Query[16] = GUID_D3D10(0x9B7E4C0Eu);
static const uint8_t IID_View[16] = { 0x3F, 0xB0, 0x02, 0xC9, 0xA7, 0x60, 0xBA, 0x49, 0x99, 0x36, 0x2A, 0x3A, 0xB3, 0x7A, 0x7E, 0x33 };
static const uint8_t IID_GS[16] = { 0x88, 0xBE, 0x16, 0x63, 0xCD, 0x54, 0x40, 0x40, 0xAB, 0x44, 0x20, 0x46, 0x1B, 0xC8, 0x1F, 0x68 };
static const uint8_t IID_PS[16] = { 0x01, 0xB6, 0x68, 0x49, 0x00, 0x9D, 0xDE, 0x4C, 0x83, 0x46, 0x8E, 0x7F, 0x67, 0x58, 0x19, 0xB6 };
static const uint8_t IID_Blend[16] = { 0x19, 0x8D, 0xAD, 0xED, 0x35, 0x8A, 0x6D, 0x4D, 0x85, 0x66, 0x2E, 0xA2, 0x76, 0xCD, 0xE1, 0x61 };
static const uint8_t IID_Depth[16] = { 0xC8, 0x1C, 0x4B, 0x2B, 0xAD, 0xA4, 0xF8, 0x41, 0x83, 0x22, 0xCA, 0x86, 0xFC, 0x3E, 0xC6, 0x75 };
static const uint8_t IID_Raster[16] = { 0x92, 0x72, 0xA0, 0xA2, 0xAF, 0x89, 0x45, 0x43, 0xBE, 0x2E, 0xC5, 0x3D, 0x9F, 0xBB, 0x6E, 0x9F };
static const uint8_t IID_DXGISurface[16] = { 0x6C, 0xB5, 0xFC, 0xCA, 0xE3, 0x6A, 0x89, 0x48, 0xBF, 0x47, 0x9E, 0x23, 0xBB, 0xD2, 0x60, 0xEC };
static const uint8_t IID_DXGIResource[16] = { 0xB4, 0x3A, 0x5F, 0x03, 0x2E, 0x48, 0x50, 0x4E, 0xB4, 0x1F, 0x8A, 0x7F, 0x8B, 0xD8, 0x96, 0x0B };

static int r10_qi_accepts(r10_kind kind, uint32_t riid)
{
    if (void_guid_equal(riid, IID_IUnknown_) || void_guid_equal(riid, IID_DeviceChild)) return 1;
    switch (kind) {
    case R10_KIND_BUFFER: return void_guid_equal(riid, IID_Resource) || void_guid_equal(riid, IID_Buffer);
    case R10_KIND_TEXTURE1D: return void_guid_equal(riid, IID_Resource) || void_guid_equal(riid, IID_Texture1D);
    case R10_KIND_TEXTURE2D: return void_guid_equal(riid, IID_Resource) || void_guid_equal(riid, IID_Texture2D) ||
                                    void_guid_equal(riid, IID_DXGISurface) || void_guid_equal(riid, IID_DXGIResource);
    case R10_KIND_TEXTURE3D: return void_guid_equal(riid, IID_Resource) || void_guid_equal(riid, IID_Texture3D);
    case R10_KIND_SRV: return void_guid_equal(riid, IID_View) || void_guid_equal(riid, IID_SRV);
    case R10_KIND_RTV: return void_guid_equal(riid, IID_View) || void_guid_equal(riid, IID_RTV);
    case R10_KIND_DSV: return void_guid_equal(riid, IID_View) || void_guid_equal(riid, IID_DSV);
    case R10_KIND_BLEND: return void_guid_equal(riid, IID_Blend);
    case R10_KIND_DEPTH: return void_guid_equal(riid, IID_Depth);
    case R10_KIND_RASTER: return void_guid_equal(riid, IID_Raster);
    case R10_KIND_SAMPLER: return void_guid_equal(riid, IID_Sampler);
    case R10_KIND_VS: return void_guid_equal(riid, IID_VS);
    case R10_KIND_PS: return void_guid_equal(riid, IID_PS);
    case R10_KIND_GS: return void_guid_equal(riid, IID_GS);
    case R10_KIND_LAYOUT: return void_guid_equal(riid, IID_Layout);
    case R10_KIND_QUERY: return void_guid_equal(riid, IID_Async) || void_guid_equal(riid, IID_Query);
    default: return 0;
    }
}

/* ------------------------------------------------------------------ shared DeviceChild methods */
static r10_object *child(void) { return (r10_object *)void_com_host(THIS_); }

COM_METHOD(child, QueryInterface, 3)
{
    r10_object *o = child();
    uint32_t out = ARG(2);
    if (!out) return E_POINTER_;
    if (r10_qi_accepts(o->kind, ARG(1))) {
        void_com_addref(&o->com);
        MEM32(out) = THIS_;
        return 0;
    }
    char text[40];
    void_guid_format(ARG(1), text);
    static unsigned logged;
    if (logged++ < 32) R10_LOG("QueryInterface %s on kind %d: E_NOINTERFACE", text, o->kind);
    MEM32(out) = 0;
    return E_NOINTERFACE_;
}
COM_METHOD(child, AddRef, 1) { return void_com_addref(&child()->com); }
COM_METHOD(child, Release, 1) { return void_com_release(&child()->com); }
COM_METHOD(child, GetDevice, 2)
{
    (void)child();
    if (ARG(1)) { MEM32(ARG(1)) = r10_dev ? r10_dev->obj.com.guest : 0; if (r10_dev) void_com_addref(&r10_dev->obj.com); }
    return 0;
}

static r10_private **private_slot(r10_private **head, uint32_t guid)
{
    for (r10_private **p = head; *p; p = &(*p)->next)
        if (!memcmp((*p)->guid, GPTR(guid), 16)) return p;
    return NULL;
}

uint32_t r10_private_get(r10_private **head, uint32_t guid, uint32_t size_va, uint32_t data)
{
    if (!guid || !size_va) return E_INVALIDARG_;
    r10_private **slot = private_slot(head, guid);
    if (!slot) { MEM32(size_va) = 0; return DXGI_ERROR_NOT_FOUND_; }
    r10_private *p = *slot;
    if (p->size == 0xFFFFFFFFu) {
        if (!data) { MEM32(size_va) = 4; return 0; }
        if (MEM32(size_va) < 4) return DXGI_ERROR_INVALID_CALL_;
        MEM32(data) = p->data;
        if (p->data) r10_object_addref_guest(p->data);
        MEM32(size_va) = 4;
        return 0;
    }
    if (!data) { MEM32(size_va) = p->size; return 0; }
    if (MEM32(size_va) < p->size) { MEM32(size_va) = p->size; return DXGI_ERROR_INVALID_CALL_; }
    memcpy(GPTR(data), GPTR(p->data), p->size);
    MEM32(size_va) = p->size;
    return 0;
}

uint32_t r10_private_set(r10_private **head, uint32_t guid, uint32_t size, uint32_t data, int interface)
{
    if (!guid) return E_INVALIDARG_;
    r10_private **slot = private_slot(head, guid);
    if (slot) {
        r10_private *old = *slot;
        *slot = old->next;
        if (old->size == 0xFFFFFFFFu) { if (old->data) r10_object_release_guest(old->data); }
        else if (old->data) void_gfree(old->data);
        free(old);
    }
    if (interface ? !data : (!size || !data)) return 0;
    r10_private *p = (r10_private *)calloc(1, sizeof *p);
    memcpy(p->guid, GPTR(guid), 16);
    if (interface) { p->size = 0xFFFFFFFFu; p->data = data; r10_object_addref_guest(data); }
    else { p->size = size; p->data = void_galloc(size); memcpy(GPTR(p->data), GPTR(data), size); }
    p->next = *head;
    *head = p;
    return 0;
}

COM_METHOD(child, GetPrivateData, 4) { return r10_private_get(&child()->priv, ARG(1), ARG(2), ARG(3)); }
COM_METHOD(child, SetPrivateData, 4) { return r10_private_set(&child()->priv, ARG(1), ARG(2), ARG(3), 0); }
COM_METHOD(child, SetPrivateDataInterface, 3) { return r10_private_set(&child()->priv, ARG(1), 0, ARG(2), 1); }

#define CHILD_METHODS child_QueryInterface, child_AddRef, child_Release, child_GetDevice, child_GetPrivateData, \
                      child_SetPrivateData, child_SetPrivateDataInterface

static void private_free(r10_object *o)
{
    while (o->priv) {
        r10_private *p = o->priv;
        o->priv = p->next;
        if (p->size == 0xFFFFFFFFu) { if (p->data) r10_object_release_guest(p->data); }
        else if (p->data) void_gfree(p->data);
        free(p);
    }
}

/* ------------------------------------------------------------------ resources */
void r10_subresource_size(const r10_resource *r, uint32_t sub, uint32_t *w, uint32_t *h, uint32_t *depth)
{
    uint32_t mips = r->desc.mips ? r->desc.mips : 1, mip = sub % mips;
    uint32_t x = r->desc.width >> mip, y = r->desc.height >> mip, z = r->desc.depth >> mip;
    *w = x ? x : 1;
    *h = r->desc.dim >= R10_DIM_TEXTURE2D ? (y ? y : 1) : 1;
    *depth = r->desc.dim == R10_DIM_TEXTURE3D ? (z ? z : 1) : 1;
}

void r10_resource_layout(r10_resource *r)
{
    if (r->desc.dim == R10_DIM_BUFFER) { r->shadow_size = r->desc.width; return; }
    if (r->sub_offset) return;
    r->sub_offset = (uint32_t *)calloc(r->subresources, 4);
    r->sub_pitch = (uint32_t *)calloc(r->subresources, 4);
    r->sub_slice = (uint32_t *)calloc(r->subresources, 4);
    uint32_t offset = 0;
    for (uint32_t s = 0; s < r->subresources; ++s) {
        uint32_t w, h, d;
        r10_subresource_size(r, s, &w, &h, &d);
        uint32_t pitch = r10_format_row_pitch(r->fmt, w);
        pitch = (pitch + 3u) & ~3u;
        uint32_t slice = pitch * r10_format_rows(r->fmt, h);
        r->sub_offset[s] = offset;
        r->sub_pitch[s] = pitch;
        r->sub_slice[s] = slice;
        offset += (slice * d + 15u) & ~15u;
    }
    r->shadow_size = offset;
}

static void resource_destroy(void_com_object *base)
{
    r10_resource *r = (r10_resource *)base;
    if (r10_dev) {
        r10_unbind_resource(r10_dev, r);
        if (r10_dev->be) r10_dev->be->resource_destroy(r10_dev, r);
    }
    r10_pack_release(r);
    private_free(&r->obj);
    r10_shadow_free(r);
    free(r->sub_offset); free(r->sub_pitch); free(r->sub_slice);
    free(r->host_copy);
    free(r);
}

COM_METHOD(res, GetType, 2)
{
    r10_resource *r = (r10_resource *)void_com_host(THIS_);
    if (ARG(1)) MEM32(ARG(1)) = r->desc.dim;
    return 0;
}
COM_METHOD(res, SetEvictionPriority, 2) { (void)void_com_host(THIS_); return 0; }
COM_METHOD(res, GetEvictionPriority, 1) { (void)void_com_host(THIS_); return 0; }

COM_METHOD(buf, Map, 4)
{
    r10_resource *r = (r10_resource *)void_com_host(THIS_);
    uint32_t out = ARG(3);
    uint32_t pitch, slice;
    uint32_t va = r10_resource_map(r10_dev, r, 0, ARG(1), ARG(2), &pitch, &slice);
    if (out) MEM32(out) = va;
    return va ? 0 : (ARG(2) & D3D10_MAP_FLAG_DO_NOT_WAIT ? DXGI_ERROR_WAS_STILL_DRAWING_ : E_INVALIDARG_);
}
COM_METHOD(buf, Unmap, 1) { r10_resource_unmap(r10_dev, (r10_resource *)void_com_host(THIS_), 0); return 0; }
COM_METHOD(buf, GetDesc, 2)
{
    r10_resource *r = (r10_resource *)void_com_host(THIS_);
    uint32_t out = ARG(1);
    if (out) { MEM32(out) = r->desc.width; MEM32(out + 4) = r->desc.usage; MEM32(out + 8) = r->desc.bind;
               MEM32(out + 12) = r->desc.cpu; MEM32(out + 16) = r->desc.misc; }
    return 0;
}

COM_METHOD(tex1, Map, 5)
{
    r10_resource *r = (r10_resource *)void_com_host(THIS_);
    uint32_t pitch, slice, va = r10_resource_map(r10_dev, r, ARG(1), ARG(2), ARG(3), &pitch, &slice);
    if (ARG(4)) MEM32(ARG(4)) = va;
    return va ? 0 : E_INVALIDARG_;
}
COM_METHOD(tex, Unmap, 2) { r10_resource_unmap(r10_dev, (r10_resource *)void_com_host(THIS_), ARG(1)); return 0; }
COM_METHOD(tex2, Map, 5)
{
    r10_resource *r = (r10_resource *)void_com_host(THIS_);
    uint32_t pitch = 0, slice = 0, va = r10_resource_map(r10_dev, r, ARG(1), ARG(2), ARG(3), &pitch, &slice);
    if (ARG(4)) { MEM32(ARG(4)) = va; MEM32(ARG(4) + 4) = pitch; }
    return va ? 0 : (ARG(3) & D3D10_MAP_FLAG_DO_NOT_WAIT ? DXGI_ERROR_WAS_STILL_DRAWING_ : E_INVALIDARG_);
}
COM_METHOD(tex3, Map, 5)
{
    r10_resource *r = (r10_resource *)void_com_host(THIS_);
    uint32_t pitch = 0, slice = 0, va = r10_resource_map(r10_dev, r, ARG(1), ARG(2), ARG(3), &pitch, &slice);
    if (ARG(4)) { MEM32(ARG(4)) = va; MEM32(ARG(4) + 4) = pitch; MEM32(ARG(4) + 8) = slice; }
    return va ? 0 : E_INVALIDARG_;
}
COM_METHOD(tex1, GetDesc, 2)
{
    r10_resource *r = (r10_resource *)void_com_host(THIS_);
    uint32_t o = ARG(1);
    if (o) { MEM32(o) = r->desc.width; MEM32(o + 4) = r->desc.mips; MEM32(o + 8) = r->desc.layers; MEM32(o + 12) = r->desc.format;
             MEM32(o + 16) = r->desc.usage; MEM32(o + 20) = r->desc.bind; MEM32(o + 24) = r->desc.cpu; MEM32(o + 28) = r->desc.misc; }
    return 0;
}
COM_METHOD(tex2, GetDesc, 2)
{
    r10_resource *r = (r10_resource *)void_com_host(THIS_);
    uint32_t o = ARG(1);
    if (o) { MEM32(o) = r->desc.width; MEM32(o + 4) = r->desc.height; MEM32(o + 8) = r->desc.mips; MEM32(o + 12) = r->desc.layers;
             MEM32(o + 16) = r->desc.format; MEM32(o + 20) = r->desc.samples; MEM32(o + 24) = r->desc.quality;
             MEM32(o + 28) = r->desc.usage; MEM32(o + 32) = r->desc.bind; MEM32(o + 36) = r->desc.cpu; MEM32(o + 40) = r->desc.misc; }
    return 0;
}
COM_METHOD(tex3, GetDesc, 2)
{
    r10_resource *r = (r10_resource *)void_com_host(THIS_);
    uint32_t o = ARG(1);
    if (o) { MEM32(o) = r->desc.width; MEM32(o + 4) = r->desc.height; MEM32(o + 8) = r->desc.depth; MEM32(o + 12) = r->desc.mips;
             MEM32(o + 16) = r->desc.format; MEM32(o + 20) = r->desc.usage; MEM32(o + 24) = r->desc.bind; MEM32(o + 28) = r->desc.cpu;
             MEM32(o + 32) = r->desc.misc; }
    return 0;
}

#define RESOURCE_METHODS CHILD_METHODS, res_GetType, res_SetEvictionPriority, res_GetEvictionPriority
static const void_fn buffer_methods[] = { RESOURCE_METHODS, buf_Map, buf_Unmap, buf_GetDesc };
static const void_fn tex1_methods[] = { RESOURCE_METHODS, tex1_Map, tex_Unmap, tex1_GetDesc };
static const void_fn tex2_methods[] = { RESOURCE_METHODS, tex2_Map, tex_Unmap, tex2_GetDesc };
static const void_fn tex3_methods[] = { RESOURCE_METHODS, tex3_Map, tex_Unmap, tex3_GetDesc };
static void_com_class buffer_class = { "ID3D10Buffer", 13, buffer_methods, 0 };
static void_com_class tex1_class = { "ID3D10Texture1D", 13, tex1_methods, 0 };
static void_com_class tex2_class = { "ID3D10Texture2D", 13, tex2_methods, 0 };
static void_com_class tex3_class = { "ID3D10Texture3D", 13, tex3_methods, 0 };

uint32_t r10_resource_guest(r10_resource *r) { return r->obj.com.guest; }

static uint32_t resource_finish(r10_device *d, r10_resource *r, uint32_t init_va, uint32_t *out)
{
    r10_kind kind = r->desc.dim == R10_DIM_BUFFER ? R10_KIND_BUFFER : r->desc.dim == R10_DIM_TEXTURE1D ? R10_KIND_TEXTURE1D
                  : r->desc.dim == R10_DIM_TEXTURE2D ? R10_KIND_TEXTURE2D : R10_KIND_TEXTURE3D;
    void_com_class *cls = kind == R10_KIND_BUFFER ? &buffer_class : kind == R10_KIND_TEXTURE1D ? &tex1_class
                        : kind == R10_KIND_TEXTURE2D ? &tex2_class : &tex3_class;
    r->subresources = r->desc.dim == R10_DIM_BUFFER ? 1 : r->desc.mips * r->desc.layers;
    r10_resource_layout(r);
    if (r->desc.dim == R10_DIM_BUFFER && (r->desc.bind & D3D10_BIND_INDEX_BUFFER) && r->desc.usage != D3D10_USAGE_DYNAMIC) {
        r->host_copy = (uint8_t *)calloc(1, r->desc.width ? r->desc.width : 1);
        if (init_va && MEM32(init_va)) memcpy(r->host_copy, GPTR(MEM32(init_va)), r->desc.width);
    }
    if (r->desc.dim != R10_DIM_BUFFER) r10_pack_prepare(d, r);
    /* A converted texture takes its initial data level by level through the pack (r10_pack.c). */
    const uint32_t *init = init_va ? (const uint32_t *)GPTR(init_va) : NULL;
    if (!d->be->resource_create(d, r, r->converted ? NULL : init)) {
        R10_LOG("resource create failed: dim %u %ux%ux%u format %u (%s) usage %u bind %x", r->desc.dim, r->desc.width,
                r->desc.height, r->desc.depth, r->desc.format, r->fmt ? r->fmt->name : "?", r->desc.usage, r->desc.bind);
        r10_pack_release(r);
        free(r->host_copy); free(r->sub_offset); free(r->sub_pitch); free(r->sub_slice); free(r);
        if (out) *out = 0;
        return E_OUTOFMEMORY_;
    }
    if (r->converted && init) {
        for (uint32_t s = 0; s < r->subresources; ++s) {
            const uint32_t *sd = init + 3 * s;
            if (!sd[0]) continue;
            uint32_t w, h, depth;
            r10_subresource_size(r, s, &w, &h, &depth);
            r10_texture_write(d, r, s, 0, 0, 0, w, h, depth, GPTR(sd[0]), sd[1], sd[2]);
        }
    }
    r10_object_wrap(&r->obj, cls, kind);
    r->obj.com.destroy = resource_destroy;
    if (out) *out = r->obj.com.guest;
    /* The kinds of resources the game makes, once each (formats, usage, binding). */
    static uint32_t seen[256];
    static unsigned seen_count;
    uint32_t sig = r->desc.dim | r->desc.format << 4 | r->desc.usage << 12 | (r->desc.bind & 0x7F) << 14 | (r->desc.cpu >> 16) << 21 |
                   (r->desc.misc & 7) << 23 | (init_va ? 1u : 0u) << 26 | (r->desc.mips > 1) << 27;
    unsigned i = 0;
    while (i < seen_count && seen[i] != sig) ++i;
    if (i == seen_count && seen_count < 256) {
        seen[seen_count++] = sig;
        R10_LOG("RESOURCE dim %u %ux%ux%u mips %u layers %u %s usage %u bind %02X cpu %X misc %X data %d", r->desc.dim,
                r->desc.width, r->desc.height, r->desc.depth, r->desc.mips, r->desc.layers, r->fmt ? r->fmt->name : "buffer",
                r->desc.usage, r->desc.bind, r->desc.cpu, r->desc.misc, init_va != 0);
    }
    return 0;
}

uint32_t r10_create_buffer(r10_device *d, uint32_t desc_va, uint32_t init_va, uint32_t *out)
{
    if (!desc_va) return E_INVALIDARG_;
    r10_resource *r = (r10_resource *)calloc(1, sizeof *r);
    r->desc.dim = R10_DIM_BUFFER;
    r->desc.width = MEM32(desc_va);
    r->desc.height = r->desc.depth = r->desc.mips = r->desc.layers = r->desc.samples = 1;
    r->desc.usage = MEM32(desc_va + 4);
    r->desc.bind = MEM32(desc_va + 8);
    r->desc.cpu = MEM32(desc_va + 12);
    r->desc.misc = MEM32(desc_va + 16);
    if (!r->desc.width || (r->desc.usage == D3D10_USAGE_IMMUTABLE && !init_va)) { free(r); return E_INVALIDARG_; }
    if (!out) { free(r); return S_FALSE_; }
    return resource_finish(d, r, init_va, out);
}

static uint32_t full_mips(uint32_t w, uint32_t h, uint32_t d)
{
    uint32_t m = 1, s = w > h ? w : h;
    if (d > s) s = d;
    while (s > 1) { s >>= 1; ++m; }
    return m;
}

uint32_t r10_create_texture_internal(r10_device *d, const r10_resource_desc *desc, r10_resource **out)
{
    r10_resource *r = (r10_resource *)calloc(1, sizeof *r);
    r->desc = *desc;
    r->fmt = r10_format_get(r10_format_resource_typed(desc->format, desc->bind));
    if (!r->fmt) { free(r); return E_INVALIDARG_; }
    uint32_t guest = 0;
    uint32_t hr = resource_finish(d, r, 0, &guest);
    if (hr) return hr;
    *out = r;
    return 0;
}

uint32_t r10_create_texture(r10_device *d, int dim, uint32_t desc_va, uint32_t init_va, uint32_t *out)
{
    if (!desc_va) return E_INVALIDARG_;
    r10_resource *r = (r10_resource *)calloc(1, sizeof *r);
    r10_resource_desc *t = &r->desc;
    t->dim = (uint32_t)dim;
    t->height = t->depth = t->layers = t->samples = 1;
    if (dim == R10_DIM_TEXTURE1D) {
        t->width = MEM32(desc_va); t->mips = MEM32(desc_va + 4); t->layers = MEM32(desc_va + 8); t->format = MEM32(desc_va + 12);
        t->usage = MEM32(desc_va + 16); t->bind = MEM32(desc_va + 20); t->cpu = MEM32(desc_va + 24); t->misc = MEM32(desc_va + 28);
    } else if (dim == R10_DIM_TEXTURE2D) {
        t->width = MEM32(desc_va); t->height = MEM32(desc_va + 4); t->mips = MEM32(desc_va + 8); t->layers = MEM32(desc_va + 12);
        t->format = MEM32(desc_va + 16); t->samples = MEM32(desc_va + 20); t->quality = MEM32(desc_va + 24);
        t->usage = MEM32(desc_va + 28); t->bind = MEM32(desc_va + 32); t->cpu = MEM32(desc_va + 36); t->misc = MEM32(desc_va + 40);
    } else {
        t->width = MEM32(desc_va); t->height = MEM32(desc_va + 4); t->depth = MEM32(desc_va + 8); t->mips = MEM32(desc_va + 12);
        t->format = MEM32(desc_va + 16); t->usage = MEM32(desc_va + 20); t->bind = MEM32(desc_va + 24); t->cpu = MEM32(desc_va + 28);
        t->misc = MEM32(desc_va + 32);
    }
    if (!t->samples) t->samples = 1;
    if (!t->layers) t->layers = 1;
    if (!t->width || !t->height || !t->depth || t->width > 16384 || t->height > 16384 || t->depth > 2048) { free(r); return E_INVALIDARG_; }
    if (!t->mips) t->mips = full_mips(t->width, t->height, dim == R10_DIM_TEXTURE3D ? t->depth : 1);
    r->fmt = r10_format_get(r10_format_resource_typed(t->format, t->bind));
    if (!r->fmt) {
        R10_LOG("CreateTexture%dD: unsupported DXGI format %u", dim - 1, t->format);
        free(r);
        return E_INVALIDARG_;
    }
    if (!out) { free(r); return S_FALSE_; }
    return resource_finish(d, r, init_va, out);
}

/* Map/Unmap: the game writes and reads guest memory; the backend gets the data at Unmap (textures,
 * staging) or when a draw uses it (dynamic buffers, r10_device.c). */
uint64_t r10_shadow_bytes;
uint32_t r10_shadow_count;
static r10_resource *shadow_list;            /* the resources that have a shadow */
static uint32_t shadow_heaps[2];             /* [0] static vertex buffers' shadows, [1] the others */
static uint64_t shadow_released_bytes, shadow_refills, shadow_purged;
static uint32_t shadow_released_count;

/* The game's meshes: vertex buffers it creates dynamic, maps with NO_OVERWRITE and never discards. */
static int vertex_only(const r10_resource *r)
{
    return r->desc.dim == R10_DIM_BUFFER && r->desc.bind == D3D10_BIND_VERTEX_BUFFER;
}

static void shadow_unlink(r10_resource *r)
{
    if (r->shadow_prev) r->shadow_prev->shadow_next = r->shadow_next;
    else if (shadow_list == r) shadow_list = r->shadow_next;
    if (r->shadow_next) r->shadow_next->shadow_prev = r->shadow_prev;
    r->shadow_prev = r->shadow_next = NULL;
}

void r10_shadow_free(r10_resource *r)
{
    if (r->shadow_released) { shadow_released_bytes -= r->shadow_size; --shadow_released_count; r->shadow_released = 0; }
    if (!r->shadow) return;
    if (!r->shadow_heap) { void_gfree(r->shadow); r->shadow = 0; return; }   /* not counted (made elsewhere) */
    void_heap_free(r->shadow_heap, r->shadow);
    r->shadow = 0;
    r10_shadow_bytes -= r->shadow_size;
    --r10_shadow_count;
    shadow_unlink(r);
}

/* Static vertex buffers' shadows live in a heap of their own, so the pages their release frees are whole. */
static int ensure_shadow(r10_device *d, r10_resource *r, uint32_t map_type)
{
    if (r->shadow) return 1;
    r10_resource_layout(r);
    int k = vertex_only(r) ? 0 : 1;
    if (!shadow_heaps[k]) shadow_heaps[k] = void_heap_create(0, 0);
    uint32_t heap = shadow_heaps[k] ? shadow_heaps[k] : void_process_heap();
    r->shadow = void_heap_alloc(heap, r->shadow_size ? r->shadow_size : 16, 1);
    if (!r->shadow) return 0;
    r->shadow_heap = heap;
    r10_shadow_bytes += r->shadow_size;
    ++r10_shadow_count;
    r->shadow_prev = NULL;
    r->shadow_next = shadow_list;
    if (shadow_list) shadow_list->shadow_prev = r;
    shadow_list = r;
    if (r->shadow_released) {
        /* Released only where the backend reads the buffer's GPU copy back, which holds all of it. */
        shadow_released_bytes -= r->shadow_size;
        --shadow_released_count;
        r->shadow_released = 0;
        ++shadow_refills;
        if (map_type != D3D10_MAP_WRITE_DISCARD && (!d->be->buffer_read || !d->be->buffer_read(d, r, GPTR(r->shadow))))
            R10_LOG("shadow refill failed: buffer of %u bytes", r->desc.width);
    }
    return 1;
}

int r10_resource_shadow(r10_device *d, r10_resource *r) { return ensure_shadow(d, r, D3D10_MAP_READ); }

/* A static vertex buffer keeps its shadow for the next Map only: one not mapped for 600 frames goes where the
 * backend can read the buffer's GPU copy back (Vulkan: host-visible homes), and the free pages of that heap go
 * back to the system. The game maps its meshes once, when it loads them (Arcadia: 47.7 MiB of shadows). */
void r10_shadow_sweep(r10_device *d)
{
    static int off = -1;
    if (off < 0) { const char *e = getenv("BIOSHOCK_R10_DEBUG"); off = e && (atoi(e) & 8388608) ? 1 : 0; }
    if (off || !d->be->buffer_read || !shadow_heaps[0]) return;
    uint32_t released = 0;
    uint64_t bytes = 0;
    for (r10_resource *r = shadow_list, *next; r; r = next) {
        next = r->shadow_next;
        if (r->shadow_heap != shadow_heaps[0] || r->mapped || r->discards || !vertex_only(r)) continue;
        if (d->frame - r->map_frame < 600 || !d->be->buffer_read(d, r, NULL)) continue;
        r10_shadow_free(r);
        r->shadow_released = 1;
        shadow_released_bytes += r->shadow_size;
        ++shadow_released_count;
        bytes += r->shadow_size;
        ++released;
    }
    if (!released) return;
    uint64_t purged = void_heap_purge(shadow_heaps[0], 0);
    shadow_purged += purged;
    static unsigned logs;
    if (logs++ < 8)
        R10_LOG("shadows: released %u of static vertex buffers (%.1f MiB), %.1f MiB of pages back; %u released (%.1f MiB), %llu refills",
                released, (double)bytes / 1048576.0, (double)purged / 1048576.0, shadow_released_count,
                (double)shadow_released_bytes / 1048576.0, (unsigned long long)shadow_refills);
}

/* QA (BIOSHOCK_R10_DEBUG 16777216): the guest ranges texture uploads read and a sample of their bytes, so the
 * census tells how much of the uploaded data the game keeps unchanged in its own memory. */
#define TEXSRC_MAX 65536u
static struct texsrc { uint32_t va, bytes; uint64_t hash; } *texsrc;
static uint32_t texsrc_next;

static uint64_t texsrc_hash(const uint8_t *p, uint32_t bytes)
{
    uint64_t h = 1469598103934665603ull;
    const uint32_t at[3] = { 0, (bytes / 2) & ~63u, bytes >= 64 ? bytes - 64 : 0 };
    for (int k = 0; k < 3; ++k)
        for (uint32_t i = 0; i < 64 && at[k] + i < bytes; ++i) h = (h ^ p[at[k] + i]) * 1099511628211ull;
    return h;
}

void r10_census_texture_source(uint32_t va, uint32_t bytes)
{
    if (bytes < 4096 || !void_vm_readable(va, bytes)) return;
    if (!texsrc && !(texsrc = (struct texsrc *)calloc(TEXSRC_MAX, sizeof *texsrc))) return;
    struct texsrc *e = &texsrc[texsrc_next++ % TEXSRC_MAX];
    e->va = va;
    e->bytes = bytes;
    e->hash = texsrc_hash((const uint8_t *)GPTR(va), bytes);
}

void r10_memory_census(r10_device *d)
{
    enum { S_VB, S_VBD, S_IB, S_CB, S_TEX, S_STAGING, S_OTHER, S_N };
    static const char *names[S_N] = { "static vertex", "discarded vertex", "index", "constant", "texture", "staging", "other" };
    uint64_t bytes[S_N] = { 0 }, cold[S_N] = { 0 };
    uint32_t count[S_N] = { 0 };
    for (r10_resource *r = shadow_list; r; r = r->shadow_next) {
        int k = r->desc.dim != R10_DIM_BUFFER ? (r->desc.usage == D3D10_USAGE_STAGING ? S_STAGING : S_TEX)
              : (r->desc.bind & D3D10_BIND_CONSTANT_BUFFER) ? S_CB : (r->desc.bind & D3D10_BIND_INDEX_BUFFER) ? S_IB
              : (r->desc.bind & D3D10_BIND_VERTEX_BUFFER) ? (r->discards ? S_VBD : S_VB) : S_OTHER;
        bytes[k] += r->shadow_size;
        ++count[k];
        if (d->frame - r->map_frame >= 600) cold[k] += r->shadow_size;
    }
    char text[1024];
    size_t n = 0;
    text[0] = 0;
    for (int k = 0; k < S_N; ++k)
        if (count[k])
            n += (size_t)snprintf(text + n, sizeof text - n, "%s%s %.1f MiB (%u, cold %.1f)", n ? "; " : "", names[k],
                                  (double)bytes[k] / 1048576.0, count[k], (double)cold[k] / 1048576.0);
    R10_LOG("census shadows: %s; released %u (%.1f MiB), %llu refills, %.1f MiB of pages purged", text, shadow_released_count,
            (double)shadow_released_bytes / 1048576.0, (unsigned long long)shadow_refills, (double)shadow_purged / 1048576.0);
    if (texsrc) {
        uint64_t kept = 0, total = 0;
        uint32_t entries = texsrc_next < TEXSRC_MAX ? texsrc_next : TEXSRC_MAX, intact = 0;
        for (uint32_t i = 0; i < entries; ++i) {
            const struct texsrc *e = &texsrc[i];
            total += e->bytes;
            if (void_vm_readable(e->va, e->bytes) && texsrc_hash((const uint8_t *)GPTR(e->va), e->bytes) == e->hash) {
                kept += e->bytes;
                ++intact;
            }
        }
        R10_LOG("census texture uploads: %u of %u recorded sources (%.1f of %.1f MiB) still unchanged in guest memory (%u uploads in all)",
                intact, entries, (double)kept / 1048576.0, (double)total / 1048576.0, texsrc_next);
    }
    void_heap_census();
    void_vm_census();
}

const uint8_t *r10_buffer_shadow(r10_resource *r) { return r->shadow ? (const uint8_t *)GPTR(r->shadow) : NULL; }

uint32_t r10_resource_map(r10_device *d, r10_resource *r, uint32_t sub, uint32_t type, uint32_t flags, uint32_t *pitch,
                          uint32_t *slice)
{
    (void)flags;
    if (sub >= r->subresources || type < D3D10_MAP_READ || type > D3D10_MAP_WRITE_NO_OVERWRITE) return 0;
    if ((type == D3D10_MAP_WRITE_DISCARD || type == D3D10_MAP_WRITE_NO_OVERWRITE) && r->desc.usage != D3D10_USAGE_DYNAMIC) return 0;
    if (r->desc.usage == D3D10_USAGE_DEFAULT || r->desc.usage == D3D10_USAGE_IMMUTABLE) return 0;
    if (!ensure_shadow(d, r, type)) return 0;
    r->map_frame = d->frame;
    ++d->stats.maps;
    if (type == D3D10_MAP_READ || type == D3D10_MAP_READ_WRITE) {
        if (r->desc.dim == R10_DIM_BUFFER) d->be->resource_read(d, r, 0, GPTR(r->shadow), r->desc.width, r->desc.width);
        else d->be->resource_read(d, r, sub, GPTR(r->shadow + r->sub_offset[sub]), r->sub_pitch[sub], r->sub_slice[sub]);
    }
    if (type == D3D10_MAP_WRITE_DISCARD) { ++r->version; ++r->discards; }
    r->mapped |= 1u << (sub & 31);
    r->map_type = type;
    if (r->desc.dim == R10_DIM_BUFFER) { *pitch = r->desc.width; *slice = r->desc.width; return r->shadow; }
    *pitch = r->sub_pitch[sub];
    *slice = r->sub_slice[sub];
    return r->shadow + r->sub_offset[sub];
}

void r10_resource_unmap(r10_device *d, r10_resource *r, uint32_t sub)
{
    if (sub >= r->subresources || !(r->mapped & (1u << (sub & 31)))) return;
    r->mapped &= ~(1u << (sub & 31));
    if (r->map_type == D3D10_MAP_READ) return;
    if (r->desc.dim == R10_DIM_BUFFER) {
        /* The game creates every buffer dynamic, its static meshes too: those are mapped with
         * NO_OVERWRITE and never discarded.  Such a buffer goes to the GPU whole at Unmap; once a
         * buffer was discarded, draws copy the bytes they read (r10_gles.c/r10_vulkan.c). Constant
         * buffers are copied whole by the draws that use them. */
        if (r->desc.bind & D3D10_BIND_CONSTANT_BUFFER) { if (r->map_type != D3D10_MAP_WRITE_DISCARD) ++r->version; return; }
        if (r->discards) return;
        ++r->version;
        d->be->resource_update(d, r, 0, 0, 0, 0, r->desc.width, 1, 1, GPTR(r->shadow), r->desc.width, r->desc.width);
        return;
    }
    ++r->version;
    if (r->desc.usage == D3D10_USAGE_STAGING) {
        /* A staging texture reaches the GPU when a copy reads it. */
        d->be->resource_update(d, r, sub, 0, 0, 0, 0, 0, 0, GPTR(r->shadow + r->sub_offset[sub]), r->sub_pitch[sub], r->sub_slice[sub]);
        return;
    }
    uint32_t w, h, depth;
    r10_subresource_size(r, sub, &w, &h, &depth);
    d->be->resource_update(d, r, sub, 0, 0, 0, w, h, depth, GPTR(r->shadow + r->sub_offset[sub]), r->sub_pitch[sub], r->sub_slice[sub]);
}

/* ------------------------------------------------------------------ views */
static void view_destroy(void_com_object *base)
{
    r10_view *v = (r10_view *)base;
    if (r10_dev && r10_dev->be) r10_dev->be->view_destroy(r10_dev, v);
    private_free(&v->obj);
    if (v->res) void_com_release(&v->res->obj.com);
    free(v);
}
COM_METHOD(view, GetResource, 2)
{
    r10_view *v = (r10_view *)void_com_host(THIS_);
    if (ARG(1)) { MEM32(ARG(1)) = v->res->obj.com.guest; void_com_addref(&v->res->obj.com); }
    return 0;
}
COM_METHOD(view, GetDesc, 2)
{
    r10_view *v = (r10_view *)void_com_host(THIS_);
    if (ARG(1)) memcpy(GPTR(ARG(1)), v->desc, v->desc_size);
    return 0;
}
static const void_fn view_methods[] = { CHILD_METHODS, view_GetResource, view_GetDesc };
static void_com_class srv_class = { "ID3D10ShaderResourceView", 9, view_methods, 0 };
static void_com_class rtv_class = { "ID3D10RenderTargetView", 9, view_methods, 0 };
static void_com_class dsv_class = { "ID3D10DepthStencilView", 9, view_methods, 0 };

uint32_t r10_create_view(r10_device *d, r10_kind kind, uint32_t resource, uint32_t desc_va, uint32_t *out)
{
    r10_resource *r = r10_resource_from(resource);
    if (!r) return E_INVALIDARG_;
    r10_view *v = (r10_view *)calloc(1, sizeof *v);
    v->res = r;
    uint32_t mips = r->desc.mips, layers = r->desc.layers;
    int cube = (r->desc.misc & D3D10_RESOURCE_MISC_TEXTURECUBE) != 0, ms = r->desc.samples > 1;
    v->desc_size = kind == R10_KIND_SRV ? 24 : 20;
    if (desc_va) {
        memcpy(v->desc, GPTR(desc_va), v->desc_size);
        v->format = MEM32(desc_va);
        v->dimension = MEM32(desc_va + 4);
        uint32_t a = MEM32(desc_va + 8), b = MEM32(desc_va + 12), c = MEM32(desc_va + 16), e = MEM32(desc_va + 20);
        if (kind == R10_KIND_SRV) {
            switch (v->dimension) {
            case D3D10_SRV_DIMENSION_BUFFER: v->buffer_first = a; v->buffer_count = b; break;
            case D3D10_SRV_DIMENSION_TEXTURE1D: case D3D10_SRV_DIMENSION_TEXTURE2D: case D3D10_SRV_DIMENSION_TEXTURE3D:
            case D3D10_SRV_DIMENSION_TEXTURECUBE:
                v->first_mip = a; v->mips = b; v->first_layer = 0; v->layers = layers; break;
            case D3D10_SRV_DIMENSION_TEXTURE1DARRAY: case D3D10_SRV_DIMENSION_TEXTURE2DARRAY:
                v->first_mip = a; v->mips = b; v->first_layer = c; v->layers = e; break;
            case D3D10_SRV_DIMENSION_TEXTURE2DMS: v->first_mip = 0; v->mips = 1; v->layers = 1; break;
            case D3D10_SRV_DIMENSION_TEXTURE2DMSARRAY: v->mips = 1; v->first_layer = a; v->layers = b; break;
            default: break;
            }
            if (v->mips == 0xFFFFFFFFu || v->first_mip + v->mips > mips) v->mips = mips - v->first_mip;
        } else if (kind == R10_KIND_RTV) {
            v->mips = 1;
            switch (v->dimension) {
            case D3D10_RTV_DIMENSION_BUFFER: v->buffer_first = a; v->buffer_count = b; break;
            case D3D10_RTV_DIMENSION_TEXTURE1D: case D3D10_RTV_DIMENSION_TEXTURE2D: v->first_mip = a; v->layers = 1; break;
            case D3D10_RTV_DIMENSION_TEXTURE1DARRAY: case D3D10_RTV_DIMENSION_TEXTURE2DARRAY:
                v->first_mip = a; v->first_layer = b; v->layers = c; break;
            case D3D10_RTV_DIMENSION_TEXTURE2DMS: v->layers = 1; break;
            case D3D10_RTV_DIMENSION_TEXTURE2DMSARRAY: v->first_layer = a; v->layers = b; break;
            case D3D10_RTV_DIMENSION_TEXTURE3D: v->first_mip = a; v->first_layer = b; v->layers = c; break;
            default: break;
            }
        } else {
            v->mips = 1;
            switch (v->dimension) {
            case D3D10_DSV_DIMENSION_TEXTURE1D: case D3D10_DSV_DIMENSION_TEXTURE2D: v->first_mip = a; v->layers = 1; break;
            case D3D10_DSV_DIMENSION_TEXTURE1DARRAY: case D3D10_DSV_DIMENSION_TEXTURE2DARRAY:
                v->first_mip = a; v->first_layer = b; v->layers = c; break;
            case D3D10_DSV_DIMENSION_TEXTURE2DMS: v->layers = 1; break;
            case D3D10_DSV_DIMENSION_TEXTURE2DMSARRAY: v->first_layer = a; v->layers = b; break;
            default: break;
            }
        }
    } else {
        /* A NULL description views the whole resource in its own format. */
        v->format = r->desc.format;
        v->first_mip = 0;
        v->mips = kind == R10_KIND_SRV ? mips : 1;
        v->layers = kind == R10_KIND_SRV ? layers : (r->desc.dim == R10_DIM_TEXTURE3D ? r->desc.depth : layers);
        if (r->desc.dim == R10_DIM_BUFFER) { v->dimension = 1; v->buffer_first = 0; v->buffer_count = r->desc.width; }
        else if (kind == R10_KIND_SRV)
            v->dimension = r->desc.dim == R10_DIM_TEXTURE1D ? (layers > 1 ? 3 : 2)
                         : r->desc.dim == R10_DIM_TEXTURE3D ? 8 : cube ? 9 : ms ? (layers > 1 ? 7 : 6) : (layers > 1 ? 5 : 4);
        else if (kind == R10_KIND_RTV)
            v->dimension = r->desc.dim == R10_DIM_TEXTURE1D ? (layers > 1 ? 3 : 2) : r->desc.dim == R10_DIM_TEXTURE3D ? 8
                         : ms ? (layers > 1 ? 7 : 6) : (layers > 1 ? 5 : 4);
        else
            v->dimension = r->desc.dim == R10_DIM_TEXTURE1D ? (layers > 1 ? 2 : 1) : ms ? (layers > 1 ? 6 : 5) : (layers > 1 ? 4 : 3);
        /* GetDesc returns what the runtime filled in. */
        uint32_t words[6] = { v->format, v->dimension, 0, 0, 0, 0 };
        if (kind == R10_KIND_SRV) { words[2] = v->first_mip; words[3] = v->mips; if (layers > 1) { words[4] = 0; words[5] = layers; } }
        else if (layers > 1) { words[3] = 0; words[4] = layers; }
        memcpy(v->desc, words, v->desc_size);
    }
    if (!v->layers) v->layers = 1;
    if (!v->mips) v->mips = 1;
    uint32_t typed = v->format ? v->format : r->desc.format;
    if (kind == R10_KIND_DSV) typed = r10_format_resource_typed(typed, D3D10_BIND_DEPTH_STENCIL);
    else if (r10_format_get(typed) && r10_format_get(typed)->kind == R10_FMT_TYPELESS) typed = r10_format_resource_typed(typed, 0);
    v->format = typed;
    v->fmt = r10_format_get(typed);
    if (!v->fmt || v->first_mip >= mips) {
        R10_LOG("view of kind %d: format %u dimension %u mip %u unsupported", kind, v->format, v->dimension, v->first_mip);
        free(v);
        return E_INVALIDARG_;
    }
    if (!out) { free(v); return S_FALSE_; }
    void_com_addref(&r->obj.com);
    v->obj.kind = kind;   /* the backend picks the view kind's path (an SRV's swizzle, mips, type) */
    if (!d->be->view_create(d, v)) {
        void_com_release(&r->obj.com);
        free(v);
        return E_OUTOFMEMORY_;
    }
    r10_object_wrap(&v->obj, kind == R10_KIND_SRV ? &srv_class : kind == R10_KIND_RTV ? &rtv_class : &dsv_class, kind);
    v->obj.com.destroy = view_destroy;
    *out = v->obj.com.guest;
    return 0;
}

/* ------------------------------------------------------------------ state objects */
static void state_destroy(void_com_object *base)
{
    r10_state_object *s = (r10_state_object *)base;
    if (r10_dev && r10_dev->be) {
        if (s->obj.kind == R10_KIND_SAMPLER) r10_dev->be->sampler_destroy(r10_dev, s);
        else if (r10_dev->be->state_destroy) r10_dev->be->state_destroy(r10_dev, s);
    }
    private_free(&s->obj);
    free(s);
}
COM_METHOD(state, GetDesc, 2)
{
    r10_state_object *s = (r10_state_object *)void_com_host(THIS_);
    uint32_t size = s->obj.kind == R10_KIND_BLEND ? 68 : s->obj.kind == R10_KIND_DEPTH ? 52 : s->obj.kind == R10_KIND_RASTER ? 40 : 52;
    if (ARG(1)) memcpy(GPTR(ARG(1)), &s->d, size);
    return 0;
}
static const void_fn state_methods[] = { CHILD_METHODS, state_GetDesc };
static void_com_class blend_class = { "ID3D10BlendState", 8, state_methods, 0 };
static void_com_class depth_class = { "ID3D10DepthStencilState", 8, state_methods, 0 };
static void_com_class raster_class = { "ID3D10RasterizerState", 8, state_methods, 0 };
static void_com_class sampler_class = { "ID3D10SamplerState", 8, state_methods, 0 };

static uint32_t hash_bytes(const void *data, size_t size)
{
    const uint8_t *p = (const uint8_t *)data;
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < size; ++i) { h ^= p[i]; h *= 16777619u; }
    return h ? h : 1;
}

/* D3D10 returns the same object for an identical description; the engine caches its states anyway, so
 * a small table per kind keeps the backends' pipeline keys stable. */
#define STATE_CACHE 4096
static r10_state_object *state_cache[4][STATE_CACHE];

uint32_t r10_create_state(r10_device *d, r10_kind kind, uint32_t desc_va, uint32_t *out)
{
    if (!desc_va) return E_INVALIDARG_;
    uint32_t size = kind == R10_KIND_BLEND ? 68 : kind == R10_KIND_DEPTH ? 52 : kind == R10_KIND_RASTER ? 40 : 52;
    unsigned table = kind == R10_KIND_BLEND ? 0 : kind == R10_KIND_DEPTH ? 1 : kind == R10_KIND_RASTER ? 2 : 3;
    uint32_t h = hash_bytes(GPTR(desc_va), size);
    for (unsigned i = 0; i < STATE_CACHE; ++i) {
        r10_state_object *s = state_cache[table][(h + i) % STATE_CACHE];
        if (!s) break;
        if (s->hash == h && !memcmp(&s->d, GPTR(desc_va), size) && s->obj.com.guest) {
            if (out) { void_com_addref(&s->obj.com); *out = s->obj.com.guest; }
            return out ? 0 : S_FALSE_;
        }
    }
    if (!out) return S_FALSE_;
    r10_state_object *s = (r10_state_object *)calloc(1, sizeof *s);
    memcpy(&s->d, GPTR(desc_va), size);
    s->hash = h;
    if (kind == R10_KIND_SAMPLER && !d->be->sampler_create(d, s)) { free(s); return E_OUTOFMEMORY_; }
    void_com_class *cls = kind == R10_KIND_BLEND ? &blend_class : kind == R10_KIND_DEPTH ? &depth_class
                        : kind == R10_KIND_RASTER ? &raster_class : &sampler_class;
    r10_object_wrap(&s->obj, cls, kind);
    s->obj.com.destroy = state_destroy;
    /* The cache holds one reference of its own: the objects live as long as the device. */
    void_com_addref(&s->obj.com);
    for (unsigned i = 0; i < STATE_CACHE; ++i) {
        r10_state_object **slot = &state_cache[table][(h + i) % STATE_CACHE];
        if (!*slot) { *slot = s; break; }
    }
    *out = s->obj.com.guest;
    return 0;
}

/* ------------------------------------------------------------------ shaders */
static void shader_destroy(void_com_object *base)
{
    r10_shader *s = (r10_shader *)base;
    if (r10_dev && r10_dev->be) r10_dev->be->shader_destroy(r10_dev, s);
    private_free(&s->obj);
    free(s->inputs);
    free(s);
}
static const void_fn shader_methods[] = { CHILD_METHODS };
static void_com_class vs_class = { "ID3D10VertexShader", 7, shader_methods, 0 };
static void_com_class ps_class = { "ID3D10PixelShader", 7, shader_methods, 0 };
static void_com_class gs_class = { "ID3D10GeometryShader", 7, shader_methods, 0 };

/* The game's bytecode is a token of a library program (r10_shaderlib.h); DXBC only when the engine reads
 * ShaderCache.pcs10 itself (BIOSHOCK_R10_SHADERS=dxbc), found by the hash of the container it was translated from. */
uint32_t r10_create_shader(r10_device *d, int type, uint32_t bytecode, uint32_t size, uint32_t *out)
{
    if (!bytecode || size < 32 || !void_vm_readable(bytecode, size)) return E_INVALIDARG_;
    const uint8_t *bytes = (const uint8_t *)GPTR(bytecode);
    const r10_lib_shader *token = r10_shaderlib_token(bytes, size);
    if (token ? (type > 1 || token->type != (uint8_t)type) : memcmp(bytes, "DXBC", 4) != 0) return E_INVALIDARG_;
    if (!out) return S_FALSE_;
    r10_shader *s = (r10_shader *)calloc(1, sizeof *s);
    s->type = type;
    if (token) {
        s->lib = token;
        s->hash = token->hash;
        s->size = token->dxbc_size;
    } else {
        s->size = size;
        s->lib = r10_shaderlib_find(bytes, size, &s->hash);
        ++d->shader_stats.from_dxbc;
    }
    ++d->shader_stats.created;
    if (s->lib) ++d->shader_stats.found;
    if (!s->lib) {
        static unsigned misses;
        if (misses++ < 64) {
            R10_LOG("SHADER_MISS %s %016llx size %u: not in the build-time library (dump in %s)", type == 0 ? "PS" : type == 1 ? "VS" : "GS",
                    (unsigned long long)s->hash, size, void_data_dir() ? void_data_dir() : "?");
            const char *dir = void_data_dir();
            if (dir) {
                char path[1200];
                snprintf(path, sizeof path, "%s/r10-shader-miss-%016llx.dxbc", dir, (unsigned long long)s->hash);
                FILE *f = fopen(path, "wb");
                if (f) { fwrite(GPTR(bytecode), 1, size, f); fclose(f); }
            }
        }
    } else {
        const r10_lib_binding *b = (const r10_lib_binding *)r10_lib_at(s->lib->bindings);
        uint32_t stage = (uint32_t)type * 160u;
        for (uint32_t i = 0; i < s->lib->binding_count; ++i) {
            uint32_t n = b[i].binding - stage;
            if (n < 16) s->used_cb |= 1u << n;
            else if (n < 32) s->used_sampler |= 1u << (n - 16);
            else if (n < 64) s->used_srv |= 1u << (n - 32);
        }
    }
    if (type == 1) r10_shader_input_signature(bytes, size, &s->inputs, &s->input_count);
    if (s->lib && !d->be->shader_create(d, s)) R10_LOG("backend rejected shader %016llx", (unsigned long long)s->hash);
    r10_object_wrap(&s->obj, type == 0 ? &ps_class : type == 1 ? &vs_class : &gs_class,
                    type == 0 ? R10_KIND_PS : type == 1 ? R10_KIND_VS : R10_KIND_GS);
    s->obj.com.destroy = shader_destroy;
    *out = s->obj.com.guest;
    return 0;
}

/* ------------------------------------------------------------------ input layouts */
static void layout_destroy(void_com_object *base)
{
    r10_layout *l = (r10_layout *)base;
    if (r10_dev && r10_dev->be && r10_dev->be->layout_destroy) r10_dev->be->layout_destroy(r10_dev, l);
    private_free(&l->obj);
    free(l);
}
static void_com_class layout_class = { "ID3D10InputLayout", 7, shader_methods, 0 };

uint32_t r10_create_layout(r10_device *d, uint32_t elements, uint32_t count, uint32_t bytecode, uint32_t size, uint32_t *out)
{
    (void)d;
    if (count > 32 || (count && !elements) || !bytecode || size < 32) return E_INVALIDARG_;
    r10_sig *sig = NULL;
    uint32_t sig_count = 0;
    if (!void_vm_readable(bytecode, size) ||
        !r10_shader_input_signature((const uint8_t *)GPTR(bytecode), size, &sig, &sig_count)) return E_INVALIDARG_;
    r10_layout *l = (r10_layout *)calloc(1, sizeof *l);
    uint32_t next_offset[R10_VB_SLOTS] = { 0 };
    for (uint32_t i = 0; i < count; ++i) {
        uint32_t e = elements + 28 * i;
        char name[64];
        uint32_t name_va = MEM32(e);
        size_t n = name_va ? void_gstrlen(name_va) : 0;
        if (n >= sizeof name) n = sizeof name - 1;
        memcpy(name, name_va ? GPTR(name_va) : "", n);
        name[n] = 0;
        uint32_t index = MEM32(e + 4), format = MEM32(e + 8), slot = MEM32(e + 12), offset = MEM32(e + 16);
        uint32_t klass = MEM32(e + 20), step = MEM32(e + 24);
        if (slot >= R10_VB_SLOTS) continue;
        const r10_format *f = r10_format_get(format);
        if (offset == 0xFFFFFFFFu) offset = (next_offset[slot] + 3u) & ~3u;   /* D3D10_APPEND_ALIGNED_ELEMENT */
        next_offset[slot] = offset + (f ? f->bytes : 4);
        int found = 0;
        for (uint32_t s = 0; s < sig_count; ++s) {
            if (sig[s].index == index && !strcasecmp(sig[s].name, name)) {
                r10_element *el = &l->el[l->count++];
                el->location = sig[s].reg;
                el->format = format;
                el->slot = slot;
                el->offset = offset;
                el->instanced = klass == 1;
                el->step = step;
                l->slot_mask |= 1u << slot;
                found = 1;
                break;
            }
        }
        (void)found;   /* elements the shader does not read are allowed */
    }
    free(sig);
    l->hash = hash_bytes(l->el, sizeof(r10_element) * l->count) ^ l->count;
    if (!out) { free(l); return S_FALSE_; }
    r10_object_wrap(&l->obj, &layout_class, R10_KIND_LAYOUT);
    l->obj.com.destroy = layout_destroy;
    *out = l->obj.com.guest;
    return 0;
}

/* ------------------------------------------------------------------ queries */
static void query_destroy(void_com_object *base)
{
    r10_query *q = (r10_query *)base;
    if (r10_dev && r10_dev->be) r10_dev->be->query_destroy(r10_dev, q);
    private_free(&q->obj);
    free(q);
}
COM_METHOD(query, Begin, 1)
{
    r10_query *q = (r10_query *)void_com_host(THIS_);
    if (q->type == D3D10_QUERY_EVENT || q->type == D3D10_QUERY_TIMESTAMP) return 0;
    q->issued = 1; q->ended = 0;
    r10_dev->be->query_begin(r10_dev, q);
    return 0;
}
COM_METHOD(query, End, 1)
{
    r10_query *q = (r10_query *)void_com_host(THIS_);
    q->ended = 1;
    r10_dev->be->query_end(r10_dev, q);
    return 0;
}
COM_METHOD(query, GetData, 4)
{
    r10_query *q = (r10_query *)void_com_host(THIS_);
    uint32_t data = ARG(1), size = ARG(2), flags = ARG(3);
    static _Thread_local unsigned spins;
    static _Thread_local uint64_t pending_since;   /* first S_FALSE of the poll loop now running */
    if (!q->ended) return DXGI_ERROR_INVALID_CALL_;
    int flush = !(flags & D3D10_ASYNC_GETDATA_DONOTFLUSH);
    int assume_visible = 0;
    if (flush && q->type == D3D10_QUERY_OCCLUSION) {
        /* A pending culling query counts as visible. Let the normal frame submission carry it. */
        static int wait_occlusion = -1;
        if (wait_occlusion < 0) wait_occlusion = r10_env_int("BIOSHOCK_R10_OCCLUSION_WAIT", 0, 0, 1);
        assume_visible = !wait_occlusion;
    }
    if (!r10_dev->be->query_poll(r10_dev, q, flush && !assume_visible)) {
        if (assume_visible) {
            spins = 0;
            pending_since = 0;
            if (data && size >= 8) { MEM32(data) = 1; MEM32(data + 4) = 0; }
            return 0;
        }
        /* The game spins on GetData (ReduceMouseLag, occlusion results).  After 8 pending polls with a
         * flush the thread sleeps 50 us, so the GPU and the other guest threads get the time (the
         * pending-query wait of the former Direct3D 9 path). */
        if (!pending_since) pending_since = r10_now_ns();
        if (!flush || spins < 8) { ++spins; return S_FALSE_; }
        struct timespec delay = { 0, 50000 };
        void_blocking_begin();
        while (nanosleep(&delay, &delay) && errno == EINTR) {}
        void_blocking_end();
        return S_FALSE_;
    }
    spins = 0;
    if (pending_since) { r10_dev->stats.query_wait_us += (uint32_t)((r10_now_ns() - pending_since) / 1000); pending_since = 0; }
    if (data && size) {
        switch (q->type) {
        case D3D10_QUERY_EVENT: case D3D10_QUERY_OCCLUSION_PREDICATE: case D3D10_QUERY_SO_OVERFLOW_PREDICATE:
            if (size >= 4) MEM32(data) = q->type == D3D10_QUERY_EVENT ? 1 : q->samples != 0;
            break;
        case D3D10_QUERY_OCCLUSION: case D3D10_QUERY_TIMESTAMP:
            if (size >= 8) { MEM32(data) = (uint32_t)q->samples; MEM32(data + 4) = (uint32_t)(q->samples >> 32); }
            break;
        case D3D10_QUERY_TIMESTAMP_DISJOINT:
            if (size >= 16) { MEM32(data) = 1000000000u; MEM32(data + 4) = 0; MEM32(data + 8) = 0; }
            break;
        default:
            memset(GPTR(data), 0, size);
        }
    }
    return 0;
}
COM_METHOD(query, GetDataSize, 1)
{
    r10_query *q = (r10_query *)void_com_host(THIS_);
    switch (q->type) {
    case D3D10_QUERY_EVENT: case D3D10_QUERY_OCCLUSION_PREDICATE: case D3D10_QUERY_SO_OVERFLOW_PREDICATE: return 4;
    case D3D10_QUERY_OCCLUSION: case D3D10_QUERY_TIMESTAMP: return 8;
    case D3D10_QUERY_TIMESTAMP_DISJOINT: return 16;
    case D3D10_QUERY_PIPELINE_STATISTICS: return 88;
    case D3D10_QUERY_SO_STATISTICS: return 16;
    default: return 0;
    }
}
COM_METHOD(query, GetDesc, 2)
{
    r10_query *q = (r10_query *)void_com_host(THIS_);
    if (ARG(1)) { MEM32(ARG(1)) = q->type; MEM32(ARG(1) + 4) = q->misc; }
    return 0;
}
static const void_fn query_methods[] = { CHILD_METHODS, query_Begin, query_End, query_GetData, query_GetDataSize, query_GetDesc };
static void_com_class query_class = { "ID3D10Query", 12, query_methods, 0 };

uint32_t r10_create_query(r10_device *d, uint32_t desc_va, uint32_t *out)
{
    if (!desc_va) return E_INVALIDARG_;
    if (!out) return S_FALSE_;
    r10_query *q = (r10_query *)calloc(1, sizeof *q);
    q->type = MEM32(desc_va);
    q->misc = MEM32(desc_va + 4);
    if (!d->be->query_create(d, q)) { free(q); return E_OUTOFMEMORY_; }
    r10_object_wrap(&q->obj, &query_class, R10_KIND_QUERY);
    q->obj.com.destroy = query_destroy;
    *out = q->obj.com.guest;
    return 0;
}

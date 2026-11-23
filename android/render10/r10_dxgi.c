/* DXGI of the native renderer: one factory, one adapter (the device's GPU), one output (the Android
 * display) and the swap chain whose back buffer the backends show in the activity's window. */
#include "r10.h"
#include <stdlib.h>

extern int void_android_display_size(int *w, int *h) __attribute__((weak));
extern void void_input_screen_size(int *w, int *h);
extern void void_input_set_screen_size(int w, int h);
/* android/host/adpf.c: the frame's work for the ADPF hint session. */
extern void ut3_adpf_work_begin(void);
extern void ut3_adpf_work_end(void);

static const uint8_t IID_IUnknown_[16] = { 0, 0, 0, 0, 0, 0, 0, 0, 0xC0, 0, 0, 0, 0, 0, 0, 0x46 };
static const uint8_t IID_ID3D10Device_[16] = { 0x0F, 0x4C, 0x7E, 0x9B, 0x2C, 0x34, 0x06, 0x41, 0xA1, 0x9F, 0x4F, 0x27, 0x04, 0xF6, 0x89, 0xF0 };
static const uint8_t IID_ID3D10Device1_[16] = { 0x8F, 0xB7, 0x50, 0x9B, 0xD3, 0xA6, 0xC6, 0x4B, 0x9C, 0xF7, 0x10, 0x3F, 0xF2, 0xB2, 0x48, 0x8C };

typedef struct dxgi_object { void_com_object com; r10_kind kind; } dxgi_object;
typedef struct dxgi_swapchain {
    dxgi_object base;
    uint8_t desc[60];            /* DXGI_SWAP_CHAIN_DESC */
    r10_resource *backbuffer;
    uint32_t fullscreen, presents;
} dxgi_swapchain;

static uint32_t factory_va, adapter_va, output_va;

/* ------------------------------------------------------------------ modes */
static const uint32_t pc_modes[][2] = {
    {640, 480}, {800, 600}, {1024, 768}, {1152, 864}, {1280, 720}, {1280, 768}, {1280, 800}, {1280, 960},
    {1280, 1024}, {1360, 768}, {1366, 768}, {1440, 900}, {1560, 720}, {1600, 900}, {1600, 1200}, {1680, 1050},
    {1920, 1080}, {2340, 1080},
};
#define MODE_COUNT (sizeof pc_modes / sizeof pc_modes[0])

static uint32_t env_dimension(const char *name)
{
    const char *text = getenv(name);
    if (!text || *text < '0' || *text > '9') return 0;
    char *end;
    unsigned long v = strtoul(text, &end, 10);
    return !*end && v >= 8 && v <= 8192 ? (uint32_t)v : 0;
}

static int display_size(uint32_t out[2])
{
    uint32_t w = env_dimension("UT3_RENDER_WIDTH"), h = env_dimension("UT3_RENDER_HEIGHT");
    if (w && h && w >= h) { out[0] = w; out[1] = h; return 1; }
    int x = 0, y = 0;
    if (!void_android_display_size || !void_android_display_size(&x, &y)) void_input_screen_size(&x, &y);
    if (x <= 0 || y <= 0) { out[0] = 1280; out[1] = 720; return 0; }
    out[0] = (uint32_t)(x > y ? x : y); out[1] = (uint32_t)(x > y ? y : x);
    return 1;
}

static uint32_t refresh_rate(void)
{
    float fps = r10_frame_limit();
    return fps >= 1 && fps <= 1000 ? (uint32_t)(fps + 0.5f) : 60;
}

/* The PC's familiar heights, each at the display's aspect ratio, then the display itself. */
static uint32_t display_modes(uint32_t result[MODE_COUNT + 1][2])
{
    uint32_t display[2], count = 0;
    int native = display_size(display);
    for (unsigned i = 0; i < MODE_COUNT + (unsigned)native; ++i) {
        uint32_t h = i == MODE_COUNT ? display[1] : pc_modes[i][1];
        uint64_t w = native ? ((uint64_t)h * display[0] + display[1] / 2u) / display[1] : pc_modes[i][0];
        if (w < 8 || w > 8192 || h > display[1]) continue;
        unsigned j = 0;
        while (j < count && (result[j][0] != w || result[j][1] != h)) ++j;
        if (j < count) continue;
        result[count][0] = (uint32_t)w;
        result[count++][1] = h;
    }
    /* Sorted by size, as DXGI lists them. */
    for (unsigned i = 1; i < count; ++i)
        for (unsigned j = i; j && (uint64_t)result[j][0] * result[j][1] < (uint64_t)result[j - 1][0] * result[j - 1][1]; --j) {
            uint32_t t0 = result[j][0], t1 = result[j][1];
            result[j][0] = result[j - 1][0]; result[j][1] = result[j - 1][1];
            result[j - 1][0] = t0; result[j - 1][1] = t1;
        }
    return count;
}

static void write_mode(uint32_t out, uint32_t w, uint32_t h, uint32_t format)
{
    MEM32(out) = w; MEM32(out + 4) = h; MEM32(out + 8) = refresh_rate(); MEM32(out + 12) = 1;
    MEM32(out + 16) = format; MEM32(out + 20) = 0; MEM32(out + 24) = 0;
}

/* ------------------------------------------------------------------ IDXGIObject */
static dxgi_object *obj(void) { return (dxgi_object *)void_com_host(THIS_); }
COM_METHOD(dxgi, QueryInterface, 3)
{
    dxgi_object *o = obj();
    if (!ARG(2)) return E_POINTER_;
    if (o->kind == R10_KIND_SWAPCHAIN && (void_guid_equal(ARG(1), IID_ID3D10Device_) || void_guid_equal(ARG(1), IID_ID3D10Device1_))) {
        MEM32(ARG(2)) = 0;
        return E_NOINTERFACE_;
    }
    void_com_addref(&o->com);
    MEM32(ARG(2)) = THIS_;
    return 0;
}
COM_METHOD(dxgi, AddRef, 1) { return void_com_addref(&obj()->com); }
/* Factory, adapter and output live for the process. */
COM_METHOD(dxgi, Release, 1)
{
    dxgi_object *o = obj();
    if (o->kind != R10_KIND_SWAPCHAIN) return o->com.refs > 1 ? (uint32_t)--o->com.refs : 1;
    return void_com_release(&o->com);
}
COM_METHOD(dxgi, SetPrivateData, 4) { (void)obj(); return 0; }
COM_METHOD(dxgi, SetPrivateDataInterface, 3) { (void)obj(); return 0; }
COM_METHOD(dxgi, GetPrivateData, 4) { (void)obj(); return DXGI_ERROR_NOT_FOUND_; }
COM_METHOD(dxgi, GetParent, 3)
{
    dxgi_object *o = obj();
    uint32_t parent = o->kind == R10_KIND_ADAPTER ? factory_va : o->kind == R10_KIND_OUTPUT ? adapter_va
                    : o->kind == R10_KIND_SWAPCHAIN ? factory_va : 0;
    if (!ARG(2)) return E_POINTER_;
    if (!parent) { MEM32(ARG(2)) = 0; return E_NOINTERFACE_; }
    r10_object_addref_guest(parent);
    MEM32(ARG(2)) = parent;
    return 0;
}
#define OBJECT_METHODS dxgi_QueryInterface, dxgi_AddRef, dxgi_Release, dxgi_SetPrivateData, dxgi_SetPrivateDataInterface, \
                       dxgi_GetPrivateData, dxgi_GetParent

/* ------------------------------------------------------------------ IDXGIFactory */
COM_METHOD(fac, EnumAdapters, 3)
{
    (void)obj();
    if (!ARG(2)) return E_INVALIDARG_;
    if (ARG(1)) { MEM32(ARG(2)) = 0; return DXGI_ERROR_NOT_FOUND_; }
    r10_object_addref_guest(adapter_va);
    MEM32(ARG(2)) = adapter_va;
    return 0;
}
COM_METHOD(fac, MakeWindowAssociation, 3) { (void)obj(); return 0; }
COM_METHOD(fac, GetWindowAssociation, 2) { (void)obj(); if (ARG(1)) MEM32(ARG(1)) = void_window_main(); return 0; }
COM_METHOD(fac, CreateSwapChain, 4)
{
    (void)obj();
    if (!r10_dev || !ARG(2) || !ARG(3)) return E_INVALIDARG_;
    return r10_swapchain_create(r10_dev, ARG(2), (uint32_t *)GPTR(ARG(3)));
}
COM_METHOD(fac, CreateSoftwareAdapter, 3) { (void)obj(); if (ARG(2)) MEM32(ARG(2)) = 0; return DXGI_ERROR_UNSUPPORTED_; }
static const void_fn factory_methods[] = { OBJECT_METHODS, fac_EnumAdapters, fac_MakeWindowAssociation, fac_GetWindowAssociation,
                                           fac_CreateSwapChain, fac_CreateSoftwareAdapter };
static void_com_class factory_class = { "IDXGIFactory", 12, factory_methods, 0 };

/* ------------------------------------------------------------------ IDXGIAdapter */
COM_METHOD(ada, EnumOutputs, 3)
{
    (void)obj();
    if (!ARG(2)) return E_INVALIDARG_;
    if (ARG(1)) { MEM32(ARG(2)) = 0; return DXGI_ERROR_NOT_FOUND_; }
    r10_object_addref_guest(output_va);
    MEM32(ARG(2)) = output_va;
    return 0;
}
COM_METHOD(ada, GetDesc, 2)
{
    (void)obj();
    uint32_t out = ARG(1);
    if (!out) return E_INVALIDARG_;
    memset(GPTR(out), 0, 292);
    /* The same identity the Direct3D 9 path reported, so the engine keeps its PC defaults. */
    void_utf8_to_gw("NVIDIA GeForce RTX 4060", out, 128);
    MEM32(out + 256) = 0x10DE;           /* VendorId */
    MEM32(out + 260) = 0x2882;           /* DeviceId */
    MEM32(out + 264) = 0x51611462u;      /* SubSysId */
    MEM32(out + 268) = 161;              /* Revision */
    MEM32(out + 272) = 1024u << 20;      /* DedicatedVideoMemory */
    MEM32(out + 276) = 0;                /* DedicatedSystemMemory */
    MEM32(out + 280) = 1024u << 20;      /* SharedSystemMemory */
    MEM32(out + 284) = 0x1234;           /* AdapterLuid */
    return 0;
}
COM_METHOD(ada, CheckInterfaceSupport, 3)
{
    (void)obj();
    if (!void_guid_equal(ARG(1), IID_ID3D10Device_)) return DXGI_ERROR_UNSUPPORTED_;
    if (ARG(2)) { MEM32(ARG(2)) = 0x00000001u; MEM32(ARG(2) + 4) = 0x0007000Au; }   /* UMD version 7.10.0.1 */
    return 0;
}
static const void_fn adapter_methods[] = { OBJECT_METHODS, ada_EnumOutputs, ada_GetDesc, ada_CheckInterfaceSupport };
static void_com_class adapter_class = { "IDXGIAdapter", 10, adapter_methods, 0 };

/* ------------------------------------------------------------------ IDXGIOutput */
COM_METHOD(out, GetDesc, 2)
{
    (void)obj();
    uint32_t o = ARG(1), size[2];
    if (!o) return E_INVALIDARG_;
    display_size(size);
    memset(GPTR(o), 0, 92);
    void_utf8_to_gw("\\\\.\\DISPLAY1", o, 32);
    MEM32(o + 64) = 0; MEM32(o + 68) = 0; MEM32(o + 72) = size[0]; MEM32(o + 76) = size[1];
    MEM32(o + 80) = 1;                   /* AttachedToDesktop */
    MEM32(o + 84) = 1;                   /* DXGI_MODE_ROTATION_IDENTITY */
    MEM32(o + 88) = 0x00010001u;         /* HMONITOR */
    return 0;
}
COM_METHOD(out, GetDisplayModeList, 5)
{
    (void)obj();
    uint32_t format = ARG(1), count_va = ARG(3), list = ARG(4);
    if (!count_va) return E_INVALIDARG_;
    uint32_t modes[MODE_COUNT + 1][2], n = format ? display_modes(modes) : 0;
    if (!list) { MEM32(count_va) = n; return 0; }
    uint32_t cap = MEM32(count_va), w = n < cap ? n : cap;
    for (uint32_t i = 0; i < w; ++i) write_mode(list + 28 * i, modes[i][0], modes[i][1], format);
    MEM32(count_va) = w;
    return w < n ? 0x887A0003u /* DXGI_ERROR_MORE_DATA */ : 0;
}
COM_METHOD(out, FindClosestMatchingMode, 4)
{
    (void)obj();
    uint32_t want = ARG(1), result = ARG(2);
    if (!want || !result) return E_INVALIDARG_;
    uint32_t modes[MODE_COUNT + 1][2], n = display_modes(modes);
    uint32_t w = MEM32(want), h = MEM32(want + 4), format = MEM32(want + 16) ? MEM32(want + 16) : 28;
    uint32_t best = n ? n - 1 : 0;
    uint64_t best_score = UINT64_MAX;
    for (uint32_t i = 0; i < n && (w || h); ++i) {
        int64_t dw = (int64_t)modes[i][0] - w, dh = (int64_t)modes[i][1] - h;
        uint64_t score = (uint64_t)(dw * dw + dh * dh);
        if (score < best_score) { best_score = score; best = i; }
    }
    write_mode(result, n ? modes[best][0] : 1280, n ? modes[best][1] : 720, format);
    return 0;
}
COM_METHOD(out, WaitForVBlank, 1) { (void)obj(); return 0; }
COM_METHOD(out, TakeOwnership, 3) { (void)obj(); return 0; }
COM_METHOD(out, ReleaseOwnership, 1) { (void)obj(); return 0; }
COM_METHOD(out, GetGammaControlCapabilities, 2) { (void)obj(); if (ARG(1)) memset(GPTR(ARG(1)), 0, 4 + 4 + 4 + 4 + 1025 * 4); return 0; }
COM_METHOD(out, SetGammaControl, 2) { (void)obj(); return 0; }
COM_METHOD(out, GetGammaControl, 2) { (void)obj(); return DXGI_ERROR_UNSUPPORTED_; }
COM_METHOD(out, SetDisplaySurface, 2) { (void)obj(); return DXGI_ERROR_UNSUPPORTED_; }
COM_METHOD(out, GetDisplaySurfaceData, 2) { (void)obj(); return DXGI_ERROR_UNSUPPORTED_; }
COM_METHOD(out, GetFrameStatistics, 2) { (void)obj(); if (ARG(1)) memset(GPTR(ARG(1)), 0, 32); return 0; }
static const void_fn output_methods[] = { OBJECT_METHODS, out_GetDesc, out_GetDisplayModeList, out_FindClosestMatchingMode,
    out_WaitForVBlank, out_TakeOwnership, out_ReleaseOwnership, out_GetGammaControlCapabilities, out_SetGammaControl,
    out_GetGammaControl, out_SetDisplaySurface, out_GetDisplaySurfaceData, out_GetFrameStatistics };
static void_com_class output_class = { "IDXGIOutput", 19, output_methods, 0 };

/* ------------------------------------------------------------------ IDXGISwapChain */
static dxgi_swapchain *chain(void)
{
    dxgi_object *o = obj();
    if (o->kind != R10_KIND_SWAPCHAIN) void_lift_trap(g_cur_func, "IDXGISwapChain method on another DXGI object");
    return (dxgi_swapchain *)o;
}

static int make_backbuffer(dxgi_swapchain *s)
{
    uint32_t w, h, format;
    memcpy(&w, s->desc, 4);
    memcpy(&h, s->desc + 4, 4);
    memcpy(&format, s->desc + 16, 4);
    if (!w || !h) {
        uint32_t size[2];
        display_size(size);
        if (!w) w = size[0];
        if (!h) h = size[1];
        memcpy(s->desc, &w, 4);
        memcpy(s->desc + 4, &h, 4);
    }
    r10_resource_desc desc = { 0 };
    desc.dim = R10_DIM_TEXTURE2D;
    desc.width = w; desc.height = h; desc.depth = 1; desc.mips = 1; desc.layers = 1;
    desc.format = format ? format : 28;
    desc.samples = 1;
    desc.usage = D3D10_USAGE_DEFAULT;
    desc.bind = D3D10_BIND_RENDER_TARGET | D3D10_BIND_SHADER_RESOURCE;
    r10_resource *r = NULL;
    if (r10_create_texture_internal(r10_dev, &desc, &r)) return 0;
    r->is_backbuffer = 1;
    s->backbuffer = r;
    r10_dev->bb_width = w;
    r10_dev->backbuffer = r;
    r10_dev->bb_height = h;
    /* Touch coordinates must use the size of the image being presented. */
    void_input_set_screen_size((int)w, (int)h);
    R10_LOG("swap chain back buffer %ux%u %s", w, h, r->fmt->name);
    return 1;
}

static void chain_destroy(void_com_object *o)
{
    dxgi_swapchain *s = (dxgi_swapchain *)o;
    if (r10_dev && r10_dev->backbuffer == s->backbuffer) r10_dev->backbuffer = NULL;
    if (s->backbuffer) void_com_release(&s->backbuffer->obj.com);
    free(s);
}

COM_METHOD(sc, GetDevice, 3)
{
    (void)chain();
    if (!ARG(2) || !r10_dev) return E_INVALIDARG_;
    void_com_addref(&r10_dev->obj.com);
    MEM32(ARG(2)) = r10_dev->obj.com.guest;
    return 0;
}
COM_METHOD(sc, Present, 3)
{
    dxgi_swapchain *s = chain();
    if (!r10_dev || !s->backbuffer) return E_FAIL_;
    ++s->presents;
    /* The frame's work ends here and the next one's begins when Present returns, after the limiter and the
     * wait for frames in flight: the ADPF hint session (android/host/adpf.c) sees the game's own work. */
    ut3_adpf_work_end();
    uint64_t start = r10_now_ns();
    uint32_t hr = r10_dev->be->present(r10_dev, s->backbuffer, ARG(1));
    uint64_t took = r10_now_ns() - start;
    /* A present that keeps the machine long starves the audio producer (threads.c, Loans): name its parts. */
    static unsigned slow_reports;
    if (took > 40000000ull && slow_reports < 40) {
        ++slow_reports;
        R10_LOG("slow present %.1f ms (frame %llu): frame waits %.1f ms (acquire %.1f, queue present %.1f), "
                "submits %.1f ms, fence status %.1f ms, query waits %.1f ms", took / 1e6,
                (unsigned long long)r10_dev->frame, r10_dev->stats.wait_us / 1000.0, r10_dev->stats.acquire_us / 1000.0,
                r10_dev->stats.queue_present_us / 1000.0, r10_dev->stats.submit_us / 1000.0,
                r10_dev->stats.status_us / 1000.0, r10_dev->stats.query_wait_us / 1000.0);
    }
    ++r10_dev->frame;
    void_perf_frame_end((uint32_t)r10_dev->frame);
    r10_frame_presented(r10_dev, r10_dev->be->name);
    ut3_adpf_work_begin();
    return hr;
}
COM_METHOD(sc, GetBuffer, 4)
{
    dxgi_swapchain *s = chain();
    if (!ARG(3)) return E_INVALIDARG_;
    if (ARG(1) || !s->backbuffer) { MEM32(ARG(3)) = 0; return DXGI_ERROR_INVALID_CALL_; }
    void_com_addref(&s->backbuffer->obj.com);
    MEM32(ARG(3)) = s->backbuffer->obj.com.guest;
    return 0;
}
COM_METHOD(sc, SetFullscreenState, 3) { chain()->fullscreen = ARG(1) != 0; return 0; }
COM_METHOD(sc, GetFullscreenState, 3)
{
    dxgi_swapchain *s = chain();
    if (ARG(1)) MEM32(ARG(1)) = s->fullscreen;
    if (ARG(2)) { MEM32(ARG(2)) = s->fullscreen ? output_va : 0; if (s->fullscreen) r10_object_addref_guest(output_va); }
    return 0;
}
COM_METHOD(sc, GetDesc, 2)
{
    dxgi_swapchain *s = chain();
    if (!ARG(1)) return E_INVALIDARG_;
    memcpy(GPTR(ARG(1)), s->desc, 60);
    MEM32(ARG(1) + 48) = !s->fullscreen;   /* Windowed */
    return 0;
}
COM_METHOD(sc, ResizeBuffers, 6)
{
    dxgi_swapchain *s = chain();
    uint32_t w = ARG(2), h = ARG(3), format = ARG(4);
    uint32_t old_w = *(uint32_t *)s->desc, old_h = *(uint32_t *)(s->desc + 4), old_f = *(uint32_t *)(s->desc + 16);
    if (!w || !h) { uint32_t size[2]; display_size(size); if (!w) w = size[0]; if (!h) h = size[1]; }
    if (!format) format = old_f;
    if (s->backbuffer && w == old_w && h == old_h && format == old_f) return 0;
    if (s->backbuffer && s->backbuffer->obj.com.refs > 1) {
        R10_LOG("ResizeBuffers with %d outstanding back buffer references", s->backbuffer->obj.com.refs - 1);
    }
    if (r10_dev && r10_dev->backbuffer == s->backbuffer) r10_dev->backbuffer = NULL;
    if (s->backbuffer) { void_com_release(&s->backbuffer->obj.com); s->backbuffer = NULL; }
    memcpy(s->desc, &w, 4);
    memcpy(s->desc + 4, &h, 4);
    memcpy(s->desc + 16, &format, 4);
    uint32_t buffers = ARG(1);
    if (buffers) memcpy(s->desc + 44, &buffers, 4);
    return make_backbuffer(s) ? 0 : E_OUTOFMEMORY_;
}
COM_METHOD(sc, ResizeTarget, 2) { (void)chain(); return 0; }
COM_METHOD(sc, GetContainingOutput, 2)
{
    (void)chain();
    if (!ARG(1)) return E_INVALIDARG_;
    r10_object_addref_guest(output_va);
    MEM32(ARG(1)) = output_va;
    return 0;
}
COM_METHOD(sc, GetFrameStatistics, 2) { (void)chain(); if (ARG(1)) memset(GPTR(ARG(1)), 0, 32); return 0; }
COM_METHOD(sc, GetLastPresentCount, 2) { dxgi_swapchain *s = chain(); if (ARG(1)) MEM32(ARG(1)) = s->presents; return 0; }
static const void_fn swapchain_methods[] = { OBJECT_METHODS, sc_GetDevice, sc_Present, sc_GetBuffer, sc_SetFullscreenState,
    sc_GetFullscreenState, sc_GetDesc, sc_ResizeBuffers, sc_ResizeTarget, sc_GetContainingOutput, sc_GetFrameStatistics,
    sc_GetLastPresentCount };
static void_com_class swapchain_class = { "IDXGISwapChain", 18, swapchain_methods, 0 };

/* ------------------------------------------------------------------ creation */
static uint32_t make(void_com_class *cls, r10_kind kind, size_t size)
{
    dxgi_object *o = (dxgi_object *)calloc(1, size);
    o->kind = kind;
    return void_com_wrap(&o->com, cls, 0);
}

uint32_t r10_dxgi_factory_create(uint32_t *out)
{
    if (!factory_va) {
        factory_va = make(&factory_class, R10_KIND_FACTORY, sizeof(dxgi_object));
        adapter_va = make(&adapter_class, R10_KIND_ADAPTER, sizeof(dxgi_object));
        output_va = make(&output_class, R10_KIND_OUTPUT, sizeof(dxgi_object));
    } else {
        r10_object_addref_guest(factory_va);
    }
    *out = factory_va;
    return 0;
}

uint32_t r10_swapchain_create(r10_device *d, uint32_t desc_va, uint32_t *out)
{
    (void)d;
    if (!desc_va) return E_INVALIDARG_;
    if (!factory_va) { uint32_t f; r10_dxgi_factory_create(&f); }
    dxgi_swapchain *s = (dxgi_swapchain *)calloc(1, sizeof *s);
    memcpy(s->desc, GPTR(desc_va), 60);
    s->base.kind = R10_KIND_SWAPCHAIN;
    s->fullscreen = !MEM32(desc_va + 48);
    if (!make_backbuffer(s)) { free(s); return E_OUTOFMEMORY_; }
    void_com_wrap(&s->base.com, &swapchain_class, 0);
    s->base.com.destroy = chain_destroy;
    *out = s->base.com.guest;
    return 0;
}

WINAPI_EXPORT("dxgi.dll", CreateDXGIFactory, 2)
{
    if (!ARG(1)) return E_INVALIDARG_;
    return r10_dxgi_factory_create((uint32_t *)GPTR(ARG(1)));
}

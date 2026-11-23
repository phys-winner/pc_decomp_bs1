/* The port's render scale on BioShock's Direct3D 10 renderer, and the few Direct3D 9 / D3DX9 imports that
 * game code outside the Direct3D 9 renderer still reaches.
 *
 * Render scale (port setting "renderScale", 50-100 %): FD3DSceneRenderInterface10's scene begin
 * (sub_109B6FA0) shrinks the scene viewport inside the unchanged full-size HDR targets; the final
 * tonemap quad of its scene end (sub_109A66D0 at 0x109B7B69, a seam of tools/build_android.py) gets the
 * full output size, so it upscales the smaller scene into the back buffer before the HUD; the end
 * (sub_109B75A0) restores the viewport and re-applies it (sub_109A47D0) for the Flash HUD.  This is
 * the Direct3D 9 hook of bioshock_render.c with the offsets of the D3D10 classes
 * (research/d3d10-census.md: the scene object's +48 viewport and +52 render device, the render
 * device's back buffer size at +548/+552). */
#include "host.h"
#include <math.h>

extern void sub_109B6FA0(void);
extern void sub_109B75A0(void);
extern void sub_109A66D0(void);

static int scene_percent = 100;
static int upscaler_mode;
static struct {
    uint32_t owner, renderer, viewport, dimensions[4], scaled[2];
    unsigned reported;
} scene;
/* The native renderer's AMD FSR1 (android/render10). */
extern int r10_upscale_available(void) __attribute__((weak));
extern int r10_upscale_scene(uint32_t sw, uint32_t sh) __attribute__((weak));
/* The scene area while the scaled scene draws (passes on full-size targets keep to it). */
extern void r10_scene_area(uint32_t w, uint32_t h, uint32_t full_w, uint32_t full_h) __attribute__((weak));
static int scene_resolving;

void void_bioshock_render_scale(int percent)
{
    if (percent >= 50 && percent <= 100 && percent % 10 == 0)
        __atomic_store_n(&scene_percent, percent, __ATOMIC_RELEASE);
}

/* 0 off (bilinear), 1 AMD FSR1. */
void void_bioshock_upscaler(int mode)
{
    if (mode >= 0 && mode <= 1 && __atomic_exchange_n(&upscaler_mode, mode, __ATOMIC_ACQ_REL) != mode)
        void_log("[render] upscaler %s", mode ? "FSR1" : "off");
}
int void_bioshock_upscaler_mode(void) { return __atomic_load_n(&upscaler_mode, __ATOMIC_ACQUIRE); }
int void_bioshock_scene_resolving(void) { return scene_resolving; }
int void_bioshock_scene_percent(void) { return scene.owner ? __atomic_load_n(&scene_percent, __ATOMIC_ACQUIRE) : 100; }

static int render_member(uint32_t address, uint32_t bytes)
{
    return address >= 0x10000u && address <= UINT32_MAX - bytes && void_vm_writable(address, bytes);
}

static void scene_begin(void)
{
    uint32_t owner = g_ecx;
    int percent = __atomic_load_n(&scene_percent, __ATOMIC_ACQUIRE);
    if (!scene.owner && percent < 100 && render_member(owner, 56) && !MEM32(owner + 4) &&
        void_vm_readable(g_esp, 8) && MEM32(g_esp + 4)) {
        uint32_t viewport = MEM32(owner + 48), renderer = MEM32(owner + 52);
        if (render_member(viewport, 36) && render_member(renderer, 568) &&
            !MEM32(viewport + 12) && !MEM32(viewport + 16) &&
            MEM32(viewport + 20) == MEM32(renderer + 548) &&
            MEM32(viewport + 24) == MEM32(renderer + 552) &&
            MEM32(viewport + 20) >= 8 && MEM32(viewport + 24) >= 8 &&
            MEM32(viewport + 20) <= 8192 && MEM32(viewport + 24) <= 8192) {
            scene.owner = owner; scene.renderer = renderer; scene.viewport = viewport;
            memcpy(scene.dimensions, GPTR(viewport + 12), sizeof scene.dimensions);
            uint32_t w = scene.dimensions[2] * (uint32_t)percent / 100;
            uint32_t h = scene.dimensions[3] * (uint32_t)percent / 100;
            MEM32(viewport + 20) = w;
            MEM32(viewport + 24) = h;
            scene.scaled[0] = w; scene.scaled[1] = h;
            if (r10_scene_area) r10_scene_area(w, h, scene.dimensions[2], scene.dimensions[3]);
            if (scene.reported != (unsigned)percent) {
                void_log("[render] scene scale %d%%: %ux%u -> %ux%u; HUD %ux%u", percent,
                    scene.dimensions[2], scene.dimensions[3], w, h, scene.dimensions[2], scene.dimensions[3]);
                scene.reported = (unsigned)percent;
            }
        }
    }
    sub_109B6FA0();
}

/* The seam at 0x109B7B69: the scene end's final tonemap quad, before the HUD.  Bilinear: the quad gets the
 * full output size and stretches the scene.  FSR1: the quad keeps the scene's size (the top-left of the
 * back buffer) and the native renderer upscales that region into the whole back buffer. */
void void_bioshock_resolve_quad10(void)
{
    int ours = scene.owner && scene_resolving && g_ecx == scene.viewport;
    int fsr = ours && __atomic_load_n(&upscaler_mode, __ATOMIC_ACQUIRE) >= 1 && r10_upscale_available && r10_upscale_scene &&
              r10_upscale_available();
    if (ours && !fsr && void_vm_writable(g_esp, 68)) {
        MEM32(g_esp + 28) = scene.dimensions[2];
        MEM32(g_esp + 32) = scene.dimensions[3];
    }
    if (ours && r10_scene_area) r10_scene_area(0, 0, 0, 0);   /* the quad writes the whole back buffer */
    int previous = scene_resolving;
    scene_resolving = 2;
    sub_109A66D0();
    scene_resolving = previous;
    if (fsr) {
        static int reported = -1;
        int done = r10_upscale_scene(scene.scaled[0], scene.scaled[1]);
        if (reported != done) {
            void_log("[render] FSR1 %ux%u -> %ux%u %s", scene.scaled[0], scene.scaled[1], scene.dimensions[2], scene.dimensions[3],
                     done ? "upscaled" : "FAILED");
            reported = done;
        }
    }
}

static void scene_end(void)
{
    uint32_t owner = g_ecx;
    scene_resolving = 1;
    sub_109B75A0();
    scene_resolving = 0;
    if (r10_scene_area) r10_scene_area(0, 0, 0, 0);
    if (scene.owner != owner) return;
    memcpy(GPTR(scene.viewport + 12), scene.dimensions, sizeof scene.dimensions);
    /* The scene end restores its saved smaller viewport; the full one is applied before the HUD draws. */
    void_regs saved;
    void_regs_save(&saved);
    (void)void_call_guest_this(0x109A47D0u, scene.viewport, 0, NULL);
    void_regs_load(&saved);
    scene.owner = 0;
}

void void_bioshock_render_register(void)
{
    if (!void_dispatch_find(0x109B6FA0u) || !void_dispatch_find(0x109B75A0u))
        void_fatal("original D3D10 scene render interface is not lifted");
    void_dispatch_add(0x109B6FA0u, scene_begin);
    void_dispatch_add(0x109B75A0u, scene_end);
}

/* ------------------------------------------------------------------ Direct3D 9 and D3DX9 leftovers */
/* The Direct3D 9 renderer is not used: Direct3DCreate9 fails, so the engine's DX9 probe reports no
 * Direct3D 9 (the renderer of the profile is D3DDrv10.D3DRenderDevice10). */
WINAPI_EXPORT("d3d9.dll", Direct3DCreate9, 1)
{
    void_log("[render] Direct3DCreate9(%u): the port renders with Direct3D 10 only", ARG(0));
    return 0;
}

/* BioShock's August 2007 D3DX33 imports outside the Direct3D 9 renderer. */
WINAPI_EXPORT("d3dx9_33.dll", D3DXVec3Normalize, 2)
{
    uint32_t out = ARG(0), in = ARG(1);
    if (!out || !in || !void_vm_readable(in, 12) || !void_vm_writable(out, 12)) return 0;
    float v[3];
    memcpy(v, GPTR(in), sizeof v);
    float length = sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    if (length == 0.0f || !isfinite(length)) {
        memset(GPTR(out), 0, 12);
        return 0;
    }
    float scale = 1.0f / length;
    for (unsigned i = 0; i < 3; ++i) v[i] *= scale;
    memcpy(GPTR(out), v, sizeof v);
    return out;
}

WINAPI_EXPORT("d3dx9_33.dll", D3DXFloat16To32Array, 3)
{
    uint32_t out = ARG(0), in = ARG(1), count = ARG(2);
    if (count && (!out || !in || !void_vm_readable(in, count * 2u) || !void_vm_writable(out, count * 4u)))
        return 0;
    for (uint32_t i = 0; i < count; ++i) {
        uint32_t half = MEM16(in + 2u * i), sign = (half & 0x8000u) << 16;
        uint32_t exponent = (half >> 10) & 31u, mantissa = half & 1023u, value;
        if (!exponent) {
            if (!mantissa) value = sign;
            else {
                int adjusted = -14;
                while (!(mantissa & 0x400u)) { mantissa <<= 1; --adjusted; }
                mantissa &= 1023u;
                value = sign | ((uint32_t)(adjusted + 127) << 23) | (mantissa << 13);
            }
        } else {
            value = sign | ((exponent + 112u) << 23) | (mantissa << 13);
        }
        MEM32(out + 4u * i) = value;
    }
    return out;
}

/* The seam of the Direct3D 9 renderer's tonemap (tools/lift_game.py, 0x10D81097) stays in its lifted,
 * never reached code; it calls the original. */
extern void sub_10D72190(void);
void void_bioshock_resolve_quad(void) { sub_10D72190(); }

/* Original Flash HUD on screens wider than 4:3.
 *
 * BioShock's gameswf viewport rule (10A658A0, branch 10A65925) stretches every
 * 1280x720 stage onto a screen wider than 4:3 with set_scale(1,1): at
 * 2780x1264 the HUD is 1.237 times too wide (the round reticle, the gauges).
 * Lifted call-site overrides (tools/lift_game.py: 10A78FAD open, 10A749C1
 * resize) call void_bioshock_flash_viewport after the original setup and the
 * movie's own SetRes ran. A movie whose SetRes declares PortAspectLayout (the
 * patched HUDPC/HUDRadial) gets the original 4:3 mechanism generalised: the
 * uniform 720/h scale, the matching mouse scale, and PortHudLayout(width,
 * uiScale, clear) re-anchors its groups to the screen edges. Every other
 * movie, and an unpatched HUD, keeps the original behaviour. */
#include "host.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

extern int void_android_port_setting(const char *key, const char *value, char *out, size_t cap) __attribute__((weak));
extern uint32_t void_bioshock_named_movie(const char *name);

static float port_float(const char *key, float fallback, float minimum, float maximum)
{
    char out[32];
    if (!void_android_port_setting || void_android_port_setting(key, NULL, out, sizeof out)) return fallback;
    float value = strtof(out, NULL);
    return isfinite(value) ? fminf(maximum, fmaxf(minimum, value)) : fallback;
}

typedef struct {
    void_regs regs;
#ifdef RECOMP_X87_WINDOW
    double window[RECOMP_X87_SLOTS];
    int64_t integers[RECOMP_X87_SLOTS];
    int sp;
#endif
} guest_state;

static void state_save(guest_state *s)
{
    void_regs_save(&s->regs);
#ifdef RECOMP_X87_WINDOW
    s->sp = g_fp_sp;
    memcpy(s->window, g_st_win, sizeof s->window);
    memcpy(s->integers, g_st_i64_win, sizeof s->integers);
#endif
}

static void state_load(const guest_state *s)
{
#ifdef RECOMP_X87_WINDOW
    g_fp_sp = s->sp;
    memcpy(g_st_win, s->window, sizeof s->window);
    memcpy(g_st_i64_win, s->integers, sizeof s->integers);
#endif
    void_regs_load(&s->regs);
}

/* QA switches (perf-flags): BIOSHOCK_HUD_ASPECT=0 keeps the original stage
 * scale, BIOSHOCK_HUD_LAYOUT=0 skips PortHudLayout, BIOSHOCK_HUD_TRACE=1 logs
 * layout variables read back from the movie. */
static int hud_flag(const char *name, int fallback)
{
    const char *value = getenv(name);
    return value && *value ? *value == '1' : fallback;
}

/* movie_root vtbl[22] get_variable (10BF6690) as text, or NULL. */
static int hud_variable(uint32_t root, const char *path, char *out, size_t cap)
{
    uint32_t name = void_gstrdup(path);
    if (!name) return 0;
    uint32_t args[1] = {name};
    uint32_t text = void_call_guest_this(0x10BF6690u, root, 1, args);
    void_gfree(name);
    size_t i = 0;
    if (text >= 0x10000u)
        for (; i + 1 < cap && void_vm_readable(text + (uint32_t)i, 1) && MEM8(text + (uint32_t)i); ++i)
            out[i] = (char)MEM8(text + (uint32_t)i);
    out[i] = 0;
    return text >= 0x10000u;
}

static void hud_trace(uint32_t root)
{
    static const char *const paths[] = {"PortStageWidth", "PortHudScale", "PortHudClear", "_visible", "_x",
        "healthBar._x", "healthBar._xscale", "healthBar._visible", "maxHealth", "healthBarWidth", "HealthBGMidStartWidth",
        "healthBar.HealthBGMid._x", "healthBar.HealthBGMid._width", "healthBar.HealthBGMid._xscale",
        "healthBar.HealthBGCapEnd._x", "healthBar.healthBar_mc._x", "healthBar.healthBar_mc._width",
        "healthBar.healthBar_mc._xscale", "bioAmmoBar.BioAmmoBGMid._width", "bioAmmoBar.BioAmmoBGCapEnd._x"};
    char line[2048], value[64];
    int used = snprintf(line, sizeof line, "[flash-aspect] trace");
    for (unsigned i = 0; i < sizeof paths / sizeof *paths && used < (int)sizeof line - 96; ++i) {
        if (!hud_variable(root, paths[i], value, sizeof value)) strcpy(value, "?");
        used += snprintf(line + used, sizeof line - (size_t)used, " %s=%s", paths[i], value);
    }
    void_log("%s", line);
}

/* movie_root vtbl[22] get_variable (10BF6690): "1" only from the patched SetRes. */
static int aspect_aware(uint32_t root)
{
    uint32_t path = void_gstrdup("PortAspectLayout");
    if (!path) return 0;
    uint32_t args[1] = {path};
    uint32_t text = void_call_guest_this(0x10BF6690u, root, 1, args);
    int aware = text >= 0x10000u && void_vm_readable(text, 2) && MEM8(text) == '1' && MEM8(text + 1) == 0;
    void_gfree(path);
    return aware;
}

/* Original movie CallMethod (10A66CD0, cdecl, printf-style argument list). */
static void hud_layout_call(uint32_t movie, const char *width, const char *scale, const char *clear, const char *mode)
{
    uint32_t name = void_gstrdup("PortHudLayout"), format = void_gstrdup("%s,%s,%s,%s");
    uint32_t a = void_gstrdup(width), b = void_gstrdup(scale), c = void_gstrdup(clear), d = void_gstrdup(mode);
    if (name && format && a && b && c && d) {
        uint32_t args[] = {movie, name, format, a, b, c, d};
        (void)void_call_guest(0x10A66CD0u, 7, args, 0);
    }
    if (d) void_gfree(d);
    if (c) void_gfree(c);
    if (b) void_gfree(b);
    if (a) void_gfree(a);
    if (format) void_gfree(format);
    if (name) void_gfree(name);
}

/* Stage geometry of a movie, or 0 when it is not a live aspect-aware stage. */
static int hud_stage(uint32_t movie, uint32_t *root_out, uint32_t *def_out, uint32_t *w_out, uint32_t *h_out)
{
    if (movie < 0x10000u || movie > UINT32_MAX - 272 || !void_vm_readable(movie, 272)) return 0;
    uint32_t root = MEM32(movie + 268), w = MEM32(movie + 104), h = MEM32(movie + 108);
    if (root < 0x10000u || root > UINT32_MAX - 36 || !void_vm_readable(root, 36) || !w || !h || w > 16384 || h > 16384)
        return 0;
    uint32_t def = MEM32(root + 8);
    if (def < 0x10000u || def > UINT32_MAX - 124 || !void_vm_readable(def + 116, 8)) return 0;
    *root_out = root; *def_out = def; *w_out = w; *h_out = h;
    return 1;
}

static void hud_relayout_locked(uint32_t movie, uint32_t w, uint32_t h)
{
    char width[32] = "0", scale[16], clear[16];
    if ((uint64_t)h * 4 < (uint64_t)w * 3) snprintf(width, sizeof width, "%.3f", 720.0 * (double)w / (double)h);
    snprintf(scale, sizeof scale, "%.3f", (double)port_float("uiScale", 1.0f, 1.0f, 1.5f));
    snprintf(clear, sizeof clear, "%.3f", (double)port_float("hudClear", 0.0f, 0.0f, 400.0f));
    /* Mode 1 moves the whole stage for the Pause clearance: the original HUD
     * script measures its gauges' own x, so they keep it. QA overrides:
     * BIOSHOCK_HUD_SCALE, BIOSHOCK_HUD_CLEAR, BIOSHOCK_HUD_SHIFTMODE. */
    const char *mode = hud_flag("BIOSHOCK_HUD_SHIFTMODE", 1) ? "1" : "0";
    const char *value = getenv("BIOSHOCK_HUD_SCALE");
    if (value && *value) snprintf(scale, sizeof scale, "%.3f", strtod(value, NULL));
    value = getenv("BIOSHOCK_HUD_CLEAR");
    if (value && *value) snprintf(clear, sizeof clear, "%.3f", strtod(value, NULL));
    hud_layout_call(movie, width, scale, clear, mode);
}

void void_bioshock_flash_viewport(uint32_t movie)
{
    uint32_t root, def, w, h;
    if (!hud_stage(movie, &root, &def, &w, &h)) return;
    guest_state saved;
    state_save(&saved);
    if (aspect_aware(root)) {
        if ((uint64_t)h * 4 < (uint64_t)w * 3 && hud_flag("BIOSHOCK_HUD_ASPECT", 1)) {
            /* The original wide branch stored set_scale(1,1) and mouse scale
             * 1280/w; generalise its 4:3 set_scale(0.75,1): the visible stage
             * is [0, 720*w/h] at the uniform 720/h scale (16:9 keeps 1.0). */
            float sx = 720.0f * (float)w / (1280.0f * (float)h), mouse;
            memcpy(GPTR(def + 116), &sx, sizeof sx);   /* the field vtbl[18] 10BF64A0 writes */
            memcpy(&mouse, GPTR(movie + 248), sizeof mouse);
            mouse *= sx;                                 /* 1280/w -> 720/h */
            memcpy(GPTR(movie + 248), &mouse, sizeof mouse);
        }
        if (hud_flag("BIOSHOCK_HUD_LAYOUT", 1)) hud_relayout_locked(movie, w, h);
        if (hud_flag("BIOSHOCK_HUD_TRACE", 0)) hud_trace(root);
        static unsigned logged;
        if (logged++ < 8) void_log("[flash-aspect] movie %08X %ux%u: uniform HUD stage, layout applied", movie, w, h);
    }
    state_load(&saved);
}

/* QA: the live HUD's layout and gauge variables, once gameplay shows it. */
void void_bioshock_flash_trace(void)
{
    if (!hud_flag("BIOSHOCK_HUD_TRACE", 0)) return;
    uint32_t movie = void_bioshock_named_movie("HUD"), root, def, w, h;
    if (!hud_stage(movie, &root, &def, &w, &h)) return;
    guest_state saved;
    state_save(&saved);
    hud_trace(root);
    state_load(&saved);
}

/* A UI scale change applies to the live HUD at once (settings bridge). */
void void_bioshock_flash_relayout(void)
{
    uint32_t movie = void_bioshock_named_movie("HUD"), root, def, w, h;
    if (!hud_stage(movie, &root, &def, &w, &h)) return;
    guest_state saved;
    state_save(&saved);
    if (aspect_aware(root)) hud_relayout_locked(movie, w, h);
    state_load(&saved);
}

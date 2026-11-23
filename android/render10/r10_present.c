/* Frame pacing and statistics shared by the native backends.
 *
 * The limiter waits for a deadline with clock_nanosleep and spins the last 150 us (the accepted
 * Android limiter of the former DXVK path, sleep_threshold.inc): a sleep that overshoots by
 * the scheduler's wake-up latency would otherwise miss the display's next vblank.  The cap follows
 * the game's settings at run time (d3d_gl_set_frame_limit, the hook android_main.c calls). */
#include "r10.h"
#include <errno.h>
#include <malloc.h>
#include <math.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

static _Atomic float frame_limit = 60.0f;
static uint64_t frame_deadline_ns, frame_period_ns;

uint64_t r10_now_ns(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec;
}

void d3d_gl_set_frame_limit(float fps)
{
    if (isfinite(fps) && fps >= 1.0f && fps <= 1000.0f) frame_limit = fps;
}

float r10_frame_limit(void)
{
    static int loaded;
    if (!loaded) {
        loaded = 1;
        const char *text = getenv("UT3_FPS_LIMIT");
        if (text) {
            char *end;
            float v = strtof(text, &end);
            if (end != text && !*end && isfinite(v) && v >= 1 && v <= 1000) frame_limit = v;
        }
    }
    return frame_limit;
}

int r10_env_int(const char *name, int fallback, int lo, int hi)
{
    const char *text = getenv(name);
    if (!text || !*text) return fallback;
    char *end;
    long v = strtol(text, &end, 10);
    return *end || v < lo || v > hi ? fallback : (int)v;
}

/* FIFO paces at the display rate; a cap below it is the limiter's work.  The swap interval stays 1
 * so a cap at or above the display rate never tears. */
int r10_swap_interval(void) { return 1; }

r10_scene_area_t r10_area;

/* BIOSHOCK_R10_DEBUG bit 32768 keeps whole targets (A/B of the scene area). */
void r10_scene_area(uint32_t w, uint32_t h, uint32_t full_w, uint32_t full_h)
{
    static int off = -1;
    if (off < 0) { const char *e = getenv("BIOSHOCK_R10_DEBUG"); off = e && (atoi(e) & 32768) ? 1 : 0; }
    if (off || w >= full_w || h >= full_h) w = h = full_w = full_h = 0;
    r10_area.w = w; r10_area.h = h; r10_area.full_w = full_w; r10_area.full_h = full_h;
}

/* A whole copy between full-size targets while the scaled scene draws needs only the scene area: the game
 * copies its depth buffer twice per frame, 1.75 ms each at 2400x1080 on the Adreno 618. */
const uint32_t *r10_area_copy_box(const r10_resource *dst, uint32_t dst_sub, uint32_t x, uint32_t y, uint32_t z,
                                  const r10_resource *src, uint32_t src_sub, const uint32_t *box, uint32_t out[6])
{
    if (box || !r10_area.w || x || y || z || src_sub || dst_sub || src->desc.width != r10_area.full_w ||
        src->desc.height != r10_area.full_h || dst->desc.width != r10_area.full_w || dst->desc.height != r10_area.full_h)
        return box;
    out[0] = 0; out[1] = 0; out[2] = 0; out[3] = r10_area.w; out[4] = r10_area.h; out[5] = 1;
    return out;
}

/* The game's own cap (bioshock_tweaks.c): 60 FPS while a cutscene runs, 0 otherwise. */
extern float void_bioshock_frame_cap(void) __attribute__((weak));

void r10_pace_frame(void)
{
    float fps = r10_frame_limit();
    float cap = void_bioshock_frame_cap ? void_bioshock_frame_cap() : 0.0f;
    if (cap > 0.0f && (fps <= 0.0f || fps > cap)) fps = cap;
    if (fps <= 0.0f) { frame_deadline_ns = 0; return; }
    uint64_t period = (uint64_t)(1000000000.0 / (double)fps);
    uint64_t now = r10_now_ns();
    if (period != frame_period_ns || !frame_deadline_ns || now >= frame_deadline_ns + period) {
        frame_deadline_ns = now;
        frame_period_ns = period;
    }
    if (now < frame_deadline_ns) {
        const uint64_t spin = 150000;
        if (frame_deadline_ns - now > spin) {
            uint64_t wake = frame_deadline_ns - spin;
            struct timespec deadline = { (time_t)(wake / 1000000000ull), (long)(wake % 1000000000ull) };
            void_blocking_begin();
            int error;
            do { error = clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &deadline, NULL); } while (error == EINTR);
            void_blocking_end();
        }
        while ((now = r10_now_ns()) < frame_deadline_ns) __asm__ volatile("yield");
    }
    /* A slow frame or a resume never makes a catch-up burst. */
    frame_deadline_ns = now >= frame_deadline_ns + period ? now + period : frame_deadline_ns + period;
}

extern int void_bioshock_anisotropy_level(void) __attribute__((weak));

/* "16x anisotropic filtering" of garamond13's BioshockGraphicalUpgrade (an idea, no code): a level of
 * 4, 8 or 16 makes every sampler with linear minification and magnification that is not a comparison
 * (shadow) sampler anisotropic (D3D10_FILTER_ANISOTROPIC, trilinear), at least at that level. 0 keeps
 * the game's own samplers (this profile's LevelOfAnisotropy and UseTrilinear). */
uint32_t r10_sampler_filter(const r10_sampler_desc *sd, uint32_t *max_anisotropy)
{
    uint32_t f = sd->filter, a = sd->max_anisotropy ? sd->max_anisotropy : 1;
    int level = void_bioshock_anisotropy_level ? void_bioshock_anisotropy_level() : 0;
    if (level > 1 && !(f & 0x80) && (f & 0x14) == 0x14) {
        static int logged;
        if (!logged) {
            logged = 1;
            R10_LOG("anisotropy %dx for the game's linear samplers (first: filter %#x, %u)", level, f, a);
        }
        f = 0x55;
        if (a < (uint32_t)level) a = (uint32_t)level;
    }
    *max_anisotropy = a > 16 ? 16 : a;
    return f;
}

static uint32_t f32_bits(float f) { uint32_t u; memcpy(&u, &f, 4); return u; }

void r10_fsr_constants(uint32_t easu[16], uint32_t rcas[4], float vw, float vh, float iw, float ih, float ow, float oh,
                       float sharpness)
{
    /* ffx_fsr1.h FsrEasuCon */
    easu[0] = f32_bits(vw / ow); easu[1] = f32_bits(vh / oh);
    easu[2] = f32_bits(0.5f * vw / ow - 0.5f); easu[3] = f32_bits(0.5f * vh / oh - 0.5f);
    easu[4] = f32_bits(1.0f / iw); easu[5] = f32_bits(1.0f / ih);
    easu[6] = f32_bits(1.0f / iw); easu[7] = f32_bits(-1.0f / ih);
    easu[8] = f32_bits(-1.0f / iw); easu[9] = f32_bits(2.0f / ih);
    easu[10] = f32_bits(1.0f / iw); easu[11] = f32_bits(2.0f / ih);
    easu[12] = f32_bits(0.0f); easu[13] = f32_bits(4.0f / ih);
    /* Not read by FsrEasuF: the last texel of the input region for the fetch callbacks (android/render10). */
    easu[14] = vw >= 1.0f ? (uint32_t)vw - 1u : 0u;
    easu[15] = vh >= 1.0f ? (uint32_t)vh - 1u : 0u;
    /* FsrRcasCon: the float kernel reads con.x = 2^-sharpness */
    rcas[0] = f32_bits(exp2f(-sharpness)); rcas[1] = rcas[2] = rcas[3] = 0;
}

int r10_upscale_available(void)
{
    return r10_dev && r10_dev->be && r10_dev->be->upscale && r10_dev->be->upscale(r10_dev, NULL, 0, 0, 0.0f);
}

int r10_upscale_scene(uint32_t sw, uint32_t sh)
{
    if (!r10_dev || !r10_dev->be || !r10_dev->be->upscale || !r10_dev->backbuffer || !sw || !sh) return 0;
    return r10_dev->be->upscale(r10_dev, r10_dev->backbuffer, sw, sh, 0.2f);
}

void r10_fit(uint32_t sw, uint32_t sh, uint32_t dw, uint32_t dh, int32_t *x, int32_t *y, int32_t *w, int32_t *h)
{
    if (!sw || !sh || !dw || !dh) { *x = *y = 0; *w = (int32_t)dw; *h = (int32_t)dh; return; }
    uint64_t a = (uint64_t)dw * sh, b = (uint64_t)dh * sw;
    if (a > b) {   /* window wider than the image: bars left and right */
        *h = (int32_t)dh;
        *w = (int32_t)((uint64_t)dh * sw / sh);
    } else {
        *w = (int32_t)dw;
        *h = (int32_t)((uint64_t)dw * sh / sw);
    }
    /* Within one percent the picture fills the window (the render size follows the display). */
    if (*w >= (int32_t)(dw - dw / 100) && *h >= (int32_t)(dh - dh / 100)) { *w = (int32_t)dw; *h = (int32_t)dh; }
    *x = ((int32_t)dw - *w) / 2;
    *y = ((int32_t)dh - *h) / 2;
}

extern void void_android_frame_presented(void) __attribute__((weak));
extern void void_android_load_ready(void) __attribute__((weak));
extern uint32_t void_vm_committed_kb(void) __attribute__((weak));
extern void ut3_openal_log_stats(void) __attribute__((weak));

void r10_note_present(void)
{
    if (void_android_frame_presented) void_android_frame_presented();
}

/* Every 30 s with the statistics: the backend's GPU memory, the guest's committed memory, the resources' guest
 * shadows and the host heap, so a session log shows where the process's memory goes. */
static void memory_line(r10_device *d, uint64_t now)
{
    static uint64_t logged_ns;
    if (logged_ns && now - logged_ns < 30000000000ull) return;
    logged_ns = now;
    char gpu[640] = "";
    if (d->be && d->be->memory_line) d->be->memory_line(gpu, sizeof gpu);
    struct mallinfo mi = mallinfo();
    R10_LOG("memory: %s%sguest committed %u MiB, resource shadows %.1f MiB (%u), host heap %.1f MiB", gpu, gpu[0] ? "; " : "",
            void_vm_committed_kb ? void_vm_committed_kb() >> 10 : 0, (double)r10_shadow_bytes / 1048576.0, r10_shadow_count,
            (double)mi.uordblks / 1048576.0);
}

/* Statistics every 600 frames or 5 s, in the format of the GLES path's [gl] line. "ms cpu" is the presenting
 * thread's own CPU time; -1 when the interval began on another thread (a garbage difference before). */
void r10_frame_presented(r10_device *d, const char *backend)
{
    static uint64_t start_ns, start_frame, cpu_start_ns, draws, maps, updates, submits, wait_us, gpu_us, gpu_frames, query_us, submit_us, status_us, sets, set_writes, draw_ns, set_ns, present_ns, acquire_us, qpresent_us;
    static pid_t cpu_tid;   /* the thread whose CPU clock cpu_start_ns read: guest threads can take turns presenting */
    uint64_t now = r10_now_ns();
    static uint64_t last_audio_stats;
    if (ut3_openal_log_stats && now - last_audio_stats >= UINT64_C(5000000000)) {
        last_audio_stats = now;
        ut3_openal_log_stats();
    }
    struct timespec cpu;
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &cpu);
    uint64_t cpu_ns = (uint64_t)cpu.tv_sec * 1000000000ull + (uint64_t)cpu.tv_nsec;
    pid_t tid = gettid();
    draws += d->stats.draws; maps += d->stats.maps; updates += d->stats.updates; submits += d->stats.submits; wait_us += d->stats.wait_us; gpu_us += d->stats.gpu_us; gpu_frames += d->stats.gpu_frames; query_us += d->stats.query_wait_us; submit_us += d->stats.submit_us; status_us += d->stats.status_us; sets += d->stats.sets; set_writes += d->stats.set_writes; draw_ns += d->stats.draw_ns; set_ns += d->stats.set_ns; present_ns += d->stats.present_ns; acquire_us += d->stats.acquire_us; qpresent_us += d->stats.queue_present_us;
    uint64_t frames = d->frame - start_frame;
    /* The shadows of cold static vertex buffers go every 300 frames (r10_objects.c); <data>/r10-memstat-now
     * (adb shell touch) logs the memory census once. */
    if (!(d->frame % 300)) r10_shadow_sweep(d);
    if (!(d->frame & 63)) {
        char path[1200];
        snprintf(path, sizeof path, "%s/r10-memstat-now", void_data_dir() ? void_data_dir() : ".");
        if (!unlink(path)) r10_memory_census(d);
    }
    if (!start_ns || frames >= 600 || now - start_ns >= 5000000000ull) {
        if (start_ns && frames) {
            double dt = (double)(now - start_ns) / 1e9;
            R10_LOG("%s frame %llu: %.1f fps; per frame %.2f ms wall, %.2f ms cpu, %.0f draws, %.0f maps, %.0f updates, %u passes, "
                    "%u pipelines, %u KiB uploaded, %.1f submits (%.2f ms), %.2f ms waits (acquire %.2f, present %.2f), %.2f ms query waits, %.2f ms fence status, %.1f sets (%.1f writes, %.2f ms), draws %.2f ms, present %.2f ms, %.2f ms gpu; shaders %u (%u in library), compiles %u (%u missing, %u by fallback); engine lookups %u by cache identity, %u by record, %u missing (%u by fallback), ShaderCache.pcs10 loads skipped %u, from Direct3D bytecode %u", backend, (unsigned long long)d->frame, frames / dt, 1000.0 * dt / frames,
                    tid == cpu_tid && cpu_ns >= cpu_start_ns ? (double)(cpu_ns - cpu_start_ns) / 1e6 / frames : -1.0,
                    (double)draws / frames, (double)maps / frames,
                    (double)updates / frames, d->stats.passes, d->stats.pipelines, d->stats.uploads_kb, (double)submits / frames, (double)submit_us / 1000.0 / frames,
                    (double)wait_us / 1000.0 / frames, (double)acquire_us / 1000.0 / frames, (double)qpresent_us / 1000.0 / frames, (double)query_us / 1000.0 / frames, (double)status_us / 1000.0 / frames, (double)sets / frames, (double)set_writes / frames, (double)set_ns / 1e6 / frames, (double)draw_ns / 1e6 / frames, (double)present_ns / 1e6 / frames, gpu_frames ? (double)gpu_us / 1000.0 / gpu_frames : 0.0,
                    d->shader_stats.created, d->shader_stats.found, d->shader_stats.compiles, d->shader_stats.compile_misses, d->shader_stats.compile_fallbacks,
                    d->shader_stats.native_identity, d->shader_stats.native_record, d->shader_stats.native_misses,
                    d->shader_stats.native_fallbacks, d->shader_stats.cache_loads_skipped, d->shader_stats.from_dxbc);
            memory_line(d, now);
        }
        start_ns = now;
        start_frame = d->frame;
        cpu_start_ns = cpu_ns;
        cpu_tid = tid;
        draws = maps = updates = submits = wait_us = gpu_us = gpu_frames = query_us = submit_us = status_us = sets = set_writes = draw_ns = set_ns = present_ns = acquire_us = qpresent_us = 0;
    }
    d->stats.draws = d->stats.maps = d->stats.updates = d->stats.submits = d->stats.wait_us = d->stats.gpu_us = d->stats.gpu_frames = d->stats.query_wait_us = d->stats.submit_us = d->stats.status_us = d->stats.sets = d->stats.set_writes = d->stats.draw_ns = d->stats.set_ns = d->stats.present_ns = d->stats.acquire_us = d->stats.queue_present_us = 0;
    d->stats.passes = 0;
    d->stats.uploads_kb = 0;
}

"""P0: the GLES present must swap with its context current after the frame limiter slept.

r10_pace_frame() waits for the FPS deadline inside void_blocking_begin(), which gives up the guest
machine; threads.c then releases the GL context of that thread (d3d_gl_thread_release). EGL 1.5
3.10.3: swapping a surface that is not bound to the calling thread's current context is
EGL_BAD_SURFACE. The frame was lost and every failure recreated the window surface (RMX3371: 463
failures in 60 s, POCO X3 NFC: 662). The production gl_current, d3d_gl_thread_release,
follow_window and gles_present of android/render10/r10_gles.c run here against an EGL model that
follows that rule.
"""
from pathlib import Path
import subprocess
import unittest

from tests.host_environment import CLANG, environment
from tests.test_mobile_shaders import function

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / "build/tests/gles-present-context"

HARNESS = r'''
#define _CRT_SECURE_NO_WARNINGS
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
typedef unsigned GLuint, GLenum; typedef int GLint; typedef float GLfloat; typedef unsigned char GLboolean;
typedef void *EGLDisplay, *EGLContext, *EGLSurface, *EGLConfig, *EGLNativeWindowType;
typedef int EGLint; typedef unsigned EGLBoolean;
#define EGL_NO_DISPLAY ((EGLDisplay)0)
#define EGL_NO_CONTEXT ((EGLContext)0)
#define EGL_NO_SURFACE ((EGLSurface)0)
#define EGL_WIDTH 0x3057
#define EGL_HEIGHT 0x3056
#define EGL_SUCCESS 0x3000
#define EGL_BAD_SURFACE 0x300D
#define GL_TRUE 1
#define GL_DRAW_FRAMEBUFFER 0x8CA9
#define GL_SCISSOR_TEST 0x0C11
#define GL_DEPTH_TEST 0x0B71
#define GL_STENCIL_TEST 0x0B90
#define GL_CULL_FACE 0x0B44
#define GL_BLEND 0x0BE2
#define GL_COLOR_BUFFER_BIT 0x4000
#define GL_TEXTURE0 0x84C0
#define GL_TEXTURE_2D 0x0DE1
#define GL_TEXTURE_MIN_FILTER 0x2801
#define GL_TEXTURE_MAG_FILTER 0x2800
#define GL_LINEAR 0x2601
#define GL_TRIANGLES 0x0004
#define DXGI_STATUS_OCCLUDED_ 0x087A0001u
#define R10_DIRTY_ALL 0xFFFFFFFFu
static unsigned logs, bad_surface_logs, new_surfaces, fatals;
#define R10_LOG(...) (++logs, printf("[r10] " __VA_ARGS__), printf("\n"))
static void void_fatal(const char *fmt, ...) { (void)fmt; ++fatals; }

/* EGL model of one game thread: a context is current on it with one surface, or nothing is. */
static int ctx_storage, pbuffer_storage, window_storage[64], next_window;
static EGLContext current_context; static EGLSurface current_surface; static EGLint egl_error = EGL_SUCCESS;
static unsigned swaps, swap_failures, destroyed, presented;
static int fail_next_swap;
static EGLBoolean eglMakeCurrent(EGLDisplay d, EGLSurface draw, EGLSurface read, EGLContext c)
{ (void)d; (void)read; current_context = c; current_surface = c ? draw : EGL_NO_SURFACE; return 1; }
static EGLContext eglGetCurrentContext(void) { return current_context; }
static EGLint eglGetError(void) { EGLint e = egl_error; egl_error = EGL_SUCCESS; return e; }
static EGLBoolean eglDestroySurface(EGLDisplay d, EGLSurface s) { (void)d; (void)s; ++destroyed; return 1; }
static EGLSurface eglCreateWindowSurface(EGLDisplay d, EGLConfig c, EGLNativeWindowType w, const EGLint *a)
{ (void)d; (void)c; (void)w; (void)a; ++new_surfaces; return &window_storage[next_window++ & 63]; }
static EGLBoolean eglSwapInterval(EGLDisplay d, EGLint i) { (void)d; (void)i; return 1; }
static EGLBoolean eglQuerySurface(EGLDisplay d, EGLSurface s, EGLint a, EGLint *v)
{ (void)d; (void)s; *v = a == EGL_WIDTH ? 2400 : 1080; return 1; }
static EGLBoolean eglSwapBuffers(EGLDisplay d, EGLSurface s)
{
    (void)d;
    if (fail_next_swap || current_context == EGL_NO_CONTEXT || current_surface != s) {
        fail_next_swap = 0; egl_error = EGL_BAD_SURFACE; ++swap_failures; return 0;
    }
    ++swaps; return 1;
}
static void glBindFramebuffer(GLenum t, GLuint f) { (void)t; (void)f; }
static void glDisable(GLenum c) { (void)c; }
static void glDisablei(GLenum c, GLuint i) { (void)c; (void)i; }
static void glColorMaski(GLuint i, GLboolean r, GLboolean g, GLboolean b, GLboolean a) { (void)i; (void)r; (void)g; (void)b; (void)a; }
static void glViewport(GLint x, GLint y, GLint w, GLint h) { (void)x; (void)y; (void)w; (void)h; }
static void glClearColor(GLfloat r, GLfloat g, GLfloat b, GLfloat a) { (void)r; (void)g; (void)b; (void)a; }
static void glClear(GLenum m) { (void)m; }
static void glUseProgram(GLuint p) { (void)p; }
static void glUniform4f(GLint l, GLfloat a, GLfloat b, GLfloat c, GLfloat d) { (void)l; (void)a; (void)b; (void)c; (void)d; }
static void glActiveTexture(GLenum t) { (void)t; }
static void glBindTexture(GLenum t, GLuint i) { (void)t; (void)i; }
static void glBindSampler(GLuint u, GLuint s) { (void)u; (void)s; }
static void glTexParameteri(GLenum t, GLenum p, GLint v) { (void)t; (void)p; (void)v; }
static void glDrawArrays(GLenum m, GLint f, GLint c) { (void)m; (void)f; (void)c; }
static GLenum glGetError(void) { return 0; }

/* Window bookkeeping of android_main.c: one live window, the serial changes when Android replaces it. */
static uint32_t window_serial = 1; static int window_object;
static void *void_android_window_current(uint32_t *serial, int wait) { (void)wait; *serial = window_serial; return &window_object; }
static void void_android_window_ack(uint32_t serial) { (void)serial; }
static void void_blocking_begin(void) {}
static void void_blocking_end(void) {}
/* threads.c Loans (2026-11-20): the urgent audio producer never touches the lender's GL context. */
static void void_machine_lend_begin(void) {}
static void void_machine_lend_end(void) {}
static int r10_swap_interval(void) { return 1; }

typedef struct { struct { uint32_t wait_us; } stats; uint64_t frame; struct { uint32_t dirty; } st; } r10_device;
typedef struct { struct { uint32_t width, height; } desc; void *be; } r10_resource;
typedef struct { GLuint id; } gl_res;
static gl_res *res_be(r10_resource *r) { return (gl_res *)r->be; }
static void r10_fit(uint32_t sw, uint32_t sh, uint32_t dw, uint32_t dh, int32_t *x, int32_t *y, int32_t *w, int32_t *h)
{ (void)sw; (void)sh; *x = 0; *y = 0; *w = (int32_t)dw; *h = (int32_t)dh; }
static void gpu_timer_frame(r10_device *d, int begin) { (void)d; (void)begin; }
static void ring_next_segment(void) {}
static void r10_note_present(void) { ++presented; }
static uint64_t clock_ns;
static uint64_t r10_now_ns(void) { return clock_ns += 1000; }
static uint32_t bound_fbo; static struct { int valid; } applied; static GLuint present_program; static GLint present_rect_loc;
static unsigned flush_gen;

static EGLDisplay egl_display = (EGLDisplay)&ctx_storage;
static EGLContext egl_context = (EGLContext)&ctx_storage;
static EGLSurface egl_surface = EGL_NO_SURFACE, egl_pbuffer = (EGLSurface)&pbuffer_storage;
static EGLConfig egl_config;
static uint32_t egl_window_serial;

void d3d_gl_thread_release(void);
/* The frame limiter: a sleep inside void_blocking_begin hands the machine over (threads.c machine_release). */
static int limiter_sleeps;
static void r10_pace_frame(void) { if (limiter_sleeps) d3d_gl_thread_release(); }

/* PRODUCTION */

int main(void)
{
    r10_device device; memset(&device, 0, sizeof device);
    gl_res texture = { 7 };
    r10_resource back = { { 2400, 1080 }, &texture };
    /* A frame whose limiter does not sleep keeps its context. */
    assert(gles_present(&device, &back, 1) == 0);
    assert(swaps == 1 && swap_failures == 0 && new_surfaces == 1);
    /* Frames at the FPS cap sleep before the swap. */
    limiter_sleeps = 1;
    for (int frame = 0; frame < 120; ++frame) assert(gles_present(&device, &back, 1) == 0);
    printf("swaps=%u failures=%u surfaces=%u logs=%u\n", swaps, swap_failures, new_surfaces, logs);
    assert(swap_failures == 0 && "the swap after the limiter sleep must have the context current (EGL_BAD_SURFACE)");
    assert(swaps == 121 && new_surfaces == 1 && destroyed == 0 && "a lost swap recreated the window surface");
    assert(fatals == 0 && logs == 1);
    /* Android replaces the window: exactly one new surface, and swaps continue. */
    ++window_serial;
    for (int frame = 0; frame < 10; ++frame) assert(gles_present(&device, &back, 1) == 0);
    assert(swaps == 131 && swap_failures == 0 && new_surfaces == 2 && destroyed == 1);
    assert(presented == 131);
    fail_next_swap = 1;
    gles_present(&device, &back, 1);
    assert(swap_failures == 1 && presented == 131);
    gles_present(&device, &back, 1);
    assert(swaps == 132 && presented == 132);
    printf("PASS gles_present keeps its context across the limiter sleep\n");
    return 0;
}
'''


class GlesPresentContextTest(unittest.TestCase):
    def test_precompiled_programs_load_before_cold_links(self):
        source = (ROOT / "android/render10/r10_gles.c").read_text(encoding="utf-8")
        start = source.index("static struct warm_program {")
        declarations = source[start:source.index("static void release_warm_programs", start)]
        warmup = function(source, "static void warm_programs(void)\n{")
        harness = r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef unsigned GLuint;
typedef struct { uint64_t vs, ps; } r10_warm_pair;
typedef struct { const unsigned char *pairs; unsigned pair_count; } r10_warmup;
typedef struct { int unused; } r10_lib_shader;
static const r10_warm_pair pairs[] = {{1, 0}, {2, 0}, {3, 0}};
static const r10_lib_shader shader;
static uint64_t clock_ns;
static const char program_cache_dir[] = "ShaderCache";
enum { R10_WARM_MAX=4, R10_DEBUG_RECORD_WARMUP=1, R10_DEBUG_NO_WARMUP=2 };
#define R10_LOG(...) ((void)0)
static uint64_t r10_now_ns(void) { return clock_ns; }
static int r10_env_int(const char *name, int fallback, int lo, int hi) { return fallback; }
static int r10_shaderlib_warmup(r10_warmup *seed)
{
    *seed = (r10_warmup){(const unsigned char *)pairs, sizeof pairs / sizeof *pairs};
    return 1;
}
static uint32_t r10_shaderlib_count(void) { return 1; }
static const r10_lib_shader *r10_shaderlib_by_hash(uint64_t hash, int type) { return hash ? &shader : NULL; }
static const char *program_cache_path(char path[1280], uint64_t vs, uint64_t ps)
{
    path[0] = (char)('0' + vs);
    path[1] = 0;
    return path;
}
static GLuint load_program_binary(const char *path) { return path[0] == '3' ? 33 : 0; }
static GLuint null_fragment = 10;
static GLuint warm_shader(const r10_lib_shader *lib, GLuint *stages) { return (*stages = 20); }
static GLuint link_program(GLuint vs, GLuint ps, const char *what, const char *cache)
{
    assert(*cache == '1');
    clock_ns += 4000000000ull;
    return 11;
}
static void save_program_binary(GLuint id, const char *path) { assert(id == 11 && *path == '1'); }
static void glDeleteShader(GLuint id) { assert(id == 20); }
@WARMUP@
int main(void)
{
    warm_programs();
    assert(warmed_count == 2 && warmed[0].id == 33 && warmed[1].id == 11);
    puts("PASS GLES binary warmup precedes cold links");
    return 0;
}
'''
        self.assertIn("PASS GLES binary warmup precedes cold links",
                      self.run_harness(harness.replace("@WARMUP@", declarations + warmup)))

    def test_invalid_gpu_durations_never_reach_frame_stats(self):
        source = (ROOT / "android/render10/r10_gles.c").read_text(encoding="utf-8")
        timer = function(source, "static void gpu_timer_frame(r10_device *d, int end)\n{")
        begin = source.index("static GLuint gpu_timer[4];")
        declarations = source[begin:source.index(timer, begin)]
        harness = r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
typedef unsigned GLuint, GLenum;
typedef int GLint;
typedef uint64_t GLuint64;
typedef struct { struct { unsigned gpu_us, gpu_frames; } stats; } r10_device;
static unsigned available = 1;
static int disjoint, reads;
static uint64_t sample = 2000000;
static void glGenQueries(int n, GLuint *q) { for (int i = 0; i < n; ++i) q[i] = i + 1; }
static void glBeginQuery(GLenum target, GLuint id) { assert(id); }
static void glEndQuery(GLenum target) {}
static void glGetQueryObjectuiv(GLuint query, GLenum pname, GLuint *value) { *value = available; }
static void glGetIntegerv(GLenum pname, GLint *value) { *value = disjoint; disjoint = 0; }
static void get_result(GLuint query, GLenum pname, GLuint64 *value) { ++reads; *value = sample; }
static void (*gl_query_u64)(GLuint, GLenum, GLuint64 *) = get_result;
@TIMER@
static void frame(r10_device *d) { gpu_timer_frame(d, 0); gpu_timer_frame(d, 1); }
int main(void)
{
    r10_device device = {0};
    for (int i = 0; i < 3; ++i) frame(&device);
    available = 0;
    disjoint = 1;
    frame(&device);
    assert(reads == 0 && !disjoint);
    available = 1;
    for (int i = 0; i < 4; ++i) frame(&device);
    assert(reads == 0 && device.stats.gpu_frames == 0);
    sample = UINT64_MAX;
    frame(&device);
    assert(reads == 1 && device.stats.gpu_frames == 0);
    sample = 2000000;
    frame(&device);
    assert(reads == 2 && device.stats.gpu_frames == 1 && device.stats.gpu_us == 2000);
    puts("PASS disjoint and invalid GPU durations discarded");
    return 0;
}
'''
        self.assertIn("PASS disjoint and invalid GPU durations discarded",
                      self.run_harness(harness.replace("@TIMER@", declarations + timer)))

    def test_swap_after_limiter_sleep_has_context(self):
        source = (ROOT / "android/render10/r10_gles.c").read_text(encoding="utf-8").replace("\r\n", "\n")
        present = function(source, "static uint32_t gles_present(")
        # The harness models the sleep in r10_pace_frame; the swap must follow it.
        self.assertLess(present.index("r10_pace_frame();"), present.index("eglSwapBuffers("))
        production = "\n".join([
            function(source, "static void gl_current(void)"),
            function(source, "void d3d_gl_thread_release(void)"),
            function(source, "static int follow_window(void)"),
            present,
        ])
        self.assertIn("PASS gles_present keeps its context",
                      self.run_harness(HARNESS.replace("/* PRODUCTION */", production)))

    def run_harness(self, harness):
        OUT.mkdir(parents=True, exist_ok=True)
        probe = OUT / "probe.c"
        probe.write_text(harness, encoding="utf-8")
        env, linker = environment("x64")
        obj, binary = OUT / "probe.obj", OUT / "probe.exe"
        for command in ([str(CLANG), "/nologo", "/Od", "/W3", "/std:c11", "/clang:--target=x86_64-pc-windows-msvc",
                         "-Wno-pointer-bool-conversion", "-Wno-unused-function", "/c", str(probe), "/Fo" + str(obj)],
                        [str(linker), "/nologo", str(obj), "/OUT:" + str(binary)],
                        [str(binary)]):
            result = subprocess.run(command, capture_output=True, text=True, env=env, timeout=120)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        return result.stdout


if __name__ == "__main__":
    unittest.main()

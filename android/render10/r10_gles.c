/* OpenGL ES 3.2 backend of the native Direct3D 10 renderer.
 *
 * - Render targets keep Direct3D's orientation: the library's vertex shaders negate clip-space Y and
 *   map depth to -w..w, so texture row 0 is the top and glViewport/glScissor take Direct3D's
 *   rectangles unchanged; the present pass flips once into the window.  Front faces swap: Direct3D's
 *   clockwise front is OpenGL's counter-clockwise in these coordinates.
 * - Uniform data and dynamic vertex/index data stream through one persistently mapped ring
 *   (GL_EXT_buffer_storage, a fence per segment).  A dynamic buffer owns a slice of its full size per
 *   WRITE_DISCARD, and a draw copies the bytes it reads from the game's mapped memory into that slice
 *   (WRITE_NO_OVERWRITE appends land in the same slice), so draws already recorded keep their data.
 * - Textures without a GPU format (BC on Adreno) are decoded to RGBA8 when they are uploaded.
 * - The context follows the guest thread that owns the machine (d3d_gl_thread_release). */
#define _GNU_SOURCE
#include "r10.h"
#include "r10_shaderlib.h"
#include "r10_vk_shaders.h"   /* the FSR1 passes as GLSL ES text */
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl32.h>
#include <GLES2/gl2ext.h>
#include <vulkan/vulkan_core.h>   /* shader library image-view types */
#include <stdlib.h>
#include <errno.h>
#include <time.h>
#include <unistd.h>

#ifndef GL_MAP_PERSISTENT_BIT_EXT
#define GL_MAP_PERSISTENT_BIT_EXT 0x0040
#define GL_MAP_COHERENT_BIT_EXT 0x0080
#endif
#ifndef GL_DEPTH_CLAMP_EXT
#define GL_DEPTH_CLAMP_EXT 0x864F
#endif
#ifndef GL_CLIP_DISTANCE0_EXT
#define GL_CLIP_DISTANCE0_EXT 0x3000
#endif
#ifndef GL_TEXTURE_SRGB_DECODE_EXT
#define GL_TEXTURE_SRGB_DECODE_EXT 0x8A48
#define GL_DECODE_EXT 0x8A49
#define GL_SKIP_DECODE_EXT 0x8A4A
#endif
/* GL_EXT_disjoint_timer_query (GPU frame time in the statistics) */
typedef void (*PFNGETQUERYU64)(GLuint, GLenum, GLuint64 *);
static PFNGETQUERYU64 gl_query_u64;

#ifndef GL_TEXTURE_MAX_ANISOTROPY_EXT
#define GL_TEXTURE_MAX_ANISOTROPY_EXT 0x84FE
#define GL_MAX_TEXTURE_MAX_ANISOTROPY_EXT 0x84FF
#endif
typedef void (*PFNBUFFERSTORAGE)(GLenum, GLsizeiptr, const void *, GLbitfield);

extern void *void_android_window_current(uint32_t *serial, int wait) __attribute__((weak));
extern void void_android_window_ack(uint32_t serial) __attribute__((weak));
extern void void_android_gpu_strings(const char *renderer, const char *version) __attribute__((weak));
extern int void_bioshock_srgb_textures(void) __attribute__((weak));
int void_files_cache_directory(const char *profile, const char *name, char *path, size_t cap);

/* ------------------------------------------------------------------ context */
static EGLDisplay egl_display = EGL_NO_DISPLAY;
static EGLContext egl_context = EGL_NO_CONTEXT;
static EGLSurface egl_surface = EGL_NO_SURFACE, egl_pbuffer = EGL_NO_SURFACE;
static EGLConfig egl_config;
static uint32_t egl_window_serial;
static int window_mode;

static struct {
    int buffer_storage, clip_distance, depth_clamp, s3tc, rgtc, astc, norm16, anisotropic, srgb_decode;
    int dxt1, s3tc_srgb;         /* BC1 alone (PowerVR: GL_EXT_texture_compression_dxt1), sRGB BC1-3 */
    int emulate_base_vertex;
    float max_anisotropy;
    GLint max_units, max_draw_buffers, max_uniform_block;
    PFNBUFFERSTORAGE BufferStorage;
} caps;

static void gl_current(void)
{
    if (egl_context == EGL_NO_CONTEXT || eglGetCurrentContext() == egl_context) return;
    EGLSurface s = egl_surface != EGL_NO_SURFACE ? egl_surface : egl_pbuffer;
    if (!eglMakeCurrent(egl_display, s, s, egl_context)) void_fatal("[r10] GL context acquire failed %04X", eglGetError());
}

/* threads.c calls this when the guest thread gives up the machine. */
void d3d_gl_thread_release(void)
{
    if (egl_context == EGL_NO_CONTEXT || eglGetCurrentContext() != egl_context) return;
    if (!eglMakeCurrent(egl_display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT))
        void_fatal("[r10] GL context release failed %04X", eglGetError());
}
void d3d_gl_thread_acquire(void) {}

/* Take up the activity's current window if it changed; without one the game waits here. */
static int follow_window(void)
{
    if (!void_android_window_current) return egl_surface != EGL_NO_SURFACE;
    uint32_t serial;
    void *window = void_android_window_current(&serial, 0);
    if (serial == egl_window_serial && egl_surface != EGL_NO_SURFACE) return 1;
    eglMakeCurrent(egl_display, egl_pbuffer, egl_pbuffer, egl_context);
    if (egl_surface != EGL_NO_SURFACE) eglDestroySurface(egl_display, egl_surface);
    egl_surface = EGL_NO_SURFACE;
    egl_window_serial = serial;
    if (void_android_window_ack) void_android_window_ack(serial);
    if (!window) {
        R10_LOG("GLES: surface gone, the game waits for the activity");
        void_blocking_begin();
        window = void_android_window_current(&serial, 1);
        void_blocking_end();
        egl_window_serial = serial;
    }
    egl_surface = eglCreateWindowSurface(egl_display, egl_config, (EGLNativeWindowType)window, NULL);
    if (egl_surface == EGL_NO_SURFACE || !eglMakeCurrent(egl_display, egl_surface, egl_surface, egl_context)) {
        R10_LOG("GLES: window surface failed %04X", eglGetError());
        eglMakeCurrent(egl_display, egl_pbuffer, egl_pbuffer, egl_context);
        egl_surface = EGL_NO_SURFACE;
        if (void_android_window_ack) void_android_window_ack(serial);
        return 0;
    }
    eglSwapInterval(egl_display, r10_swap_interval());
    R10_LOG("GLES: presenting into a new window surface");
    if (void_android_window_ack) void_android_window_ack(serial);
    return 1;
}

/* ------------------------------------------------------------------ backend objects */
typedef struct gl_res {
    GLuint id;                   /* buffer or texture */
    GLenum target;
    GLenum internal, format, type;
    int compressed, decoded, convert;   /* decoded: BC stored as RGBA8; convert: CPU conversion of uploads */
    int converted;               /* the converted pack's blocks (r10_pack.c): ASTC 4x4 or ETC2 RGBA8 EAC */
    int base_level, max_level;   /* the levels the last bound view selected */
    uint64_t slice_version;      /* dynamic buffers: the version whose ring slice is current */
    uint32_t slice_offset, slice_lo, slice_hi;   /* ring offset of the slice and the bytes copied into it */
    uint32_t slice_generation;
    uint32_t cb_offset; uint64_t cb_version; uint32_t cb_generation;   /* constant buffer upload of the ring */
    uint8_t *data;               /* constant buffers of DEFAULT usage: the bytes UpdateSubresource wrote */
} gl_res;

typedef struct gl_view { GLuint tex; GLenum target; int level, layer, layers, base, levels; int depth_stencil; } gl_view;
/* id: as the game described it; raw: without sRGB decode ("HDR off"); nearest: for depth textures,
 * which OpenGL ES samples only with nearest filtering unless they compare. */
typedef struct gl_sampler { GLuint id, raw, nearest; int compare; } gl_sampler;
typedef struct gl_shader {
    GLuint id, icb;
    int uses_vertex_id, texture_cb0, compile_failed, has_program;
    GLenum texture_targets[];  /* indexed by the shader library's combined samplers */
} gl_shader;

typedef struct gl_program {
    r10_shader *vs, *ps;
    GLuint id;
    struct gl_program *next;
} gl_program;

#define PROGRAM_BUCKETS 4096
static gl_program *programs[PROGRAM_BUCKETS];
static GLuint null_fragment;
static GLuint vao, draw_fbo, read_fbo;
static GLuint cb0_textures[2];   /* one texture buffer per shader stage */
static GLuint present_program, present_vs, present_fs;
static GLint present_rect_loc;

typedef struct fbo_entry {
    uint64_t key[9];
    GLuint fbo;
    struct fbo_entry *next;
} fbo_entry;
#define FBO_BUCKETS 512
static fbo_entry *fbos[FBO_BUCKETS];

/* ------------------------------------------------------------------ the ring */
#define RING_SEGMENTS 4u
#define RING_SEGMENT (24u << 20)
static GLuint ring_buffer;
static uint8_t *ring_map;
static GLsync ring_fence[RING_SEGMENTS];
static uint32_t ring_segment, ring_used, ring_generation = 1;
static GLint ubo_align = 256;

static int ring_init(void)
{
    glGenBuffers(1, &ring_buffer);
    glBindBuffer(GL_COPY_WRITE_BUFFER, ring_buffer);
    GLsizeiptr size = (GLsizeiptr)RING_SEGMENTS * RING_SEGMENT;
    if (caps.buffer_storage) {
        caps.BufferStorage(GL_COPY_WRITE_BUFFER, size, NULL, GL_MAP_WRITE_BIT | GL_MAP_PERSISTENT_BIT_EXT | GL_MAP_COHERENT_BIT_EXT);
        ring_map = (uint8_t *)glMapBufferRange(GL_COPY_WRITE_BUFFER, 0, size,
                                               GL_MAP_WRITE_BIT | GL_MAP_PERSISTENT_BIT_EXT | GL_MAP_COHERENT_BIT_EXT);
    }
    if (!ring_map) {
        R10_LOG("GLES: no persistent mapping; uploads use glBufferSubData");
        glBufferData(GL_COPY_WRITE_BUFFER, size, NULL, GL_STREAM_DRAW);
    }
    glGetIntegerv(GL_UNIFORM_BUFFER_OFFSET_ALIGNMENT, &ubo_align);
    if (ubo_align < 16) ubo_align = 16;
    if (caps.max_uniform_block < 4096 * 16) {
        GLint alignment = 0;
        glGetIntegerv(GL_TEXTURE_BUFFER_OFFSET_ALIGNMENT, &alignment);
        if (alignment > ubo_align) ubo_align = alignment;
        glGenTextures(2, cb0_textures);
    }
    return 1;
}

static void ring_next_segment(void)
{
    ring_fence[ring_segment] = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
    ring_segment = (ring_segment + 1) % RING_SEGMENTS;
    ring_used = 0;
    ++ring_generation;
    if (ring_fence[ring_segment]) {
        uint64_t wait_start = r10_now_ns();
        void_machine_lend_begin();   /* the audio producer may render meanwhile (threads.c, Loans) */
        GLenum r = glClientWaitSync(ring_fence[ring_segment], GL_SYNC_FLUSH_COMMANDS_BIT, 2000000000ull);
        void_machine_lend_end();
        if (r10_dev) r10_dev->stats.wait_us += (uint32_t)((r10_now_ns() - wait_start) / 1000);
        if (r == GL_TIMEOUT_EXPIRED || r == GL_WAIT_FAILED) R10_LOG("GLES: ring wait %04X", r);
        glDeleteSync(ring_fence[ring_segment]);
        ring_fence[ring_segment] = 0;
    }
}

/* Room for `bytes` (aligned); returns the ring offset. */
static uint32_t ring_alloc(uint32_t bytes, uint32_t align)
{
    if (bytes > RING_SEGMENT) return UINT32_MAX;
    uint32_t at = (ring_used + align - 1) & ~(align - 1);
    if (at + bytes > RING_SEGMENT) { ring_next_segment(); at = 0; }
    ring_used = at + bytes;
    return ring_segment * RING_SEGMENT + at;
}

static void ring_write(uint32_t offset, const void *data, uint32_t bytes)
{
    if (ring_map) memcpy(ring_map + offset, data, bytes);
    else { glBindBuffer(GL_COPY_WRITE_BUFFER, ring_buffer); glBufferSubData(GL_COPY_WRITE_BUFFER, offset, bytes, data); }
}

/* ------------------------------------------------------------------ helpers */
static gl_res *res_be(r10_resource *r) { return (gl_res *)r->be; }

static GLenum tex_target(const r10_resource *r)
{
    if (r->desc.dim == R10_DIM_TEXTURE3D) return GL_TEXTURE_3D;
    if (r->desc.misc & D3D10_RESOURCE_MISC_TEXTURECUBE) return r->desc.layers > 6 ? GL_TEXTURE_CUBE_MAP_ARRAY : GL_TEXTURE_CUBE_MAP;
    return r->desc.layers > 1 ? GL_TEXTURE_2D_ARRAY : GL_TEXTURE_2D;
}

static int format_usable(const r10_format *f)
{
    if (!f->gl_internal) return 0;
    if (f->kind == R10_FMT_BC) {
        /* Per format: the sRGB S3TC formats have an extension of their own, and BC1 can come alone. */
        if (f->bc > 3) return caps.rgtc;
        if (f->srgb && !caps.s3tc_srgb) return 0;
        return f->bc == 1 ? caps.s3tc || caps.dxt1 : caps.s3tc;
    }
    if (f->gl_internal == 0x805B || f->gl_internal == 0x822C || f->gl_internal == 0x822A || f->gl_internal == 0x8F9B ||
        f->gl_internal == 0x8F99 || f->gl_internal == 0x8F98) return caps.norm16;
    return 1;
}

/* Converts `w x h` texels of a subresource to what the texture stores; returns the data to upload
 * (`owned` must be freed) and its row pitch. */
static const void *convert_upload(r10_resource *r, gl_res *g, const void *data, uint32_t w, uint32_t h, uint32_t pitch,
                                  void **owned, uint32_t *out_pitch)
{
    const r10_format *f = r->fmt;
    *owned = NULL;
    *out_pitch = pitch;
    if (g->decoded) {
        uint8_t *rgba = (uint8_t *)malloc((size_t)w * h * 4);
        r10_bc_decode(f->bc, f->dxgi == 81 || f->dxgi == 84, (const uint8_t *)data, w, h, pitch, rgba, w * 4);
        *owned = rgba;
        *out_pitch = w * 4;
        return rgba;
    }
    if (f->gl_swizzle == 1) {           /* BGRA -> RGBA */
        uint8_t *out = (uint8_t *)malloc((size_t)w * h * 4);
        for (uint32_t y = 0; y < h; ++y) {
            const uint8_t *s = (const uint8_t *)data + (size_t)y * pitch;
            uint8_t *d = out + (size_t)y * w * 4;
            for (uint32_t x = 0; x < w; ++x) {
                d[4 * x] = s[4 * x + 2]; d[4 * x + 1] = s[4 * x + 1]; d[4 * x + 2] = s[4 * x];
                d[4 * x + 3] = f->gl_alpha_one ? 255 : s[4 * x + 3];
            }
        }
        *owned = out;
        *out_pitch = w * 4;
        return out;
    }
    if (f->gl_swizzle == 2) {           /* B5G5R5A1 -> RGBA8 */
        uint8_t *out = (uint8_t *)malloc((size_t)w * h * 4);
        for (uint32_t y = 0; y < h; ++y) {
            const uint16_t *s = (const uint16_t *)((const uint8_t *)data + (size_t)y * pitch);
            uint8_t *d = out + (size_t)y * w * 4;
            for (uint32_t x = 0; x < w; ++x) {
                uint16_t v = s[x];
                uint32_t b = v & 31, gg = (v >> 5) & 31, rr = (v >> 10) & 31;
                d[4 * x] = (uint8_t)(rr << 3 | rr >> 2); d[4 * x + 1] = (uint8_t)(gg << 3 | gg >> 2);
                d[4 * x + 2] = (uint8_t)(b << 3 | b >> 2); d[4 * x + 3] = (v & 0x8000) ? 255 : 0;
            }
        }
        *owned = out;
        *out_pitch = w * 4;
        return out;
    }
    return data;
}

static void upload_level(r10_resource *r, gl_res *g, uint32_t sub, uint32_t x, uint32_t y, uint32_t z, uint32_t w, uint32_t h,
                         uint32_t depth, const void *data, uint32_t pitch, uint32_t slice)
{
    uint32_t mip = sub % r->desc.mips, layer = sub / r->desc.mips;
    uint32_t lw, lh, ld;
    r10_subresource_size(r, sub, &lw, &lh, &ld);
    glBindTexture(g->target, g->id);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    if (g->compressed) {
        /* Whole blocks: Direct3D boxes of BC textures are block aligned. The converted pack's blocks are 16 bytes. */
        uint32_t block = g->converted ? R10_PACK_BLOCK_BYTES : r->fmt->bytes;
        uint32_t bw = (w + 3) / 4, bh = (h + 3) / 4, bytes = bw * bh * block;
        uint8_t *packed = (uint8_t *)malloc(bytes ? bytes : 1);
        for (uint32_t row = 0; row < bh; ++row) memcpy(packed + row * bw * block, (const uint8_t *)data + (size_t)row * pitch, bw * block);
        uint32_t uw = x + w > lw ? lw - x : w, uh = y + h > lh ? lh - y : h;
        if (g->target == GL_TEXTURE_CUBE_MAP)
            glCompressedTexSubImage2D(GL_TEXTURE_CUBE_MAP_POSITIVE_X + layer, (GLint)mip, (GLint)x, (GLint)y, (GLsizei)uw, (GLsizei)uh, g->internal, (GLsizei)bytes, packed);
        else if (g->target == GL_TEXTURE_2D)
            glCompressedTexSubImage2D(GL_TEXTURE_2D, (GLint)mip, (GLint)x, (GLint)y, (GLsizei)uw, (GLsizei)uh, g->internal, (GLsizei)bytes, packed);
        else
            glCompressedTexSubImage3D(g->target, (GLint)mip, (GLint)x, (GLint)y, (GLint)(g->target == GL_TEXTURE_3D ? z : layer), (GLsizei)uw, (GLsizei)uh, 1, g->internal, (GLsizei)bytes, packed);
        free(packed);
        return;
    }
    for (uint32_t k = 0; k < depth; ++k) {
        void *owned;
        uint32_t row_pitch;
        const uint8_t *src = (const uint8_t *)data + (size_t)k * slice;
        uint32_t uw = w, uh = h;
        if (g->decoded) { uw = (x + w > lw ? lw - x : w); uh = (y + h > lh ? lh - y : h); }
        const void *pixels = convert_upload(r, g, src, g->decoded ? w : w, g->decoded ? h : h, pitch, &owned, &row_pitch);
        uint32_t texel = g->decoded || r->fmt->gl_swizzle ? 4 : r->fmt->bytes;
        glPixelStorei(GL_UNPACK_ROW_LENGTH, (GLint)(row_pitch / texel));
        if (g->target == GL_TEXTURE_CUBE_MAP)
            glTexSubImage2D(GL_TEXTURE_CUBE_MAP_POSITIVE_X + layer, (GLint)mip, (GLint)x, (GLint)y, (GLsizei)uw, (GLsizei)uh, g->format, g->type, pixels);
        else if (g->target == GL_TEXTURE_2D)
            glTexSubImage2D(GL_TEXTURE_2D, (GLint)mip, (GLint)x, (GLint)y, (GLsizei)uw, (GLsizei)uh, g->format, g->type, pixels);
        else
            glTexSubImage3D(g->target, (GLint)mip, (GLint)x, (GLint)y, (GLint)(g->target == GL_TEXTURE_3D ? z + k : layer), (GLsizei)uw, (GLsizei)uh, 1, g->format, g->type, pixels);
        glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
        free(owned);
    }
}

/* ------------------------------------------------------------------ init */
static GLuint compile_sources(GLenum stage, GLsizei count, const char *const *sources, const char *what)
{
    GLuint s = glCreateShader(stage);
    glShaderSource(s, count, sources, NULL);
    void_machine_lend_begin();
    glCompileShader(s);
    GLint ok = 0;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    void_machine_lend_end();
    if (!ok) {
        char log[2048];
        glGetShaderInfoLog(s, sizeof log, NULL, log);
        R10_LOG("GLES: %s does not compile: %s", what, log);
        glDeleteShader(s);
        return 0;
    }
    return s;
}

static GLuint compile(GLenum stage, const char *text, const char *what)
{
    return compile_sources(stage, 1, &text, what);
}

static char program_cache_dir[1100];
static uint64_t program_driver_key;

static GLuint load_program_binary(const char *path)
{
    FILE *file = fopen(path, "rb");
    if (!file) return 0;
    uint32_t header[2];
    GLuint program = 0;
    if (fread(header, sizeof header, 1, file) == 1 && header[1] && header[1] <= 8 * 1024 * 1024) {
        void *data = malloc(header[1]);
        if (data && fread(data, 1, header[1], file) == header[1]) {
            program = glCreateProgram();
            glProgramBinary(program, header[0], data, (GLsizei)header[1]);
            GLint linked = 0;
            glGetProgramiv(program, GL_LINK_STATUS, &linked);
            if (!linked) { glGetError(); glDeleteProgram(program); program = 0; }
        }
        free(data);
    }
    fclose(file);
    return program;
}

static void save_program_binary(GLuint program, const char *path)
{
    GLint capacity = 0;
    glGetProgramiv(program, GL_PROGRAM_BINARY_LENGTH, &capacity);
    if (capacity <= 0 || capacity > 8 * 1024 * 1024) return;
    void *data = malloc((size_t)capacity);
    if (!data) return;
    GLsizei length = 0;
    GLenum format;
    glGetProgramBinary(program, capacity, &length, &format, data);
    if (length > 0) {
        char draft[1400];
        int n = snprintf(draft, sizeof draft, "%s.XXXXXX", path);
        int fd = n > 0 && n < (int)sizeof draft ? mkstemp(draft) : -1;
        if (fd >= 0) {
            FILE *file = fdopen(fd, "wb");
            uint32_t header[] = { format, (uint32_t)length };
            if (file) {
                int ok = fwrite(header, sizeof header, 1, file) == 1 && fwrite(data, 1, length, file) == (size_t)length;
                if (!fclose(file) && ok) rename(draft, path);
            } else close(fd);
            unlink(draft);
        }
    }
    free(data);
}

static GLuint link_program(GLuint vs, GLuint fs, const char *what, const char *cache)
{
    GLuint p = glCreateProgram();
    if (cache) glProgramParameteri(p, GL_PROGRAM_BINARY_RETRIEVABLE_HINT, GL_TRUE);
    glAttachShader(p, vs);
    glAttachShader(p, fs);
    glLinkProgram(p);
    GLint ok = 0;
    glGetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[2048];
        glGetProgramInfoLog(p, sizeof log, NULL, log);
        R10_LOG("GLES: %s does not link: %s", what, log);
        glDeleteProgram(p);
        return 0;
    }
    glDetachShader(p, vs);
    glDetachShader(p, fs);
    return p;
}

static const char present_vs_text[] =
    "#version 320 es\n"
    "out vec2 uv;\n"
    "void main() {\n"
    "    vec2 p = vec2(float((gl_VertexID << 1) & 2), float(gl_VertexID & 2));\n"
    "    uv = vec2(p.x, 1.0 - p.y);\n"
    "    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);\n"
    "}\n";
static const char present_fs_text[] =
    "#version 320 es\n"
    "precision highp float;\n"
    "in vec2 uv;\n"
    "layout(binding = 0) uniform highp sampler2D image;\n"
    "uniform vec4 rect;\n"
    "layout(location = 0) out vec4 color;\n"
    "void main() { color = vec4(texture(image, rect.xy + uv * rect.zw).rgb, 1.0); }\n";

void void_native_worker_offload_begin(void);    /* threads.c */
void void_native_worker_affinity_end(void);

/* The GL driver's threads are born inside EGL initialisation on the calling guest thread, pinned to the guest
 * machine's prime core: they get the big cores beside it instead (see vk_init). */
static int gles_init_inner(r10_device *d);
static void warm_programs(void);
static void release_warm_programs(void);
static int gles_init(r10_device *d)
{
    void_native_worker_offload_begin();
    int ok = gles_init_inner(d);
    void_native_worker_affinity_end();
    return ok;
}

static int gles_init_inner(r10_device *d)
{
    (void)d;
    egl_display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (!eglInitialize(egl_display, NULL, NULL)) { R10_LOG("GLES: eglInitialize failed %04X", eglGetError()); return 0; }
    uint32_t serial = 0;
    void *window = void_android_window_current ? void_android_window_current(&serial, 1) : NULL;
    window_mode = window != NULL;
    egl_window_serial = serial;
    const EGLint attrs[] = { EGL_SURFACE_TYPE, EGL_WINDOW_BIT | EGL_PBUFFER_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT_KHR,
                             EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
                             EGL_DEPTH_SIZE, 0, EGL_STENCIL_SIZE, 0, EGL_NONE };
    EGLint n = 0;
    if (!eglChooseConfig(egl_display, attrs, &egl_config, 1, &n) || !n) { R10_LOG("GLES: no EGL config"); return 0; }
    const EGLint ctx[] = { EGL_CONTEXT_CLIENT_VERSION, 3, EGL_CONTEXT_MINOR_VERSION_KHR, 2, EGL_NONE };
    egl_context = eglCreateContext(egl_display, egl_config, EGL_NO_CONTEXT, ctx);
    if (egl_context == EGL_NO_CONTEXT) { R10_LOG("GLES: no OpenGL ES 3.2 context (%04X)", eglGetError()); return 0; }
    const EGLint pb[] = { EGL_WIDTH, 16, EGL_HEIGHT, 16, EGL_NONE };
    egl_pbuffer = eglCreatePbufferSurface(egl_display, egl_config, pb);
    if (window) egl_surface = eglCreateWindowSurface(egl_display, egl_config, (EGLNativeWindowType)window, NULL);
    EGLSurface s = egl_surface != EGL_NO_SURFACE ? egl_surface : egl_pbuffer;
    if (!eglMakeCurrent(egl_display, s, s, egl_context)) { R10_LOG("GLES: eglMakeCurrent failed %04X", eglGetError()); return 0; }
    if (egl_surface != EGL_NO_SURFACE) eglSwapInterval(egl_display, r10_swap_interval());
    if (void_android_window_ack) void_android_window_ack(serial);
    const char *ext = (const char *)glGetString(GL_EXTENSIONS);
    ext = ext ? ext : "";
    caps.buffer_storage = strstr(ext, "GL_EXT_buffer_storage") != NULL;
    caps.BufferStorage = (PFNBUFFERSTORAGE)eglGetProcAddress("glBufferStorageEXT");
    if (!caps.BufferStorage) caps.buffer_storage = 0;
    caps.clip_distance = strstr(ext, "GL_EXT_clip_cull_distance") != NULL;
    caps.depth_clamp = strstr(ext, "GL_EXT_depth_clamp") != NULL;
    caps.s3tc = strstr(ext, "GL_EXT_texture_compression_s3tc") != NULL;
    caps.s3tc_srgb = strstr(ext, "GL_EXT_texture_compression_s3tc_srgb") != NULL;
    caps.dxt1 = strstr(ext, "GL_EXT_texture_compression_dxt1") != NULL;
    caps.rgtc = strstr(ext, "GL_EXT_texture_compression_rgtc") != NULL;
    caps.astc = strstr(ext, "GL_KHR_texture_compression_astc_ldr") != NULL;
    /* The converted pack (r10_pack.c): ASTC with the extension, ETC2 is core in OpenGL ES 3.0. */
    r10_gles_backend.pack_formats = (caps.astc ? R10_PACK_ASTC : 0) | R10_PACK_ETC2;
    caps.norm16 = strstr(ext, "GL_EXT_texture_norm16") != NULL;
    caps.anisotropic = strstr(ext, "GL_EXT_texture_filter_anisotropic") != NULL;
    caps.srgb_decode = strstr(ext, "GL_EXT_texture_sRGB_decode") != NULL;
    const char *renderer = (const char *)glGetString(GL_RENDERER), *version = (const char *)glGetString(GL_VERSION);
    const char *g51 = renderer ? strstr(renderer, "Mali-G51") : NULL;
    int mali_g51 = g51 && (g51[8] < '0' || g51[8] > '9');
    caps.emulate_base_vertex = renderer && (strstr(renderer, "Mali-T") || mali_g51);
    if (strstr(ext, "GL_EXT_disjoint_timer_query") && !(mali_g51 && version && strstr(version, "r18p0")))
        gl_query_u64 = (PFNGETQUERYU64)eglGetProcAddress("glGetQueryObjectui64vEXT");
    if (caps.emulate_base_vertex) R10_LOG("GLES: using vertex buffer offsets for indexed base vertices");
    if (caps.anisotropic) glGetFloatv(GL_MAX_TEXTURE_MAX_ANISOTROPY_EXT, &caps.max_anisotropy);
    glGetIntegerv(GL_MAX_COMBINED_TEXTURE_IMAGE_UNITS, &caps.max_units);
    glGetIntegerv(GL_MAX_DRAW_BUFFERS, &caps.max_draw_buffers);
    glGetIntegerv(GL_MAX_UNIFORM_BLOCK_SIZE, &caps.max_uniform_block);
    r10_gles_backend.bc_supported = caps.s3tc;
    R10_LOG("GLES: %s / %s / %s; buffer storage %d, clip distance %d, depth clamp %d, S3TC %d (sRGB %d, DXT1 %d), RGTC %d, ASTC %d, units %d",
            (const char *)glGetString(GL_VENDOR), renderer, version, caps.buffer_storage, caps.clip_distance, caps.depth_clamp,
            caps.s3tc, caps.s3tc_srgb, caps.dxt1, caps.rgtc, caps.astc, caps.max_units);
    if (void_android_gpu_strings) void_android_gpu_strings(renderer, version);
    GLint program_binary_formats = 0;
    glGetIntegerv(GL_NUM_PROGRAM_BINARY_FORMATS, &program_binary_formats);
    const char *profile = void_data_dir();
    program_cache_dir[0] = 0;
    if (program_binary_formats && profile &&
        !void_files_cache_directory(profile, "ShaderCache", program_cache_dir, sizeof program_cache_dir)) {
        program_cache_dir[0] = 0;
        R10_LOG("GLES: could not create the program binary cache directory");
    }
    program_driver_key = 1469598103934665603ull;
    const char *const driver[] = { renderer, version };
    for (unsigned i = 0; i < sizeof driver / sizeof *driver; ++i)
        for (const unsigned char *c = (const unsigned char *)driver[i]; c && *c; ++c)
            program_driver_key = (program_driver_key ^ *c) * 1099511628211ull;
    const GLint shader_caps[] = { caps.max_uniform_block, caps.max_units, caps.clip_distance };
    for (unsigned i = 0; i < sizeof shader_caps / sizeof *shader_caps; ++i)
        program_driver_key = (program_driver_key ^ (uint32_t)shader_caps[i]) * 1099511628211ull;
    glGenVertexArrays(1, &vao);
    glBindVertexArray(vao);
    glGenFramebuffers(1, &draw_fbo);
    glGenFramebuffers(1, &read_fbo);
    glEnable(GL_PRIMITIVE_RESTART_FIXED_INDEX);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    ring_init();
    null_fragment = compile(GL_FRAGMENT_SHADER, "#version 320 es\nvoid main() {}\n", "null fragment shader");
    present_vs = compile(GL_VERTEX_SHADER, present_vs_text, "present vertex shader");
    present_fs = compile(GL_FRAGMENT_SHADER, present_fs_text, "present fragment shader");
    present_program = present_vs && present_fs ? link_program(present_vs, present_fs, "present program", NULL) : 0;
    if (!present_program) return 0;
    present_rect_loc = glGetUniformLocation(present_program, "rect");
    warm_programs();
    return 1;
}

static void gles_shutdown(r10_device *d)
{
    (void)d;
    release_warm_programs();
}

/* ------------------------------------------------------------------ resources */
static void texture_create(r10_resource *r, gl_res *g);
static void forget_fbos_of(GLuint tex);

static int gles_resource_create(r10_device *d, r10_resource *r, const uint32_t *init)
{
    (void)d;
    gl_current();
    gl_res *g = (gl_res *)calloc(1, sizeof *g);
    r->be = g;
    if (r->desc.dim == R10_DIM_BUFFER) {
        int streamed = r->desc.usage == D3D10_USAGE_DYNAMIC || (r->desc.bind & D3D10_BIND_CONSTANT_BUFFER);
        if (r->desc.usage == D3D10_USAGE_STAGING) return 1;
        if ((r->desc.bind & D3D10_BIND_CONSTANT_BUFFER) && r->desc.usage != D3D10_USAGE_DYNAMIC) {
            g->data = (uint8_t *)calloc(1, r->desc.width);
            if (init && init[0]) memcpy(g->data, GPTR(init[0]), r->desc.width);
        }
        if (streamed) return 1;   /* a dynamic buffer gets its GL buffer at its first static upload */
        glGenBuffers(1, &g->id);
        glBindBuffer(GL_COPY_WRITE_BUFFER, g->id);
        glBufferData(GL_COPY_WRITE_BUFFER, r->desc.width, init && init[0] ? GPTR(init[0]) : NULL, GL_STATIC_DRAW);
        return 1;
    }
    if (r->desc.usage == D3D10_USAGE_STAGING) return 1;
    texture_create(r, g);
    if (init) {
        for (uint32_t s = 0; s < r->subresources; ++s) {
            const uint32_t *sd = init + 3 * s;
            if (!sd[0]) continue;
            uint32_t w, h, dd;
            r10_subresource_size(r, s, &w, &h, &dd);
            upload_level(r, g, s, 0, 0, 0, w, h, dd, GPTR(sd[0]), sd[1], sd[2]);
        }
    }
    return 1;
}

/* The GL texture of a resource for its current storage (gles_resource_create, gles_resource_store). */
static void texture_create(r10_resource *r, gl_res *g)
{
    const r10_format *f = r->fmt;
    g->target = tex_target(r);
    g->compressed = g->decoded = g->convert = g->converted = 0;
    g->base_level = 0;
    if (f->kind == R10_FMT_BC && r->converted) {
        /* The converted pack's blocks (r10_pack.c): COMPRESSED_[SRGB8_ALPHA8_]ASTC_4x4 or [S]RGBA8_ETC2_EAC. */
        int etc2 = r10_pack_family() == R10_PACK_ETC2;
        g->internal = etc2 ? (f->srgb ? 0x9279u : 0x9278u) : (f->srgb ? 0x93D0u : 0x93B0u);
        g->format = g->type = 0;
        g->compressed = g->converted = 1;
    } else if (f->kind == R10_FMT_BC && !format_usable(f)) {
        g->decoded = 1;
        g->internal = f->srgb ? GL_SRGB8_ALPHA8 : GL_RGBA8; g->format = GL_RGBA; g->type = GL_UNSIGNED_BYTE;
    } else if (!format_usable(f)) {
        /* 16-bit normalized formats without GL_EXT_texture_norm16: a float format of the same channels. */
        g->internal = f->bytes == 8 ? GL_RGBA16F : f->bytes == 4 ? GL_RG16F : GL_R16F;
        g->format = f->bytes == 8 ? GL_RGBA : f->bytes == 4 ? GL_RG : GL_RED;
        g->type = GL_HALF_FLOAT;
        g->convert = 1;
        R10_LOG("GLES: %s stored as half float", f->name);
    } else {
        g->internal = f->gl_internal; g->format = f->gl_format; g->type = f->gl_type;
        g->compressed = f->kind == R10_FMT_BC;
    }
    glGenTextures(1, &g->id);
    glBindTexture(g->target, g->id);
    GLsizei levels = (GLsizei)r->desc.mips;
    if (g->target == GL_TEXTURE_3D)
        glTexStorage3D(g->target, levels, g->internal, (GLsizei)r->desc.width, (GLsizei)r->desc.height, (GLsizei)r->desc.depth);
    else if (g->target == GL_TEXTURE_2D_ARRAY || g->target == GL_TEXTURE_CUBE_MAP_ARRAY)
        glTexStorage3D(g->target, levels, g->internal, (GLsizei)r->desc.width, (GLsizei)r->desc.height, (GLsizei)r->desc.layers);
    else
        glTexStorage2D(g->target, levels, g->internal, (GLsizei)r->desc.width, (GLsizei)r->desc.height);
    GLenum e = glGetError();
    if (e) { R10_LOG("GLES: texture storage %ux%u %s levels %d: error %04X", r->desc.width, r->desc.height, f->name, levels, e); }
    glTexParameteri(g->target, GL_TEXTURE_MAX_LEVEL, levels - 1);
    g->max_level = levels - 1;
    if (f->gl_r_to_a) {
        glTexParameteri(g->target, GL_TEXTURE_SWIZZLE_R, GL_ZERO); glTexParameteri(g->target, GL_TEXTURE_SWIZZLE_G, GL_ZERO);
        glTexParameteri(g->target, GL_TEXTURE_SWIZZLE_B, GL_ZERO); glTexParameteri(g->target, GL_TEXTURE_SWIZZLE_A, GL_RED);
    } else if (f->gl_alpha_one) glTexParameteri(g->target, GL_TEXTURE_SWIZZLE_A, GL_ONE);
    if (f->kind == R10_FMT_DEPTH) glTexParameteri(g->target, GL_DEPTH_STENCIL_TEXTURE_MODE, GL_DEPTH_COMPONENT);
}

/* A converted texture leaves the pack (r10_pack.c): a new GL texture for its storage. GL keeps the old one
 * for the commands already given; views take the new name where they are bound (bind_textures). */
static int gles_resource_store(r10_device *d, r10_resource *r)
{
    (void)d;
    gl_res *g = res_be(r);
    if (!g || r->desc.dim == R10_DIM_BUFFER || r->desc.usage == D3D10_USAGE_STAGING) return 0;
    gl_current();
    if (g->id) { forget_fbos_of(g->id); glDeleteTextures(1, &g->id); g->id = 0; }
    texture_create(r, g);
    return g->id != 0;
}

static void forget_fbos_of(GLuint tex)
{
    for (unsigned b = 0; b < FBO_BUCKETS; ++b)
        for (fbo_entry **p = &fbos[b]; *p;) {
            int uses = 0;
            for (int i = 0; i < 9; ++i) if ((GLuint)((*p)->key[i] >> 32) == tex && tex) uses = 1;
            if (uses) { fbo_entry *e = *p; *p = e->next; glDeleteFramebuffers(1, &e->fbo); free(e); }
            else p = &(*p)->next;
        }
}

static void gles_resource_destroy(r10_device *d, r10_resource *r)
{
    (void)d;
    gl_res *g = res_be(r);
    if (!g) return;
    gl_current();
    if (r->desc.dim == R10_DIM_BUFFER) { if (g->id) glDeleteBuffers(1, &g->id); }
    else if (g->id) { forget_fbos_of(g->id); glDeleteTextures(1, &g->id); }
    free(g->data);
    free(g);
    r->be = NULL;
}

static void gles_resource_update(r10_device *d, r10_resource *r, uint32_t sub, uint32_t x, uint32_t y, uint32_t z, uint32_t w,
                                 uint32_t h, uint32_t depth, const void *data, uint32_t row_pitch, uint32_t slice_pitch)
{
    (void)d;
    gl_res *g = res_be(r);
    if (!g) return;
    gl_current();
    if (r->desc.dim == R10_DIM_BUFFER) {
        if (g->data) { memcpy(g->data + x, data, w); ++g->cb_version; return; }
        if (r->desc.bind & D3D10_BIND_CONSTANT_BUFFER) return;
        if (!g->id) {
            /* The static upload of a never discarded buffer: its whole content from the game's memory. */
            glGenBuffers(1, &g->id);
            glBindBuffer(GL_COPY_WRITE_BUFFER, g->id);
            const void *all = r->shadow ? GPTR(r->shadow) : NULL;
            glBufferData(GL_COPY_WRITE_BUFFER, r->desc.width, all, GL_STATIC_DRAW);
            if (all) return;
        }
        glBindBuffer(GL_COPY_WRITE_BUFFER, g->id);
        glBufferSubData(GL_COPY_WRITE_BUFFER, x, w, data);
        return;
    }
    if (r->desc.usage == D3D10_USAGE_STAGING || !g->id || !w) return;
    upload_level(r, g, sub, x, y, z, w, h, depth, data, row_pitch, slice_pitch);
}

static GLuint fbo_for(const GLuint *colors, const int *levels, const int *layers, GLenum *targets, int count, GLuint depth,
                      int depth_level, int depth_layer, GLenum depth_target, int depth_stencil);

static int gles_resource_read(r10_device *d, r10_resource *r, uint32_t sub, void *out, uint32_t row_pitch, uint32_t slice_pitch)
{
    (void)d; (void)slice_pitch;
    /* Staging resources hold their bytes in the shadow already (copy() filled it). */
    (void)r; (void)sub; (void)out; (void)row_pitch;
    return 1;
}

/* Reads a 2D subresource of a texture into memory in its Direct3D format. */
static int read_texture(r10_resource *src, uint32_t sub, uint8_t *out, uint32_t pitch)
{
    gl_res *g = res_be(src);
    if (!g || !g->id || g->compressed || g->decoded) return 0;
    uint32_t w, h, dd;
    r10_subresource_size(src, sub, &w, &h, &dd);
    uint32_t mip = sub % src->desc.mips, layer = sub / src->desc.mips;
    glBindFramebuffer(GL_READ_FRAMEBUFFER, read_fbo);
    if (g->target == GL_TEXTURE_2D || g->target == GL_TEXTURE_CUBE_MAP)
        glFramebufferTexture2D(GL_READ_FRAMEBUFFER, src->fmt->kind == R10_FMT_DEPTH ? GL_DEPTH_ATTACHMENT : GL_COLOR_ATTACHMENT0,
                               g->target == GL_TEXTURE_CUBE_MAP ? GL_TEXTURE_CUBE_MAP_POSITIVE_X + layer : GL_TEXTURE_2D, g->id, (GLint)mip);
    else
        glFramebufferTextureLayer(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, g->id, (GLint)mip, (GLint)layer);
    if (src->fmt->kind == R10_FMT_DEPTH) return 0;
    uint32_t texel = src->fmt->gl_swizzle ? 4 : src->fmt->bytes;
    glPixelStorei(GL_PACK_ROW_LENGTH, (GLint)(pitch / texel));
    glReadPixels(0, 0, (GLsizei)w, (GLsizei)h, g->format, g->type, out);
    glPixelStorei(GL_PACK_ROW_LENGTH, 0);
    if (src->fmt->gl_swizzle == 1)
        for (uint32_t y = 0; y < h; ++y)
            for (uint32_t x = 0; x < w; ++x) { uint8_t *p = out + (size_t)y * pitch + 4 * x; uint8_t t = p[0]; p[0] = p[2]; p[2] = t; }
    glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
    return 1;
}

static void gles_copy(r10_device *d, r10_resource *dst, uint32_t dst_sub, uint32_t x, uint32_t y, uint32_t z, r10_resource *src,
                      uint32_t src_sub, const uint32_t *box)
{
    gl_current();
    gl_res *gs = res_be(src), *gd = res_be(dst);
    if (!gs || !gd) return;
    if (dst->desc.dim == R10_DIM_BUFFER) {
        uint32_t sx = box ? box[0] : 0, w = box ? box[3] - box[0] : src->desc.width;
        if (sx > src->desc.width || w > src->desc.width - sx || x > dst->desc.width || w > dst->desc.width - x) return;
        const uint8_t *host = gs->data ? gs->data : src->shadow ? (const uint8_t *)GPTR(src->shadow) : src->host_copy;
        if (gs->id && gd->id) {
            glBindBuffer(GL_COPY_READ_BUFFER, gs->id); glBindBuffer(GL_COPY_WRITE_BUFFER, gd->id);
            glCopyBufferSubData(GL_COPY_READ_BUFFER, GL_COPY_WRITE_BUFFER, sx, x, w);
        } else if (host) {
            if (dst->shadow) memcpy(GPTR(dst->shadow + x), host + sx, w);
            if (dst->host_copy) memcpy(dst->host_copy + x, host + sx, w);
            gles_resource_update(d, dst, 0, x, 0, 0, w, 1, 1, host + sx, w, w);
        }
        return;
    }
    uint32_t w, h, dd;
    r10_subresource_size(src, src_sub, &w, &h, &dd);
    uint32_t sx = 0, sy = 0, sz = 0;
    if (box) { sx = box[0]; sy = box[1]; sz = box[2]; w = box[3] - box[0]; h = box[4] - box[1]; dd = box[5] - box[2]; }
    if (dst->desc.usage == D3D10_USAGE_STAGING) {
        /* GPU -> CPU: the staging texture's shadow receives the texels (Map READ returns them). */
        if (!dst->shadow && !r10_resource_shadow(d, dst)) return;
        if (src->desc.usage == D3D10_USAGE_STAGING && src->shadow) {
            memcpy(GPTR(dst->shadow + dst->sub_offset[dst_sub]), GPTR(src->shadow + src->sub_offset[src_sub]), dst->sub_slice[dst_sub]);
            return;
        }
        if (!box && !x && !y) read_texture(src, src_sub, (uint8_t *)GPTR(dst->shadow + dst->sub_offset[dst_sub]), dst->sub_pitch[dst_sub]);
        return;
    }
    if (src->desc.usage == D3D10_USAGE_STAGING) {
        if (!src->shadow) return;
        const uint8_t *base = (const uint8_t *)GPTR(src->shadow + src->sub_offset[src_sub]);
        uint32_t pitch = src->sub_pitch[src_sub];
        const uint8_t *at = base + (size_t)(sy / src->fmt->block) * pitch + (size_t)(sx / src->fmt->block) * src->fmt->bytes;
        upload_level(dst, gd, dst_sub, x, y, z, w, h, dd, at, pitch, src->sub_slice[src_sub]);
        return;
    }
    if (!gs->id || !gd->id) return;
    uint32_t smip = src_sub % src->desc.mips, slayer = src_sub / src->desc.mips;
    uint32_t dmip = dst_sub % dst->desc.mips, dlayer = dst_sub / dst->desc.mips;
    GLenum st = gs->target == GL_TEXTURE_CUBE_MAP ? GL_TEXTURE_CUBE_MAP : gs->target;
    GLenum dt = gd->target == GL_TEXTURE_CUBE_MAP ? GL_TEXTURE_CUBE_MAP : gd->target;
    glCopyImageSubData(gs->id, st, (GLint)smip, (GLint)sx, (GLint)sy, (GLint)(gs->target == GL_TEXTURE_3D ? sz : slayer),
                       gd->id, dt, (GLint)dmip, (GLint)x, (GLint)y, (GLint)(gd->target == GL_TEXTURE_3D ? z : dlayer),
                       (GLsizei)w, (GLsizei)h, (GLsizei)(gs->target == GL_TEXTURE_3D ? dd : 1));
    GLenum e = glGetError();
    if (e) {
        static unsigned logged;
        if (logged++ < 16) R10_LOG("GLES: copy %s -> %s %ux%u: error %04X", src->fmt->name, dst->fmt->name, w, h, e);
    }
}

static void gles_resolve(r10_device *d, r10_resource *dst, uint32_t dst_sub, r10_resource *src, uint32_t src_sub)
{
    gles_copy(d, dst, dst_sub, 0, 0, 0, src, src_sub, NULL);
}

static void gles_generate_mips(r10_device *d, r10_view *v)
{
    (void)d;
    gl_res *g = res_be(v->res);
    if (!g || !g->id || g->compressed) return;
    gl_current();
    glBindTexture(g->target, g->id);
    glGenerateMipmap(g->target);
}

/* ------------------------------------------------------------------ views, samplers, shaders */
static int gles_view_create(r10_device *d, r10_view *v)
{
    (void)d;
    gl_view *gv = (gl_view *)calloc(1, sizeof *gv);
    gl_res *g = res_be(v->res);
    gv->tex = g ? g->id : 0;
    gv->target = g ? g->target : GL_TEXTURE_2D;
    gv->level = (int)v->first_mip;
    gv->layer = (int)v->first_layer;
    gv->layers = (int)v->layers;
    gv->base = (int)v->first_mip;
    gv->levels = (int)v->mips;
    gv->depth_stencil = v->fmt->stencil;
    v->be = gv;
    return 1;
}
static void gles_view_destroy(r10_device *d, r10_view *v) { (void)d; free(v->be); v->be = NULL; }

static GLenum gl_address(uint32_t a)
{
    switch (a) {
    case D3D10_TEXTURE_ADDRESS_MIRROR: return GL_MIRRORED_REPEAT;
    case D3D10_TEXTURE_ADDRESS_CLAMP: case D3D10_TEXTURE_ADDRESS_MIRROR_ONCE: return GL_CLAMP_TO_EDGE;
    case D3D10_TEXTURE_ADDRESS_BORDER: return GL_CLAMP_TO_BORDER;
    default: return GL_REPEAT;
    }
}
static GLenum gl_compare(uint32_t f)
{
    static const GLenum t[] = { GL_ALWAYS, GL_NEVER, GL_LESS, GL_EQUAL, GL_LEQUAL, GL_GREATER, GL_NOTEQUAL, GL_GEQUAL, GL_ALWAYS };
    return f < 9 ? t[f] : GL_ALWAYS;
}

static int gles_sampler_create(r10_device *d, r10_state_object *s)
{
    (void)d;
    gl_current();
    const r10_sampler_desc *sd = &s->d.sampler;
    gl_sampler *gs = (gl_sampler *)calloc(1, sizeof *gs);
    glGenSamplers(1, &gs->id);
    uint32_t max_anisotropy, f = r10_sampler_filter(sd, &max_anisotropy);
    int anisotropic = (f & 0x40) != 0, compare = (f & 0x80) != 0;
    int min_linear = (f & 0x10) != 0, mag_linear = (f & 0x04) != 0, mip_linear = (f & 0x01) != 0;
    if (anisotropic) min_linear = mag_linear = mip_linear = 1;
    glSamplerParameteri(gs->id, GL_TEXTURE_MIN_FILTER, min_linear ? (mip_linear ? GL_LINEAR_MIPMAP_LINEAR : GL_LINEAR_MIPMAP_NEAREST)
                                                                  : (mip_linear ? GL_NEAREST_MIPMAP_LINEAR : GL_NEAREST_MIPMAP_NEAREST));
    glSamplerParameteri(gs->id, GL_TEXTURE_MAG_FILTER, mag_linear ? GL_LINEAR : GL_NEAREST);
    glSamplerParameteri(gs->id, GL_TEXTURE_WRAP_S, (GLint)gl_address(sd->address_u));
    glSamplerParameteri(gs->id, GL_TEXTURE_WRAP_T, (GLint)gl_address(sd->address_v));
    glSamplerParameteri(gs->id, GL_TEXTURE_WRAP_R, (GLint)gl_address(sd->address_w));
    glSamplerParameterf(gs->id, GL_TEXTURE_MIN_LOD, sd->min_lod);
    glSamplerParameterf(gs->id, GL_TEXTURE_MAX_LOD, sd->max_lod);
    glSamplerParameterfv(gs->id, GL_TEXTURE_BORDER_COLOR, sd->border);
    if (compare) {
        glSamplerParameteri(gs->id, GL_TEXTURE_COMPARE_MODE, GL_COMPARE_REF_TO_TEXTURE);
        glSamplerParameteri(gs->id, GL_TEXTURE_COMPARE_FUNC, (GLint)gl_compare(sd->comparison));
    }
    if (anisotropic && caps.anisotropic) {
        float a = (float)max_anisotropy;
        glSamplerParameterf(gs->id, GL_TEXTURE_MAX_ANISOTROPY_EXT, a < caps.max_anisotropy ? a : caps.max_anisotropy);
    }
    gs->compare = compare;
    if (caps.srgb_decode) {
        glGenSamplers(1, &gs->raw);
        GLint v;
        static const GLenum params[] = { GL_TEXTURE_MIN_FILTER, GL_TEXTURE_MAG_FILTER, GL_TEXTURE_WRAP_S, GL_TEXTURE_WRAP_T, GL_TEXTURE_WRAP_R,
                                         GL_TEXTURE_COMPARE_MODE, GL_TEXTURE_COMPARE_FUNC };
        for (size_t i = 0; i < sizeof params / sizeof params[0]; ++i) { glGetSamplerParameteriv(gs->id, params[i], &v); glSamplerParameteri(gs->raw, params[i], v); }
        glSamplerParameterf(gs->raw, GL_TEXTURE_MIN_LOD, sd->min_lod);
        glSamplerParameterf(gs->raw, GL_TEXTURE_MAX_LOD, sd->max_lod);
        glSamplerParameterfv(gs->raw, GL_TEXTURE_BORDER_COLOR, sd->border);
        if (anisotropic && caps.anisotropic) { float a; glGetSamplerParameterfv(gs->id, GL_TEXTURE_MAX_ANISOTROPY_EXT, &a); glSamplerParameterf(gs->raw, GL_TEXTURE_MAX_ANISOTROPY_EXT, a); }
        glSamplerParameteri(gs->raw, GL_TEXTURE_SRGB_DECODE_EXT, GL_SKIP_DECODE_EXT);
    }
    if (!compare && (min_linear || mag_linear)) {
        glGenSamplers(1, &gs->nearest);
        glSamplerParameteri(gs->nearest, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glSamplerParameteri(gs->nearest, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glSamplerParameteri(gs->nearest, GL_TEXTURE_WRAP_S, (GLint)gl_address(sd->address_u));
        glSamplerParameteri(gs->nearest, GL_TEXTURE_WRAP_T, (GLint)gl_address(sd->address_v));
        glSamplerParameteri(gs->nearest, GL_TEXTURE_WRAP_R, (GLint)gl_address(sd->address_w));
    }
    s->be = gs;
    return 1;
}
static void gles_sampler_destroy(r10_device *d, r10_state_object *s)
{
    (void)d;
    gl_sampler *gs = (gl_sampler *)s->be;
    if (gs) {
        gl_current();
        glDeleteSamplers(1, &gs->id);
        if (gs->raw) glDeleteSamplers(1, &gs->raw);
        if (gs->nearest) glDeleteSamplers(1, &gs->nearest);
        free(gs);
    }
    s->be = NULL;
}

/* Dynamic indexing can require a 64 KiB block even on a driver limited to 16 KiB. */
static int shader_texture_cb0(const r10_lib_shader *lib)
{
    return cb0_textures[lib->type] && lib->es_cb0_bytes > (uint32_t)caps.max_uniform_block;
}

static GLenum shader_texture_target(const r10_lib_shader *lib, int slot)
{
    const r10_lib_binding *bindings = r10_lib_at(lib->bindings);
    uint32_t binding = lib->type * 160u + 32u + (uint32_t)slot;
    for (uint32_t i = 0; slot >= 0 && i < lib->binding_count; ++i) {
        if (bindings[i].binding != binding) continue;
        switch (bindings[i].view_type) {
        case VK_IMAGE_VIEW_TYPE_CUBE: return GL_TEXTURE_CUBE_MAP;
        case VK_IMAGE_VIEW_TYPE_CUBE_ARRAY: return GL_TEXTURE_CUBE_MAP_ARRAY;
        case VK_IMAGE_VIEW_TYPE_3D: return GL_TEXTURE_3D;
        case VK_IMAGE_VIEW_TYPE_1D_ARRAY: case VK_IMAGE_VIEW_TYPE_2D_ARRAY: return GL_TEXTURE_2D_ARRAY;
        case VK_IMAGE_VIEW_TYPE_MAX_ENUM: return GL_TEXTURE_BUFFER;
        default: break;
        }
        break;
    }
    return GL_TEXTURE_2D;
}

static GLuint compile_game_shader(const r10_lib_shader *lib)
{
    char *text = r10_shaderlib_glsl(lib);
    const char *body = text ? strchr(text, '\n') : NULL;
    if (!body) { free(text); return 0; }
    char defines[96];
    snprintf(defines, sizeof defines, "%s%s#define BS_CB0_UNIT %d\n",
             caps.clip_distance ? "#define BS_CLIP_DISTANCE\n" : "",
             shader_texture_cb0(lib) ? "#define BS_CB0_TEXTURE\n" : "", caps.max_units - 2 + lib->type);
    const char *sources[] = { "#version 320 es\n", defines, body + 1 };
    char what[64];
    snprintf(what, sizeof what, "%s %016llx", lib->type ? "vertex shader" : "pixel shader", (unsigned long long)lib->hash);
    uint64_t start = r10_now_ns();
    GLuint id = compile_sources(lib->type ? GL_VERTEX_SHADER : GL_FRAGMENT_SHADER, 3, sources, what);
    void_perf_add(VOID_PERF_SHADER_COMPILE, r10_now_ns() - start, 0);
    free(text);
    return id;
}

static int gles_shader_create(r10_device *d, r10_shader *s)
{
    (void)d;
    if (s->type > 1) return 0;
    gl_shader *gs = calloc(1, sizeof *gs + s->lib->combined_count * sizeof *gs->texture_targets);
    if (!gs) return 0;
    gs->texture_cb0 = shader_texture_cb0(s->lib);
    gs->uses_vertex_id = (s->lib->flags & R10_LIB_VERTEX_ID) != 0;
    const r10_lib_combined *combined = r10_lib_at(s->lib->combined);
    for (uint32_t i = 0; i < s->lib->combined_count; ++i)
        gs->texture_targets[i] = shader_texture_target(s->lib, combined[i].texture);
    if (s->lib->icb_size) {
        gl_current();
        glGenBuffers(1, &gs->icb);
        glBindBuffer(GL_UNIFORM_BUFFER, gs->icb);
        glBufferData(GL_UNIFORM_BUFFER, s->lib->icb_size, r10_lib_at(s->lib->icb), GL_STATIC_DRAW);
    }
    s->be = gs;
    return 1;
}

static void gles_shader_destroy(r10_device *d, r10_shader *s)
{
    (void)d;
    gl_shader *gs = (gl_shader *)s->be;
    if (!gs) return;
    if (gs->id || gs->icb || gs->has_program) gl_current();
    if (gs->has_program)
        for (unsigned b = 0; b < PROGRAM_BUCKETS; ++b)
            for (gl_program **p = &programs[b]; *p;) {
                if ((*p)->vs == s || (*p)->ps == s) { gl_program *e = *p; *p = e->next; glDeleteProgram(e->id); free(e); }
                else p = &(*p)->next;
            }
    if (gs->id) glDeleteShader(gs->id);
    if (gs->icb) glDeleteBuffers(1, &gs->icb);
    free(gs);
    s->be = NULL;
}

static const char *program_cache_path(char path[1280], uint64_t vs, uint64_t ps)
{
    int n = program_cache_dir[0] ? snprintf(path, 1280,
        "%s/r10-es-%016llx-%016llx-%016llx-%016llx.bin", program_cache_dir,
        (unsigned long long)program_driver_key, (unsigned long long)r10_shaderlib_key(),
        (unsigned long long)vs, (unsigned long long)ps) : -1;
    return n > 0 && n < 1280 ? path : NULL;
}

/* A prepared program is handed off to the normal shader lifetime on its first draw. */
static struct warm_program { r10_warm_pair shader; GLuint id; } warmed[R10_WARM_MAX];
static uint32_t warmed_count;
static int record_warm_programs;

static void release_warm_programs(void)
{
    if (!warmed_count) return;
    gl_current();
    while (warmed_count) glDeleteProgram(warmed[--warmed_count].id);
}

static GLuint take_warm_program(uint64_t vs, uint64_t ps)
{
    for (uint32_t i = 0; i < warmed_count; ++i) {
        if (warmed[i].shader.vs == vs && warmed[i].shader.ps == ps) {
            GLuint id = warmed[i].id;
            warmed[i] = warmed[--warmed_count];
            return id;
        }
    }
    return 0;
}

static GLuint warm_shader(const r10_lib_shader *lib, GLuint *stages)
{
    GLuint *id = &stages[r10_shaderlib_index(lib)];
    if (!*id) *id = compile_game_shader(lib);
    return *id;
}

static void warm_programs(void)
{
    r10_warmup seed;
    int debug = r10_env_int("BIOSHOCK_R10_DEBUG", 0, 0, INT32_MAX);
    record_warm_programs = (debug & R10_DEBUG_RECORD_WARMUP) != 0;
    if (record_warm_programs || (debug & R10_DEBUG_NO_WARMUP) || !r10_shaderlib_warmup(&seed)) return;
    uint32_t stage_count = r10_shaderlib_count();
    GLuint *stages = calloc(stage_count, sizeof *stages);
    if (!stages) return;
    uint64_t start = r10_now_ns();
    uint64_t deadline = start + 4000000000ull;
    unsigned char cached[R10_WARM_MAX] = {0};
    /* A few slow links early in the list must not consume time needed to load existing binaries. */
    for (uint32_t i = 0; program_cache_dir[0] && i < seed.pair_count && r10_now_ns() < deadline; ++i) {
        r10_warm_pair pair;
        memcpy(&pair, seed.pairs + i * sizeof pair, sizeof pair);
        char path[1280];
        const char *cache = program_cache_path(path, pair.vs, pair.ps);
        GLuint id = cache ? load_program_binary(cache) : 0;
        if (id) {
            cached[i] = 1;
            warmed[warmed_count++] = (struct warm_program){pair, id};
        }
    }
    unsigned cache_hits = warmed_count;
    for (uint32_t i = 0; i < seed.pair_count && r10_now_ns() < deadline; ++i) {
        if (cached[i]) continue;
        r10_warm_pair pair;
        memcpy(&pair, seed.pairs + i * sizeof pair, sizeof pair);
        const r10_lib_shader *vs = r10_shaderlib_by_hash(pair.vs, 1), *ps = r10_shaderlib_by_hash(pair.ps, 0);
        if (!vs || (pair.ps && !ps)) continue;
        char path[1280];
        const char *cache = program_cache_path(path, pair.vs, pair.ps);
        GLuint v = warm_shader(vs, stages), f = ps ? warm_shader(ps, stages) : null_fragment;
        GLuint id = v && f ? link_program(v, f, "startup program", cache) : 0;
        if (id && cache) save_program_binary(id, cache);
        if (id) warmed[warmed_count++] = (struct warm_program){pair, id};
    }
    unsigned compiled = 0;
    for (uint32_t i = 0; i < stage_count; ++i) if (stages[i]) { glDeleteShader(stages[i]); ++compiled; }
    free(stages);
    R10_LOG("GLES: warmed %u/%u programs during startup in %.1f ms, %u from binary cache, %u compiled shader stages",
            warmed_count, seed.pair_count, (r10_now_ns() - start) / 1e6, cache_hits, compiled);
}

static GLuint shader_id(const r10_shader *s, gl_shader *gs)
{
    if (!gs->id && !gs->compile_failed) {
        gs->id = compile_game_shader(s->lib);
        gs->compile_failed = !gs->id;
    }
    return gs->id;
}

static gl_program *program_for(r10_shader *vs, r10_shader *ps)
{
    unsigned b = (unsigned)((((uintptr_t)vs >> 4) * 31u) ^ ((uintptr_t)ps >> 4)) % PROGRAM_BUCKETS;
    for (gl_program *p = programs[b]; p; p = p->next) if (p->vs == vs && p->ps == ps) return p;
    gl_shader *v = (gl_shader *)vs->be, *f = ps ? (gl_shader *)ps->be : NULL;
    if (!v || (ps && !f)) return NULL;
    gl_program *p = (gl_program *)calloc(1, sizeof *p);
    if (!p) return NULL;
    p->vs = vs; p->ps = ps;
    v->has_program = 1;
    if (f) f->has_program = 1;
    p->id = take_warm_program(vs->hash, ps ? ps->hash : 0);
    if (!p->id) {
        char path[1280];
        const char *cache = program_cache_path(path, vs->hash, ps ? ps->hash : 0);
        if (cache) {
            uint64_t start = r10_now_ns();
            void_machine_lend_begin();
            p->id = load_program_binary(cache);
            void_machine_lend_end();
            if (p->id) void_perf_add(VOID_PERF_PROGRAM_CACHE, r10_now_ns() - start, 0);
        }
        if (!p->id) {
            GLuint vert = shader_id(vs, v), frag = f ? shader_id(ps, f) : null_fragment;
            if (vert && frag) {
                char what[96];
                snprintf(what, sizeof what, "program %016llx + %016llx", (unsigned long long)vs->hash, (unsigned long long)(ps ? ps->hash : 0));
                uint64_t start = r10_now_ns();
                void_machine_lend_begin();
                p->id = link_program(vert, frag, what, cache);
                if (p->id && cache) save_program_binary(p->id, cache);
                void_machine_lend_end();
                void_perf_add(VOID_PERF_PROGRAM_LINK, r10_now_ns() - start, 0);
            }
        }
        if (r10_dev) ++r10_dev->stats.pipelines;
    }
    if (record_warm_programs && r10_dev && p->id)
        R10_LOG("GLES first use frame %llu: vs %016llx ps %016llx", (unsigned long long)r10_dev->frame,
                (unsigned long long)vs->hash, (unsigned long long)(ps ? ps->hash : 0));
    p->next = programs[b];
    programs[b] = p;
    return p;
}

/* ------------------------------------------------------------------ framebuffers */
static GLuint fbo_for(const GLuint *colors, const int *levels, const int *layers, GLenum *targets, int count, GLuint depth,
                      int depth_level, int depth_layer, GLenum depth_target, int depth_stencil)
{
    uint64_t key[9] = { 0 };
    for (int i = 0; i < count; ++i)
        key[i] = (uint64_t)colors[i] << 32 | (uint64_t)(uint32_t)levels[i] << 16 | (uint64_t)(uint32_t)(layers[i] & 0xFFFF);
    key[8] = (uint64_t)depth << 32 | (uint64_t)(uint32_t)depth_level << 16 | (uint64_t)(uint32_t)(depth_layer & 0xFFFF);
    uint64_t h = 1469598103934665603ull;
    for (int i = 0; i < 9; ++i) { h ^= key[i]; h *= 1099511628211ull; }
    unsigned b = (unsigned)(h % FBO_BUCKETS);
    for (fbo_entry *e = fbos[b]; e; e = e->next) if (!memcmp(e->key, key, sizeof key)) return e->fbo;
    fbo_entry *e = (fbo_entry *)calloc(1, sizeof *e);
    memcpy(e->key, key, sizeof key);
    glGenFramebuffers(1, &e->fbo);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, e->fbo);
    GLenum bufs[8];
    for (int i = 0; i < 8; ++i) bufs[i] = GL_NONE;
    for (int i = 0; i < count; ++i) {
        if (!colors[i]) continue;
        if (targets[i] == GL_TEXTURE_2D || targets[i] == GL_TEXTURE_CUBE_MAP)
            glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0 + i,
                                   targets[i] == GL_TEXTURE_CUBE_MAP ? GL_TEXTURE_CUBE_MAP_POSITIVE_X + layers[i] : GL_TEXTURE_2D,
                                   colors[i], levels[i]);
        else glFramebufferTextureLayer(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0 + i, colors[i], levels[i], layers[i]);
        bufs[i] = GL_COLOR_ATTACHMENT0 + i;
    }
    if (depth) {
        GLenum point = depth_stencil ? GL_DEPTH_STENCIL_ATTACHMENT : GL_DEPTH_ATTACHMENT;
        if (depth_target == GL_TEXTURE_2D || depth_target == GL_TEXTURE_CUBE_MAP)
            glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, point,
                                   depth_target == GL_TEXTURE_CUBE_MAP ? GL_TEXTURE_CUBE_MAP_POSITIVE_X + depth_layer : GL_TEXTURE_2D,
                                   depth, depth_level);
        else glFramebufferTextureLayer(GL_DRAW_FRAMEBUFFER, point, depth, depth_level, depth_layer);
    }
    glDrawBuffers(count ? count : 1, bufs);
    GLenum status = glCheckFramebufferStatus(GL_DRAW_FRAMEBUFFER);
    if (status != GL_FRAMEBUFFER_COMPLETE) R10_LOG("GLES: framebuffer incomplete %04X (%d colors, depth %u)", status, count, depth);
    e->next = fbos[b];
    fbos[b] = e;
    return e->fbo;
}

static GLuint view_fbo(r10_view **rtv, uint32_t count, r10_view *dsv)
{
    GLuint colors[8]; int levels[8], layers[8]; GLenum targets[8];
    for (uint32_t i = 0; i < count; ++i) {
        gl_view *v = rtv[i] ? (gl_view *)rtv[i]->be : NULL;
        colors[i] = v ? v->tex : 0; levels[i] = v ? v->level : 0; layers[i] = v ? v->layer : 0;
        targets[i] = v ? v->target : GL_TEXTURE_2D;
    }
    gl_view *dv = dsv ? (gl_view *)dsv->be : NULL;
    return fbo_for(colors, levels, layers, targets, (int)count, dv ? dv->tex : 0, dv ? dv->level : 0, dv ? dv->layer : 0,
                   dv ? dv->target : GL_TEXTURE_2D, dv ? dv->depth_stencil : 0);
}

/* ------------------------------------------------------------------ draw state */
static GLenum gl_blend(uint32_t b)
{
    switch (b) {
    case D3D10_BLEND_ZERO: return GL_ZERO;
    case D3D10_BLEND_ONE: return GL_ONE;
    case D3D10_BLEND_SRC_COLOR: return GL_SRC_COLOR;
    case D3D10_BLEND_INV_SRC_COLOR: return GL_ONE_MINUS_SRC_COLOR;
    case D3D10_BLEND_SRC_ALPHA: return GL_SRC_ALPHA;
    case D3D10_BLEND_INV_SRC_ALPHA: return GL_ONE_MINUS_SRC_ALPHA;
    case D3D10_BLEND_DEST_ALPHA: return GL_DST_ALPHA;
    case D3D10_BLEND_INV_DEST_ALPHA: return GL_ONE_MINUS_DST_ALPHA;
    case D3D10_BLEND_DEST_COLOR: return GL_DST_COLOR;
    case D3D10_BLEND_INV_DEST_COLOR: return GL_ONE_MINUS_DST_COLOR;
    case D3D10_BLEND_SRC_ALPHA_SAT: return GL_SRC_ALPHA_SATURATE;
    case D3D10_BLEND_BLEND_FACTOR: return GL_CONSTANT_COLOR;
    case D3D10_BLEND_INV_BLEND_FACTOR: return GL_ONE_MINUS_CONSTANT_COLOR;
    default: return GL_ONE;
    }
}
static GLenum gl_blend_alpha(uint32_t b)
{
    if (b == D3D10_BLEND_SRC_COLOR) return GL_SRC_ALPHA;
    if (b == D3D10_BLEND_INV_SRC_COLOR) return GL_ONE_MINUS_SRC_ALPHA;
    if (b == D3D10_BLEND_DEST_COLOR) return GL_DST_ALPHA;
    if (b == D3D10_BLEND_INV_DEST_COLOR) return GL_ONE_MINUS_DST_ALPHA;
    if (b == D3D10_BLEND_BLEND_FACTOR) return GL_CONSTANT_ALPHA;
    if (b == D3D10_BLEND_INV_BLEND_FACTOR) return GL_ONE_MINUS_CONSTANT_ALPHA;
    return gl_blend(b);
}
static GLenum gl_blend_op(uint32_t op)
{
    switch (op) {
    case D3D10_BLEND_OP_SUBTRACT: return GL_FUNC_SUBTRACT;
    case D3D10_BLEND_OP_REV_SUBTRACT: return GL_FUNC_REVERSE_SUBTRACT;
    case D3D10_BLEND_OP_MIN: return GL_MIN;
    case D3D10_BLEND_OP_MAX: return GL_MAX;
    default: return GL_FUNC_ADD;
    }
}
static GLenum gl_stencil_op(uint32_t op)
{
    static const GLenum t[] = { GL_KEEP, GL_KEEP, GL_ZERO, GL_REPLACE, GL_INCR, GL_DECR, GL_INVERT, GL_INCR_WRAP, GL_DECR_WRAP };
    return op < 9 ? t[op] : GL_KEEP;
}

static const r10_blend_desc default_blend = { 0, { 0 }, D3D10_BLEND_ONE, D3D10_BLEND_ZERO, D3D10_BLEND_OP_ADD, D3D10_BLEND_ONE,
                                              D3D10_BLEND_ZERO, D3D10_BLEND_OP_ADD, { 15, 15, 15, 15, 15, 15, 15, 15 } };
static const r10_depth_desc default_depth = { 1, 1, D3D10_COMPARISON_LESS, 0, 0xFF, 0xFF, { 0, 0 },
                                              { 1, 1, 1, D3D10_COMPARISON_ALWAYS }, { 1, 1, 1, D3D10_COMPARISON_ALWAYS } };
static const r10_raster_desc default_raster = { D3D10_FILL_SOLID, D3D10_CULL_BACK, 0, 0, 0, 0, 1, 0, 0, 0 };

static struct {
    r10_state_object *blend, *depth, *raster;
    uint32_t stencil_ref;
    int valid;
} applied;

static void apply_fixed_state(r10_device *d)
{
    r10_state *st = &d->st;
    if (!applied.valid || applied.blend != st->blend || (st->dirty & (R10_DIRTY_BLEND | R10_DIRTY_BLEND_FACTOR))) {
        const r10_blend_desc *b = st->blend ? &st->blend->d.blend : &default_blend;
        for (int i = 0; i < 8 && i < caps.max_draw_buffers; ++i) {
            if (b->enable[i]) {
                glEnablei(GL_BLEND, (GLuint)i);
                glBlendFuncSeparatei((GLuint)i, gl_blend(b->src), gl_blend(b->dst), gl_blend_alpha(b->src_alpha), gl_blend_alpha(b->dst_alpha));
                glBlendEquationSeparatei((GLuint)i, gl_blend_op(b->op), gl_blend_op(b->op_alpha));
            } else glDisablei(GL_BLEND, (GLuint)i);
            uint8_t m = b->mask[i];
            glColorMaski((GLuint)i, (m & 1) != 0, (m & 2) != 0, (m & 4) != 0, (m & 8) != 0);
        }
        if (b->alpha_to_coverage) glEnable(GL_SAMPLE_ALPHA_TO_COVERAGE); else glDisable(GL_SAMPLE_ALPHA_TO_COVERAGE);
        glBlendColor(st->blend_factor[0], st->blend_factor[1], st->blend_factor[2], st->blend_factor[3]);
        applied.blend = st->blend;
    }
    if (!applied.valid || applied.depth != st->depth || applied.stencil_ref != st->stencil_ref) {
        const r10_depth_desc *z = st->depth ? &st->depth->d.depth : &default_depth;
        int has_depth = st->dsv != NULL;
        if (z->depth_enable && has_depth) { glEnable(GL_DEPTH_TEST); glDepthFunc(gl_compare(z->depth_func)); }
        else glDisable(GL_DEPTH_TEST);
        glDepthMask(z->depth_write_mask && has_depth ? GL_TRUE : GL_FALSE);
        if (z->stencil_enable && has_depth && st->dsv->fmt->stencil) {
            glEnable(GL_STENCIL_TEST);
            /* Direct3D's front faces are OpenGL's back faces' opposite only through glFrontFace (below). */
            glStencilFuncSeparate(GL_FRONT, gl_compare(z->front.func), (GLint)st->stencil_ref, z->stencil_read_mask);
            glStencilFuncSeparate(GL_BACK, gl_compare(z->back.func), (GLint)st->stencil_ref, z->stencil_read_mask);
            glStencilOpSeparate(GL_FRONT, gl_stencil_op(z->front.fail), gl_stencil_op(z->front.depth_fail), gl_stencil_op(z->front.pass));
            glStencilOpSeparate(GL_BACK, gl_stencil_op(z->back.fail), gl_stencil_op(z->back.depth_fail), gl_stencil_op(z->back.pass));
            glStencilMask(z->stencil_write_mask);
        } else glDisable(GL_STENCIL_TEST);
        applied.depth = st->depth;
        applied.stencil_ref = st->stencil_ref;
    }
    if (!applied.valid || applied.raster != st->raster) {
        const r10_raster_desc *r = st->raster ? &st->raster->d.raster : &default_raster;
        if (r->cull == D3D10_CULL_NONE) glDisable(GL_CULL_FACE);
        else { glEnable(GL_CULL_FACE); glCullFace(r->cull == D3D10_CULL_FRONT ? GL_FRONT : GL_BACK); }
        glFrontFace(r->front_ccw ? GL_CW : GL_CCW);
        if (r->depth_bias || r->slope_scaled_depth_bias != 0.0f) {
            glEnable(GL_POLYGON_OFFSET_FILL);
            glPolygonOffset(r->slope_scaled_depth_bias, (float)r->depth_bias);
        } else glDisable(GL_POLYGON_OFFSET_FILL);
        if (r->scissor) glEnable(GL_SCISSOR_TEST); else glDisable(GL_SCISSOR_TEST);
        if (caps.depth_clamp) { if (!r->depth_clip) glEnable(GL_DEPTH_CLAMP_EXT); else glDisable(GL_DEPTH_CLAMP_EXT); }
        applied.raster = st->raster;
    }
    applied.valid = 1;
}

/* Bytes [lo, hi) of a dynamic buffer into its ring slice; returns the ring offset of byte 0. */
static uint32_t stream_buffer(r10_resource *r, uint32_t lo, uint32_t hi)
{
    gl_res *g = res_be(r);
    const uint8_t *src = r10_buffer_shadow(r);
    if (!src) src = r->host_copy;
    if (hi > r->desc.width) hi = r->desc.width;
    if (lo >= hi) lo = hi = 0;
    if (g->slice_version != r->version || g->slice_generation + RING_SEGMENTS - 1 <= ring_generation || g->slice_offset == UINT32_MAX) {
        g->slice_offset = ring_alloc(r->desc.width, 256);
        g->slice_version = r->version;
        g->slice_generation = ring_generation;
        g->slice_lo = g->slice_hi = 0;
        if (g->slice_offset == UINT32_MAX) return UINT32_MAX;
    }
    if (src && hi > lo) {
        if (g->slice_hi <= g->slice_lo) { ring_write(g->slice_offset + lo, src + lo, hi - lo); g->slice_lo = lo; g->slice_hi = hi; }
        else {
            if (lo < g->slice_lo) { ring_write(g->slice_offset + lo, src + lo, g->slice_lo - lo); g->slice_lo = lo; }
            if (hi > g->slice_hi) { ring_write(g->slice_offset + g->slice_hi, src + g->slice_hi, hi - g->slice_hi); g->slice_hi = hi; }
        }
        if (r10_dev) r10_dev->stats.uploads_kb += (hi - lo) >> 10;
    }
    return g->slice_offset;
}

static void bind_constant_buffers(r10_stage_state *stage, const r10_shader *s)
{
    if (!s || !s->lib) return;
    const gl_shader *gs = (const gl_shader *)s->be;
    const r10_lib_ubo *ubos = (const r10_lib_ubo *)r10_lib_at(s->lib->ubos);
    for (uint32_t i = 0; i < s->lib->ubo_count; ++i) {
        uint32_t slot = ubos[i].slot, binding = ubos[i].binding;
        if (slot == 14 && s->lib->icb_size) {
            glBindBufferRange(GL_UNIFORM_BUFFER, binding, gs->icb, 0, s->lib->icb_size);
            continue;
        }
        r10_resource *cb = slot < R10_CB_SLOTS ? stage->cb[slot] : NULL;
        if (!cb) continue;
        gl_res *g = res_be(cb);
        const uint8_t *data = g->data ? g->data : r10_buffer_shadow(cb);
        if (!data) continue;
        /* A constant buffer's bytes go into the ring once per version and ring segment. */
        uint64_t version = g->data ? g->cb_version : cb->version;
        if (!g->cb_generation || g->cb_version != version || g->cb_generation + RING_SEGMENTS - 1 <= ring_generation) {
            uint32_t at = ring_alloc(cb->desc.width, (uint32_t)ubo_align);
            if (at == UINT32_MAX) continue;
            ring_write(at, data, cb->desc.width);
            g->cb_offset = at;
            g->cb_generation = ring_generation;
            g->cb_version = version;
        }
        if (slot == 0 && gs->texture_cb0) {
            glActiveTexture(GL_TEXTURE0 + caps.max_units - 2 + s->type);
            glBindTexture(GL_TEXTURE_BUFFER, cb0_textures[s->type]);
            glTexBufferRange(GL_TEXTURE_BUFFER, GL_RGBA32UI, ring_buffer, g->cb_offset, cb->desc.width);
        } else {
            glBindBufferRange(GL_UNIFORM_BUFFER, binding, ring_buffer, g->cb_offset, cb->desc.width);
        }
    }
}

static void bind_textures(r10_stage_state *stage, const r10_shader *s)
{
    if (!s || !s->lib) return;
    const gl_shader *shader = s->be;
    const r10_lib_combined *c = (const r10_lib_combined *)r10_lib_at(s->lib->combined);
    for (uint32_t i = 0; i < s->lib->combined_count; ++i) {
        r10_view *v = c[i].texture >= 0 ? stage->srv[c[i].texture] : NULL;
        gl_view *gv = v ? (gl_view *)v->be : NULL;
        gl_res *vg = v ? res_be(v->res) : NULL;
        /* A texture that left the converted pack has a new name (gles_resource_store). */
        if (gv && vg && vg->id && gv->tex != vg->id && v->res->desc.dim != R10_DIM_BUFFER) { gv->tex = vg->id; gv->target = vg->target; }
        glActiveTexture(GL_TEXTURE0 + c[i].unit);
        if (gv && gv->tex) {
            glBindTexture(gv->target, gv->tex);
            gl_res *g = res_be(v->res);
            int base = gv->base, max = gv->base + gv->levels - 1;
            if (g->base_level != base) { glTexParameteri(gv->target, GL_TEXTURE_BASE_LEVEL, base); g->base_level = base; }
            if (g->max_level != max) { glTexParameteri(gv->target, GL_TEXTURE_MAX_LEVEL, max); g->max_level = max; }
        } else {
            /* A texture unit keeps independent bindings for 2D textures, cube maps and other targets. */
            glBindTexture(shader->texture_targets[i], 0);
        }
        r10_state_object *smp = c[i].sampler >= 0 ? stage->sampler[c[i].sampler] : NULL;
        gl_sampler *gs = smp ? (gl_sampler *)smp->be : NULL;
        GLuint id = gs ? gs->id : 0;
        if (gs && v && v->fmt->kind == R10_FMT_DEPTH && !gs->compare && gs->nearest) id = gs->nearest;
        else if (gs && gs->raw && v && v->res->fmt->srgb && void_bioshock_srgb_textures && !void_bioshock_srgb_textures()) id = gs->raw;
        glBindSampler(c[i].unit, id);
    }
}

static void vertex_format(uint32_t dxgi, GLint *size, GLenum *type, GLboolean *norm, int *integer)
{
    *integer = 0; *norm = GL_FALSE;
    switch (dxgi) {
    case 2: *size = 4; *type = GL_FLOAT; return;
    case 3: *size = 4; *type = GL_UNSIGNED_INT; *integer = 1; return;
    case 4: *size = 4; *type = GL_INT; *integer = 1; return;
    case 6: *size = 3; *type = GL_FLOAT; return;
    case 7: *size = 3; *type = GL_UNSIGNED_INT; *integer = 1; return;
    case 8: *size = 3; *type = GL_INT; *integer = 1; return;
    case 10: *size = 4; *type = GL_HALF_FLOAT; return;
    case 11: *size = 4; *type = GL_UNSIGNED_SHORT; *norm = GL_TRUE; return;
    case 12: *size = 4; *type = GL_UNSIGNED_SHORT; *integer = 1; return;
    case 13: *size = 4; *type = GL_SHORT; *norm = GL_TRUE; return;
    case 14: *size = 4; *type = GL_SHORT; *integer = 1; return;
    case 16: *size = 2; *type = GL_FLOAT; return;
    case 17: *size = 2; *type = GL_UNSIGNED_INT; *integer = 1; return;
    case 18: *size = 2; *type = GL_INT; *integer = 1; return;
    case 24: *size = 4; *type = GL_UNSIGNED_INT_2_10_10_10_REV; *norm = GL_TRUE; return;
    case 28: *size = 4; *type = GL_UNSIGNED_BYTE; *norm = GL_TRUE; return;
    case 30: *size = 4; *type = GL_UNSIGNED_BYTE; *integer = 1; return;
    case 31: *size = 4; *type = GL_BYTE; *norm = GL_TRUE; return;
    case 32: *size = 4; *type = GL_BYTE; *integer = 1; return;
    case 34: *size = 2; *type = GL_HALF_FLOAT; return;
    case 35: *size = 2; *type = GL_UNSIGNED_SHORT; *norm = GL_TRUE; return;
    case 36: *size = 2; *type = GL_UNSIGNED_SHORT; *integer = 1; return;
    case 37: *size = 2; *type = GL_SHORT; *norm = GL_TRUE; return;
    case 38: *size = 2; *type = GL_SHORT; *integer = 1; return;
    case 41: *size = 1; *type = GL_FLOAT; return;
    case 42: *size = 1; *type = GL_UNSIGNED_INT; *integer = 1; return;
    case 43: *size = 1; *type = GL_INT; *integer = 1; return;
    case 49: *size = 2; *type = GL_UNSIGNED_BYTE; *norm = GL_TRUE; return;
    case 54: *size = 1; *type = GL_HALF_FLOAT; return;
    case 87: *size = 4; *type = GL_UNSIGNED_BYTE; *norm = GL_TRUE; return;   /* BGRA: no ES equivalent (see draw) */
    default: *size = 4; *type = GL_FLOAT; return;
    }
}

static GLenum gl_mode(uint32_t topology)
{
    switch (topology) {
    case D3D10_PRIMITIVE_TOPOLOGY_POINTLIST: return GL_POINTS;
    case D3D10_PRIMITIVE_TOPOLOGY_LINELIST: return GL_LINES;
    case D3D10_PRIMITIVE_TOPOLOGY_LINESTRIP: return GL_LINE_STRIP;
    case D3D10_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP: return GL_TRIANGLE_STRIP;
    default: return GL_TRIANGLES;
    }
}

static GLuint bound_fbo = 0xFFFFFFFFu;
static uint32_t enabled_attribs;
static void gpu_timer_frame(r10_device *d, int end);

static void bind_targets(r10_device *d)
{
    GLuint fbo = view_fbo(d->st.rtv, d->st.rtv_count, d->st.dsv);
    if (fbo != bound_fbo) { glBindFramebuffer(GL_DRAW_FRAMEBUFFER, fbo); bound_fbo = fbo; }
}

static void gles_draw(r10_device *d, const r10_draw *dr)
{
    gl_current();
    r10_state *st = &d->st;
    r10_shader *vs = st->vs.shader, *ps = st->ps.shader;
    gl_program *prog = program_for(vs, ps);
    if (!prog || !prog->id) return;
    if (st->dirty & R10_DIRTY_TARGETS) { bind_targets(d); applied.valid = 0; }
    else if (bound_fbo == 0xFFFFFFFFu) bind_targets(d);
    gpu_timer_frame(d, 0);
    glUseProgram(prog->id);
    apply_fixed_state(d);
    if (st->dirty & (R10_DIRTY_VIEWPORT | R10_DIRTY_TARGETS)) {
        const r10_viewport *v = &st->viewport[0];
        if (st->viewports) { glViewport(v->x, v->y, (GLsizei)v->w, (GLsizei)v->h); glDepthRangef(v->minz, v->maxz); }
    }
    if (st->dirty & (R10_DIRTY_SCISSOR | R10_DIRTY_TARGETS)) {
        const r10_rect *s = &st->scissor[0];
        if (st->scissors) glScissor(s->l, s->t, s->r - s->l, s->b - s->t);
    }
    if (caps.clip_distance) {
        static int clip_on;
        int want = vs->lib && (vs->lib->flags & R10_LIB_CLIP_DISTANCE);
        if (want != clip_on) { if (want) glEnable(GL_CLIP_DISTANCE0_EXT); else glDisable(GL_CLIP_DISTANCE0_EXT); clip_on = want; }
    }
    bind_constant_buffers(&st->vs, vs);
    bind_constant_buffers(&st->ps, ps);
    bind_textures(&st->vs, vs);
    bind_textures(&st->ps, ps);
    /* Vertex input */
    r10_layout *l = st->layout;
    int32_t base_vertex = dr->base_vertex;
    /* Mali T-series and G51 can fault on a nonzero indexed base vertex. */
    int shift_base = caps.emulate_base_vertex && dr->indexed && base_vertex > 0 &&
                     !((gl_shader *)vs->be)->uses_vertex_id;
    uint32_t want = 0;
    if (l) {
        for (uint32_t i = 0; i < l->count; ++i) {
            const r10_element *e = &l->el[i];
            GLint size; GLenum type; GLboolean norm; int integer;
            vertex_format(e->format, &size, &type, &norm, &integer);
            if (integer) glVertexAttribIFormat(e->location, size, type, e->offset);
            else glVertexAttribFormat(e->location, size, type, norm, e->offset);
            glVertexAttribBinding(e->location, e->slot);
            want |= 1u << e->location;
        }
        for (uint32_t slot = 0; slot < R10_VB_SLOTS; ++slot) {
            if (!(l->slot_mask & (1u << slot))) continue;
            r10_resource *vb = st->vb[slot].buf;
            uint32_t stride = st->vb[slot].stride, offset = st->vb[slot].offset;
            int instanced = 0;
            uint32_t step = 1;
            for (uint32_t i = 0; i < l->count; ++i) if (l->el[i].slot == slot && l->el[i].instanced) { instanced = 1; step = l->el[i].step; }
            glVertexBindingDivisor(slot, instanced ? step : 0);
            if (!vb) { glBindVertexBuffer(slot, 0, 0, (GLsizei)stride); continue; }
            gl_res *g = res_be(vb);
            GLintptr binding_offset = offset;
            if (shift_base && !instanced) binding_offset += (GLintptr)base_vertex * stride;
            if (g->id && !vb->discards) glBindVertexBuffer(slot, g->id, binding_offset, (GLsizei)stride);
            else {
                uint32_t lo, hi;
                if (instanced) { lo = offset; hi = offset + (dr->first_instance + dr->instances) * stride; }
                else if (dr->vertex_hi == 0xFFFFFFFFu) { lo = offset; hi = vb->desc.width; }
                else { lo = offset + dr->vertex_lo * stride; hi = offset + dr->vertex_hi * stride; }
                uint32_t at = stream_buffer(vb, lo, hi);
                if (at == UINT32_MAX) return;
                glBindVertexBuffer(slot, ring_buffer, at + binding_offset, (GLsizei)stride);
            }
        }
    }
    for (uint32_t i = 0; i < 16; ++i) {
        uint32_t bit = 1u << i;
        if ((want & bit) && !(enabled_attribs & bit)) glEnableVertexAttribArray(i);
        if (!(want & bit) && (enabled_attribs & bit)) glDisableVertexAttribArray(i);
    }
    enabled_attribs = want;
    GLenum mode = gl_mode(st->topology);
    if (dr->indexed) {
        r10_resource *ib = st->ib;
        if (!ib) return;
        gl_res *g = res_be(ib);
        int wide = st->ib_format == 42;
        uint32_t isize = wide ? 4 : 2;
        uintptr_t offset = st->ib_offset + (uintptr_t)dr->first * isize;
        if (g->id && !ib->discards) glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, g->id);
        else {
            uint32_t at = stream_buffer(ib, (uint32_t)offset, (uint32_t)offset + dr->count * isize);
            if (at == UINT32_MAX) return;
            glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ring_buffer);
            offset += at;
        }
        GLenum type = wide ? GL_UNSIGNED_INT : GL_UNSIGNED_SHORT;
        if (shift_base || (caps.emulate_base_vertex && !base_vertex))
            glDrawElementsInstanced(mode, (GLsizei)dr->count, type, (const void *)offset, (GLsizei)dr->instances);
        else
            glDrawElementsInstancedBaseVertex(mode, (GLsizei)dr->count, type, (const void *)offset,
                                              (GLsizei)dr->instances, base_vertex);
    } else {
        glDrawArraysInstanced(mode, (GLint)dr->first, (GLsizei)dr->count, (GLsizei)dr->instances);
    }
    st->dirty = 0;
}

/* ------------------------------------------------------------------ clears */
static void clear_prepare(void)
{
    glDisable(GL_SCISSOR_TEST);
    applied.valid = 0;
}

static void gles_clear_rtv(r10_device *d, r10_view *v, const float color[4])
{
    gl_current();
    GLuint fbo = view_fbo(&v, 1, NULL);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, fbo);
    gpu_timer_frame(d, 0);
    bound_fbo = 0xFFFFFFFFu;
    clear_prepare();
    glColorMaski(0, GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    float c[4] = { color[0], color[1], color[2], color[3] };
    if (v->fmt->is_int) {
        GLuint u[4] = { (GLuint)color[0], (GLuint)color[1], (GLuint)color[2], (GLuint)color[3] };
        glClearBufferuiv(GL_COLOR, 0, u);
    } else glClearBufferfv(GL_COLOR, 0, c);
    d->st.dirty |= R10_DIRTY_TARGETS | R10_DIRTY_SCISSOR | R10_DIRTY_VIEWPORT;
}

static void gles_clear_dsv(r10_device *d, r10_view *v, uint32_t flags, float depth, uint8_t stencil)
{
    gl_current();
    GLuint fbo = view_fbo(NULL, 0, v);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, fbo);
    gpu_timer_frame(d, 0);
    bound_fbo = 0xFFFFFFFFu;
    clear_prepare();
    glDepthMask(GL_TRUE);
    glStencilMask(0xFF);
    if ((flags & D3D10_CLEAR_DEPTH) && (flags & D3D10_CLEAR_STENCIL) && v->fmt->stencil) glClearBufferfi(GL_DEPTH_STENCIL, 0, depth, stencil);
    else if (flags & D3D10_CLEAR_DEPTH) glClearBufferfv(GL_DEPTH, 0, &depth);
    else if ((flags & D3D10_CLEAR_STENCIL) && v->fmt->stencil) { GLint s = stencil; glClearBufferiv(GL_STENCIL, 0, &s); }
    d->st.dirty |= R10_DIRTY_TARGETS | R10_DIRTY_SCISSOR | R10_DIRTY_VIEWPORT;
}

/* ------------------------------------------------------------------ queries */
typedef struct gl_query { GLuint id; GLsync fence; uint32_t flush_gen; } gl_query;
/* A Direct3D flush is needed once until new commands follow: a query ended before the last flush (or swap)
 * polls without flushing again.  Every poll flushed before, ~15 glFlush per frame of the Medical save on the
 * Adreno 735, each one able to end the tiler's render pass. */
static uint32_t flush_gen;
static int gles_query_create(r10_device *d, r10_query *q) { (void)d; q->be = calloc(1, sizeof(gl_query)); return 1; }
static void gles_query_destroy(r10_device *d, r10_query *q)
{
    (void)d;
    gl_query *g = (gl_query *)q->be;
    if (!g) return;
    gl_current();
    if (g->id) glDeleteQueries(1, &g->id);
    if (g->fence) glDeleteSync(g->fence);
    free(g);
}
static void gles_query_begin(r10_device *d, r10_query *q)
{
    (void)d;
    gl_query *g = (gl_query *)q->be;
    if (q->type != D3D10_QUERY_OCCLUSION && q->type != D3D10_QUERY_OCCLUSION_PREDICATE) return;
    gl_current();
    if (!g->id) glGenQueries(1, &g->id);
    glBeginQuery(GL_ANY_SAMPLES_PASSED_CONSERVATIVE, g->id);
}
static void gles_query_end(r10_device *d, r10_query *q)
{
    (void)d;
    gl_query *g = (gl_query *)q->be;
    gl_current();
    if ((q->type == D3D10_QUERY_OCCLUSION || q->type == D3D10_QUERY_OCCLUSION_PREDICATE) && g->id && q->issued) {
        glEndQuery(GL_ANY_SAMPLES_PASSED_CONSERVATIVE);
        g->flush_gen = flush_gen;
        return;
    }
    if (g->fence) glDeleteSync(g->fence);
    g->fence = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
    g->flush_gen = flush_gen;
}
static int gles_query_poll_inner(r10_query *q, int flush);
/* Statistics: the poll time goes to "fence status", every flush it asks for to "submits". */
static int gles_query_poll(r10_device *d, r10_query *q, int flush)
{
    uint64_t start = r10_now_ns();
    gl_query *g = (gl_query *)q->be;
    if (flush && g->flush_gen != flush_gen) flush = 0;   /* already flushed since the query ended */
    int done = gles_query_poll_inner(q, flush);
    if (!done && flush) ++flush_gen;
    d->stats.status_us += (uint32_t)((r10_now_ns() - start) / 1000);
    if (!done && flush) ++d->stats.submits;
    return done;
}
static int gles_query_poll_inner(r10_query *q, int flush)
{
    gl_query *g = (gl_query *)q->be;
    gl_current();
    if (q->type == D3D10_QUERY_OCCLUSION || q->type == D3D10_QUERY_OCCLUSION_PREDICATE) {
        if (!g->id) { q->samples = 1; return 1; }
        GLuint ready = 0;
        glGetQueryObjectuiv(g->id, GL_QUERY_RESULT_AVAILABLE, &ready);
        if (!ready) { if (flush) glFlush(); return 0; }
        GLuint any = 0;
        glGetQueryObjectuiv(g->id, GL_QUERY_RESULT, &any);
        q->samples = any ? 1000 : 0;
        return 1;
    }
    if (q->type == D3D10_QUERY_TIMESTAMP) { q->samples = r10_now_ns(); return 1; }
    if (!g->fence) return 1;
    GLenum r = glClientWaitSync(g->fence, flush ? GL_SYNC_FLUSH_COMMANDS_BIT : 0, 0);
    return r == GL_ALREADY_SIGNALED || r == GL_CONDITION_SATISFIED;
}

static void gles_flush(r10_device *d) { (void)d; gl_current(); glFlush(); ++flush_gen; }

/* ------------------------------------------------------------------ present */
/* Keep GL_TIME_ELAPSED on offscreen work. Spanning the window pass can fault on Mali. */
static GLuint gpu_timer[4];
static enum { TIMER_IDLE, TIMER_RUNNING, TIMER_ENDED, TIMER_DISJOINT } gpu_timer_state[4];
static unsigned gpu_timer_slot;
#ifndef GL_TIME_ELAPSED_EXT
#define GL_TIME_ELAPSED_EXT 0x88BF
#endif
#ifndef GL_QUERY_RESULT_EXT
#define GL_QUERY_RESULT_EXT 0x8866
#define GL_QUERY_RESULT_AVAILABLE_EXT 0x8867
#endif
#ifndef GL_GPU_DISJOINT_EXT
#define GL_GPU_DISJOINT_EXT 0x8FBB
#endif

static void gpu_timer_frame(r10_device *d, int end)
{
    if (!gl_query_u64) return;
    if (!gpu_timer[0]) glGenQueries(4, gpu_timer);
    unsigned s = gpu_timer_slot & 3;
    if (end) {
        if (gpu_timer_state[s] == TIMER_RUNNING) {
            glEndQuery(GL_TIME_ELAPSED_EXT);
            gpu_timer_state[s] = TIMER_ENDED;
        }
        /* the oldest slot: four frames back */
        unsigned o = (gpu_timer_slot + 1) & 3;
        if (gpu_timer_state[o] != TIMER_IDLE) {
            GLuint available = 0;
            glGetQueryObjectuiv(gpu_timer[o], GL_QUERY_RESULT_AVAILABLE_EXT, &available);
            GLint disjoint = 0;
            glGetIntegerv(GL_GPU_DISJOINT_EXT, &disjoint);
            if (disjoint) {
                /* Reading the flag clears it. Invalidate every result that could span the clock change. */
                for (unsigned i = 0; i < sizeof gpu_timer_state / sizeof *gpu_timer_state; ++i)
                    if (gpu_timer_state[i] == TIMER_ENDED) gpu_timer_state[i] = TIMER_DISJOINT;
            }
            if (available) {
                if (gpu_timer_state[o] == TIMER_ENDED) {
                    GLuint64 ns = 0;
                    gl_query_u64(gpu_timer[o], GL_QUERY_RESULT_EXT, &ns);
                    /* Some Mali and Adreno drivers also return invalid durations without the flag. */
                    if (ns && ns < 1000000000ull) { d->stats.gpu_us += (uint32_t)(ns / 1000); ++d->stats.gpu_frames; }
                }
                gpu_timer_state[o] = TIMER_IDLE;
            }
        }
        ++gpu_timer_slot;
    } else if (gpu_timer_state[s] == TIMER_IDLE) {
        glBeginQuery(GL_TIME_ELAPSED_EXT, gpu_timer[s]);
        gpu_timer_state[s] = TIMER_RUNNING;
    }
}

static uint32_t gles_present(r10_device *d, r10_resource *bb, uint32_t interval)
{
    (void)interval;
    gl_current();
    gpu_timer_frame(d, 1);
    if (!follow_window()) return DXGI_STATUS_OCCLUDED_;
    gl_res *g = res_be(bb);
    EGLint w = 0, h = 0;
    eglQuerySurface(egl_display, egl_surface, EGL_WIDTH, &w);
    eglQuerySurface(egl_display, egl_surface, EGL_HEIGHT, &h);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
    bound_fbo = 0xFFFFFFFFu;
    applied.valid = 0;
    glDisable(GL_SCISSOR_TEST); glDisable(GL_DEPTH_TEST); glDisable(GL_STENCIL_TEST); glDisable(GL_CULL_FACE);
    glDisablei(GL_BLEND, 0); glColorMaski(0, GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glViewport(0, 0, w, h);
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    int32_t x, y, vw, vh;
    r10_fit(bb->desc.width, bb->desc.height, (uint32_t)w, (uint32_t)h, &x, &y, &vw, &vh);
    glViewport(x, y, vw, vh);
    glUseProgram(present_program);
    glUniform4f(present_rect_loc, 0, 0, 1, 1);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, g->id);
    glBindSampler(0, 0);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    r10_pace_frame();
    /* The limiter sleeps inside void_blocking_begin, which gives up the guest machine, and threads.c
     * then releases the GL context of this thread. A swap without it current is EGL_BAD_SURFACE: the
     * frame was lost and the window surface recreated on most frames at the FPS cap. */
    gl_current();
    uint64_t swap_start = r10_now_ns();
    void_machine_lend_begin();
    EGLBoolean swapped = eglSwapBuffers(egl_display, egl_surface);
    if (swapped) r10_note_present();
    void_machine_lend_end();
    d->stats.wait_us += (uint32_t)((r10_now_ns() - swap_start) / 1000);
    if (!swapped) {
        EGLint e = eglGetError();
        R10_LOG("GLES: eglSwapBuffers failed %04X", e);
        egl_window_serial = 0xFFFFFFFFu;
    }
    ring_next_segment();
    ++flush_gen;   /* the swap flushed the frame */
    GLenum e = glGetError();
    if (e) {
        static unsigned logged;
        if (logged++ < 32) R10_LOG("GLES: error %04X during frame %llu", e, (unsigned long long)d->frame);
    }
    d->st.dirty = R10_DIRTY_ALL;
    return 0;
}

static uint8_t *gles_read_rgba(r10_device *d, r10_resource *r, uint32_t sub, uint32_t *w, uint32_t *h)
{
    (void)d;
    gl_current();
    uint32_t dd;
    r10_subresource_size(r, sub, w, h, &dd);
    uint8_t *out = (uint8_t *)malloc((size_t)*w * *h * 4);
    if (!read_texture(r, sub, out, *w * 4)) { free(out); return NULL; }
    return out;
}

static void gles_layout_destroy(r10_device *d, r10_layout *l) { (void)d; (void)l; }
static void gles_state_destroy(r10_device *d, r10_state_object *s)
{
    (void)d;
    if (applied.blend == s) applied.blend = NULL, applied.valid = 0;
    if (applied.depth == s) applied.depth = NULL, applied.valid = 0;
    if (applied.raster == s) applied.raster = NULL, applied.valid = 0;
}

/* ------------------------------------------------------------------ AMD FSR1 */
/* The render scale draws the scene's tonemap into the top-left sw x sh of the back buffer; EASU upscales it
 * into a texture of the back buffer's size, RCAS sharpens that back into the back buffer before the HUD. */
static GLuint fsr_easu_program, fsr_rcas_program, fsr_fbo, fsr_temp;
static GLint fsr_easu_con[4], fsr_rcas_con;
static uint32_t fsr_w, fsr_h;
static int fsr_failed;

static int fsr_prepare(void)
{
    if (fsr_easu_program) return 1;
    if (fsr_failed) return 0;
    GLuint easu = compile(GL_FRAGMENT_SHADER, r10_fsr_easu_es, "FSR1 EASU");
    GLuint rcas = compile(GL_FRAGMENT_SHADER, r10_fsr_rcas_es, "FSR1 RCAS");
    GLuint pe = easu ? link_program(present_vs, easu, "FSR1 EASU program", NULL) : 0;
    GLuint pr = rcas ? link_program(present_vs, rcas, "FSR1 RCAS program", NULL) : 0;
    if (easu) glDeleteShader(easu);
    if (rcas) glDeleteShader(rcas);
    if (!pe || !pr) {
        if (pe) glDeleteProgram(pe);
        if (pr) glDeleteProgram(pr);
        fsr_failed = 1;
        R10_LOG("GLES: FSR1 unavailable; the render scale stretches the scene bilinearly");
        return 0;
    }
    static const char *names[4] = { "con0", "con1", "con2", "con3" };
    for (int i = 0; i < 4; ++i) fsr_easu_con[i] = glGetUniformLocation(pe, names[i]);
    fsr_rcas_con = glGetUniformLocation(pr, "con0");
    glUseProgram(pe); glUniform1i(glGetUniformLocation(pe, "src"), 0);
    glUseProgram(pr); glUniform1i(glGetUniformLocation(pr, "src"), 0);
    glGenFramebuffers(1, &fsr_fbo);
    fsr_easu_program = pe;
    fsr_rcas_program = pr;
    R10_LOG("GLES: FSR1 EASU/RCAS ready");
    return 1;
}

static int gles_upscale(r10_device *d, r10_resource *bb, uint32_t sw, uint32_t sh, float sharpness)
{
    gl_current();
    if (!fsr_prepare()) return 0;
    if (!bb) return 1;
    gl_res *g = res_be(bb);
    if (!g || !g->id) return 0;
    uint32_t w = bb->desc.width, h = bb->desc.height;
    if (fsr_w != w || fsr_h != h) {
        if (fsr_temp) glDeleteTextures(1, &fsr_temp);
        glGenTextures(1, &fsr_temp);
        glBindTexture(GL_TEXTURE_2D, fsr_temp);
        glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8, (GLsizei)w, (GLsizei)h);
        fsr_w = w; fsr_h = h;
    }
    uint32_t easu[16], rcas[4];
    r10_fsr_constants(easu, rcas, (float)sw, (float)sh, (float)w, (float)h, (float)w, (float)h, sharpness);
    glDisable(GL_SCISSOR_TEST); glDisable(GL_DEPTH_TEST); glDisable(GL_STENCIL_TEST); glDisable(GL_CULL_FACE);
    glDisablei(GL_BLEND, 0); glColorMaski(0, GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glViewport(0, 0, (GLsizei)w, (GLsizei)h);
    glActiveTexture(GL_TEXTURE0);
    glBindSampler(0, 0);
    /* EASU: back buffer region -> temporary texture */
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, fsr_fbo);
    glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, fsr_temp, 0);
    glBindTexture(GL_TEXTURE_2D, g->id);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glUseProgram(fsr_easu_program);
    for (int i = 0; i < 4; ++i) glUniform4uiv(fsr_easu_con[i], 1, easu + 4 * i);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    /* RCAS: temporary texture -> back buffer */
    glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, g->id, 0);
    glBindTexture(GL_TEXTURE_2D, fsr_temp);
    glUseProgram(fsr_rcas_program);
    glUniform4uiv(fsr_rcas_con, 1, rcas);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, 0, 0);
    bound_fbo = 0xFFFFFFFFu;
    applied.valid = 0;
    d->st.dirty = R10_DIRTY_ALL;
    return 1;
}

r10_backend r10_gles_backend = {
    "OpenGL ES", gles_init, gles_shutdown, 0,
    gles_resource_create, gles_resource_destroy, gles_resource_update, gles_resource_read, gles_copy, gles_resolve,
    gles_generate_mips, gles_view_create, gles_view_destroy, gles_sampler_create, gles_sampler_destroy, gles_shader_create,
    gles_shader_destroy, gles_layout_destroy, gles_state_destroy, gles_clear_rtv, gles_clear_dsv, gles_draw,
    gles_query_create, gles_query_destroy, gles_query_begin, gles_query_end, gles_query_poll, gles_flush, gles_present,
    gles_read_rgba, gles_upscale,
    format_usable, 0, gles_resource_store,
};

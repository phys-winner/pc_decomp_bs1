/* BioShock's native Direct3D 10 renderer: the D3DDrv10 package of Bioshock.exe talks to these objects
 * instead of d3d10.dll/dxgi.dll/d3dx10_33.dll, and they draw with Vulkan 1.1 (r10_vulkan.c) or
 * OpenGL ES 3.2 (r10_gles.c).  No Direct3D 9 code, no DXVK and no shader translator runs on the device:
 * every shader of the game is a build-time library entry (tools/build_shaderlib10.py, r10_shaderlib.h).
 *
 * The objects are guest COM objects (com.h): 8 bytes of guest memory with the vtable VA and an index
 * of the host object table.  Every method runs with the machine owned by the calling guest thread, so
 * the device needs no locks of its own; the OpenGL ES context follows the thread that owns the machine
 * (r10_gles_thread_acquire/release).
 *
 * Conventions of both backends (the same as DXVK's on Vulkan):
 *   - render targets keep Direct3D's orientation (row 0 at the top).  Vulkan: negative viewport height.
 *     OpenGL ES: the vertex shaders negate Y and convert depth to -w..w (bs_shaderc), the present pass
 *     flips once into the window;
 *   - formats keep Direct3D's memory layout; OpenGL ES samples BGRA uploads through a swizzle. */
#ifndef R10_H
#define R10_H

#include "com.h"
#include <math.h>
#include <stdint.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

#define R10_LOG(...) void_log("[r10] " __VA_ARGS__)

/* ------------------------------------------------------------------ enums (d3d10.h / dxgi.h) */
enum {
    D3D10_USAGE_DEFAULT = 0, D3D10_USAGE_IMMUTABLE = 1, D3D10_USAGE_DYNAMIC = 2, D3D10_USAGE_STAGING = 3
};
enum {
    D3D10_BIND_VERTEX_BUFFER = 0x1, D3D10_BIND_INDEX_BUFFER = 0x2, D3D10_BIND_CONSTANT_BUFFER = 0x4,
    D3D10_BIND_SHADER_RESOURCE = 0x8, D3D10_BIND_STREAM_OUTPUT = 0x10, D3D10_BIND_RENDER_TARGET = 0x20,
    D3D10_BIND_DEPTH_STENCIL = 0x40
};
enum { D3D10_CPU_ACCESS_WRITE = 0x10000, D3D10_CPU_ACCESS_READ = 0x20000 };
enum { D3D10_RESOURCE_MISC_GENERATE_MIPS = 0x1, D3D10_RESOURCE_MISC_SHARED = 0x2, D3D10_RESOURCE_MISC_TEXTURECUBE = 0x4 };
enum { D3D10_MAP_READ = 1, D3D10_MAP_WRITE = 2, D3D10_MAP_READ_WRITE = 3, D3D10_MAP_WRITE_DISCARD = 4,
       D3D10_MAP_WRITE_NO_OVERWRITE = 5 };
enum { D3D10_MAP_FLAG_DO_NOT_WAIT = 0x100000 };
enum { D3D10_CLEAR_DEPTH = 1, D3D10_CLEAR_STENCIL = 2 };
enum {
    R10_DIM_UNKNOWN = 0, R10_DIM_BUFFER = 1, R10_DIM_TEXTURE1D = 2, R10_DIM_TEXTURE2D = 3, R10_DIM_TEXTURE3D = 4
};
enum {   /* D3D10_SRV_DIMENSION */
    D3D10_SRV_DIMENSION_UNKNOWN = 0, D3D10_SRV_DIMENSION_BUFFER = 1, D3D10_SRV_DIMENSION_TEXTURE1D = 2,
    D3D10_SRV_DIMENSION_TEXTURE1DARRAY = 3, D3D10_SRV_DIMENSION_TEXTURE2D = 4, D3D10_SRV_DIMENSION_TEXTURE2DARRAY = 5,
    D3D10_SRV_DIMENSION_TEXTURE2DMS = 6, D3D10_SRV_DIMENSION_TEXTURE2DMSARRAY = 7, D3D10_SRV_DIMENSION_TEXTURE3D = 8,
    D3D10_SRV_DIMENSION_TEXTURECUBE = 9
};
enum {   /* D3D10_RTV_DIMENSION / D3D10_DSV_DIMENSION share the 2D values we use */
    D3D10_RTV_DIMENSION_BUFFER = 1, D3D10_RTV_DIMENSION_TEXTURE1D = 2, D3D10_RTV_DIMENSION_TEXTURE1DARRAY = 3,
    D3D10_RTV_DIMENSION_TEXTURE2D = 4, D3D10_RTV_DIMENSION_TEXTURE2DARRAY = 5, D3D10_RTV_DIMENSION_TEXTURE2DMS = 6,
    D3D10_RTV_DIMENSION_TEXTURE2DMSARRAY = 7, D3D10_RTV_DIMENSION_TEXTURE3D = 8
};
enum {
    D3D10_DSV_DIMENSION_TEXTURE1D = 1, D3D10_DSV_DIMENSION_TEXTURE1DARRAY = 2, D3D10_DSV_DIMENSION_TEXTURE2D = 3,
    D3D10_DSV_DIMENSION_TEXTURE2DARRAY = 4, D3D10_DSV_DIMENSION_TEXTURE2DMS = 5, D3D10_DSV_DIMENSION_TEXTURE2DMSARRAY = 6
};
enum {   /* D3D10_PRIMITIVE_TOPOLOGY */
    D3D10_PRIMITIVE_TOPOLOGY_UNDEFINED = 0, D3D10_PRIMITIVE_TOPOLOGY_POINTLIST = 1, D3D10_PRIMITIVE_TOPOLOGY_LINELIST = 2,
    D3D10_PRIMITIVE_TOPOLOGY_LINESTRIP = 3, D3D10_PRIMITIVE_TOPOLOGY_TRIANGLELIST = 4, D3D10_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP = 5
};
enum { D3D10_FILL_WIREFRAME = 2, D3D10_FILL_SOLID = 3 };
enum { D3D10_CULL_NONE = 1, D3D10_CULL_FRONT = 2, D3D10_CULL_BACK = 3 };
enum {   /* D3D10_BLEND */
    D3D10_BLEND_ZERO = 1, D3D10_BLEND_ONE = 2, D3D10_BLEND_SRC_COLOR = 3, D3D10_BLEND_INV_SRC_COLOR = 4,
    D3D10_BLEND_SRC_ALPHA = 5, D3D10_BLEND_INV_SRC_ALPHA = 6, D3D10_BLEND_DEST_ALPHA = 7, D3D10_BLEND_INV_DEST_ALPHA = 8,
    D3D10_BLEND_DEST_COLOR = 9, D3D10_BLEND_INV_DEST_COLOR = 10, D3D10_BLEND_SRC_ALPHA_SAT = 11,
    D3D10_BLEND_BLEND_FACTOR = 14, D3D10_BLEND_INV_BLEND_FACTOR = 15, D3D10_BLEND_SRC1_COLOR = 16,
    D3D10_BLEND_INV_SRC1_COLOR = 17, D3D10_BLEND_SRC1_ALPHA = 18, D3D10_BLEND_INV_SRC1_ALPHA = 19
};
enum { D3D10_BLEND_OP_ADD = 1, D3D10_BLEND_OP_SUBTRACT = 2, D3D10_BLEND_OP_REV_SUBTRACT = 3, D3D10_BLEND_OP_MIN = 4,
       D3D10_BLEND_OP_MAX = 5 };
enum { D3D10_COMPARISON_NEVER = 1, D3D10_COMPARISON_LESS = 2, D3D10_COMPARISON_EQUAL = 3, D3D10_COMPARISON_LESS_EQUAL = 4,
       D3D10_COMPARISON_GREATER = 5, D3D10_COMPARISON_NOT_EQUAL = 6, D3D10_COMPARISON_GREATER_EQUAL = 7,
       D3D10_COMPARISON_ALWAYS = 8 };
enum { D3D10_STENCIL_OP_KEEP = 1, D3D10_STENCIL_OP_ZERO = 2, D3D10_STENCIL_OP_REPLACE = 3, D3D10_STENCIL_OP_INCR_SAT = 4,
       D3D10_STENCIL_OP_DECR_SAT = 5, D3D10_STENCIL_OP_INVERT = 6, D3D10_STENCIL_OP_INCR = 7, D3D10_STENCIL_OP_DECR = 8 };
enum { D3D10_TEXTURE_ADDRESS_WRAP = 1, D3D10_TEXTURE_ADDRESS_MIRROR = 2, D3D10_TEXTURE_ADDRESS_CLAMP = 3,
       D3D10_TEXTURE_ADDRESS_BORDER = 4, D3D10_TEXTURE_ADDRESS_MIRROR_ONCE = 5 };
enum { D3D10_QUERY_EVENT = 0, D3D10_QUERY_OCCLUSION = 1, D3D10_QUERY_TIMESTAMP = 2, D3D10_QUERY_TIMESTAMP_DISJOINT = 3,
       D3D10_QUERY_PIPELINE_STATISTICS = 4, D3D10_QUERY_OCCLUSION_PREDICATE = 5, D3D10_QUERY_SO_STATISTICS = 6,
       D3D10_QUERY_SO_OVERFLOW_PREDICATE = 7 };
enum { D3D10_ASYNC_GETDATA_DONOTFLUSH = 1 };

#define DXGI_ERROR_NOT_FOUND_ 0x887A0002u
#define DXGI_ERROR_UNSUPPORTED_ 0x887A0004u
#define DXGI_ERROR_INVALID_CALL_ 0x887A0001u
#define DXGI_ERROR_WAS_STILL_DRAWING_ 0x887A000Au
#define DXGI_STATUS_OCCLUDED_ 0x087A0001u

/* ------------------------------------------------------------------ formats (r10_format.c) */
enum r10_format_kind { R10_FMT_COLOR = 0, R10_FMT_DEPTH, R10_FMT_BC, R10_FMT_TYPELESS };
typedef struct r10_format {
    uint32_t dxgi;
    uint8_t kind;           /* r10_format_kind */
    uint8_t bytes;          /* bytes per texel, or per 4x4 block for BC */
    uint8_t block;          /* 4 for BC formats, else 1 */
    uint8_t bc;             /* 1..5 BC family, 0 otherwise */
    uint8_t srgb, stencil, is_int, bgra;
    uint32_t vk;            /* VkFormat */
    uint32_t vk_typeless_family;   /* the DXGI typeless format of the family (mutable views) */
    uint32_t gl_internal, gl_format, gl_type;   /* 0 when OpenGL ES has no direct match */
    uint8_t gl_swizzle;     /* 1: sample BGRA data uploaded as RGBA (B and R exchanged) */
    uint8_t gl_alpha_one;   /* X8 formats: alpha reads 1 */
    uint8_t gl_r_to_a;      /* A8_UNORM stored as R8: sample (0,0,0,r) */
    uint8_t pad;
    const char *name;
} r10_format;

const r10_format *r10_format_get(uint32_t dxgi);
/* The format views of a typeless resource read it as (e.g. R24G8_TYPELESS -> D24_UNORM_S8_UINT). */
uint32_t r10_format_resource_typed(uint32_t dxgi, uint32_t bind);
uint32_t r10_format_row_pitch(const r10_format *f, uint32_t width);
uint32_t r10_format_rows(const r10_format *f, uint32_t height);
/* BC1..5 blocks -> RGBA8 (BC4: R8 as RGBA r,0,0,1; BC5: RG as r,g,0,1), rows top-down. */
void r10_bc_decode(int bc, int snorm, const uint8_t *src, uint32_t width, uint32_t height, uint32_t pitch,
                   uint8_t *rgba, uint32_t out_pitch);
float r10_half_to_float(uint16_t h);
uint16_t r10_float_to_half(float f);

/* ------------------------------------------------------------------ the build-time shader library */
struct r10_lib_shader;

/* ------------------------------------------------------------------ objects */
struct r10_device;
typedef struct r10_device r10_device;

typedef enum r10_kind {
    R10_KIND_DEVICE = 1, R10_KIND_BUFFER, R10_KIND_TEXTURE1D, R10_KIND_TEXTURE2D, R10_KIND_TEXTURE3D,
    R10_KIND_SRV, R10_KIND_RTV, R10_KIND_DSV, R10_KIND_BLEND, R10_KIND_DEPTH, R10_KIND_RASTER, R10_KIND_SAMPLER,
    R10_KIND_VS, R10_KIND_PS, R10_KIND_GS, R10_KIND_LAYOUT, R10_KIND_QUERY,
    R10_KIND_FACTORY, R10_KIND_ADAPTER, R10_KIND_OUTPUT, R10_KIND_SWAPCHAIN, R10_KIND_DXGI_DEVICE,
    R10_KIND_REFLECTION, R10_KIND_BLOB
} r10_kind;

typedef struct r10_private {     /* SetPrivateData / SetPrivateDataInterface */
    uint8_t guid[16];
    uint32_t size, data;         /* data: guest copy, or the interface VA when size == 0xFFFFFFFF */
    struct r10_private *next;
} r10_private;

typedef struct r10_object {
    void_com_object com;
    r10_kind kind;
    r10_private *priv;
    uint64_t id;                 /* unique for the device's caches (never reused) */
} r10_object;

/* D3D10_BUFFER_DESC / D3D10_TEXTURE?D_DESC folded together. */
typedef struct r10_resource_desc {
    uint32_t dim;                /* R10_DIM_* */
    uint32_t width, height, depth, mips, layers;
    uint32_t format;             /* DXGI format of the resource (may be typeless) */
    uint32_t samples, quality;
    uint32_t usage, bind, cpu, misc;
} r10_resource_desc;

typedef struct r10_resource {
    r10_object obj;
    r10_resource_desc desc;
    const r10_format *fmt;       /* the resource's own format */
    const r10_format *store;     /* what the GPU stores (BC decoded to RGBA8 without BC support) */
    uint32_t subresources;       /* mips * layers */
    /* Guest memory the game writes through Map (dynamic and staging resources, lazily). */
    uint32_t shadow, shadow_size;
    uint32_t *sub_offset;        /* textures: byte offset of each subresource in the shadow */
    uint32_t *sub_pitch;         /* row pitch of each subresource */
    uint32_t *sub_slice;         /* depth pitch of each subresource */
    uint32_t mapped;             /* bit per mapped subresource (buffers: bit 0) */
    uint32_t map_type;
    /* The shadow's heap (r10_objects.c: static vertex buffers apart from the rest), the frame of the last Map,
     * the list of resources with a shadow; a released shadow (a static vertex buffer not mapped for a while)
     * is refilled from the GPU copy at the next Map (backend buffer_read). */
    uint32_t shadow_heap;
    uint64_t map_frame;
    struct r10_resource *shadow_prev, *shadow_next;
    uint8_t shadow_released;
    /* Buffers: a host copy of an index buffer's data (the draw finds the vertices it uses). */
    uint8_t *host_copy;
    uint64_t version;            /* buffers: bumped by WRITE_DISCARD and by uploads of never discarded ones */
    uint32_t discards;           /* WRITE_DISCARD maps: 0 means the buffer is static (uploaded at Unmap) */
    uint64_t gpu_use;            /* last submission serial that referenced it */
    int is_backbuffer;
    uint32_t bound_rtv;          /* count of render target bindings (hazards) */
    /* The launcher's converted texture pack (r10_pack.c): a BC1-3 texture the backend cannot sample is stored
     * in the pack's GPU format (`converted`) while every level written into it is in the pack; `pack_raw`
     * keeps, per subresource, the pack's copy of the original BC blocks it holds (NULL: never written or not
     * in the pack), so a write the pack lacks can bring the texture back to the decoded storage without losing
     * a level, and a texture that left the pack still copies levels to and from converted ones. */
    uint8_t pack_eligible, converted;
    const uint8_t **pack_raw;
    void *be;                    /* backend object */
} r10_resource;

typedef struct r10_view {
    r10_object obj;
    r10_resource *res;           /* holds one reference */
    uint32_t format;             /* typed DXGI format of the view */
    const r10_format *fmt;
    uint32_t dimension;          /* D3D10_*_DIMENSION of the view's kind */
    uint32_t first_mip, mips, first_layer, layers;
    uint32_t buffer_first, buffer_count;
    uint8_t desc[24];            /* the description as the game passed it (GetDesc) */
    uint32_t desc_size;
    void *be;
} r10_view;

typedef struct r10_blend_desc {  /* D3D10_BLEND_DESC */
    uint32_t alpha_to_coverage, enable[8], src, dst, op, src_alpha, dst_alpha, op_alpha;
    uint8_t mask[8];
} r10_blend_desc;
typedef struct r10_stencil_op_desc { uint32_t fail, depth_fail, pass, func; } r10_stencil_op_desc;
typedef struct r10_depth_desc {  /* D3D10_DEPTH_STENCIL_DESC */
    uint32_t depth_enable, depth_write_mask, depth_func, stencil_enable;
    uint8_t stencil_read_mask, stencil_write_mask, pad[2];
    r10_stencil_op_desc front, back;
} r10_depth_desc;
typedef struct r10_raster_desc { /* D3D10_RASTERIZER_DESC */
    uint32_t fill, cull, front_ccw;
    int32_t depth_bias;
    float depth_bias_clamp, slope_scaled_depth_bias;
    uint32_t depth_clip, scissor, multisample, antialiased_line;
} r10_raster_desc;
typedef struct r10_sampler_desc {/* D3D10_SAMPLER_DESC */
    uint32_t filter, address_u, address_v, address_w;
    float mip_lod_bias;
    uint32_t max_anisotropy, comparison;
    float border[4], min_lod, max_lod;
} r10_sampler_desc;
/* The filter and anisotropy a backend creates for a game sampler (port setting "anisotropy", r10_present.c). */
uint32_t r10_sampler_filter(const r10_sampler_desc *sd, uint32_t *max_anisotropy);

typedef struct r10_state_object {
    r10_object obj;
    union { r10_blend_desc blend; r10_depth_desc depth; r10_raster_desc raster; r10_sampler_desc sampler; } d;
    uint32_t hash;
    void *be;                    /* samplers: the backend sampler */
} r10_state_object;

/* An input signature element of a vertex shader (ISGN). */
typedef struct r10_sig {
    char name[32];
    uint32_t index, reg, sysval, comp, mask;
} r10_sig;

typedef struct r10_shader {
    r10_object obj;
    int type;                    /* 0 pixel, 1 vertex, 2 geometry */
    const struct r10_lib_shader *lib;  /* NULL: not in the library (draws with it are skipped) */
    uint64_t hash;
    uint32_t size;
    r10_sig *inputs; uint32_t input_count;
    uint32_t used_cb, used_srv, used_sampler;   /* slot masks from the library bindings */
    void *be;
} r10_shader;

typedef struct r10_element {
    uint32_t location;           /* vertex shader input register */
    uint32_t format;             /* DXGI */
    uint32_t slot, offset, instanced, step;
} r10_element;

typedef struct r10_layout {
    r10_object obj;
    r10_element el[32];
    uint32_t count;
    uint32_t slot_mask;          /* vertex buffer slots it reads */
    uint32_t hash;
} r10_layout;

typedef struct r10_query {
    r10_object obj;
    uint32_t type, misc;
    int issued, ended;
    uint64_t serial;             /* submission that ends it */
    uint64_t samples;            /* occlusion result */
    void *be;
} r10_query;

/* ------------------------------------------------------------------ device state */
#define R10_CB_SLOTS 14
#define R10_SRV_SLOTS 128
#define R10_SAMPLER_SLOTS 16
#define R10_VB_SLOTS 16
#define R10_RT_SLOTS 8

typedef struct r10_stage_state {
    r10_shader *shader;
    r10_resource *cb[R10_CB_SLOTS];
    r10_view *srv[R10_SRV_SLOTS];
    r10_state_object *sampler[R10_SAMPLER_SLOTS];
    uint32_t srv_max;            /* one past the highest bound SRV slot */
} r10_stage_state;

typedef struct r10_viewport { int32_t x, y; uint32_t w, h; float minz, maxz; } r10_viewport;
typedef struct r10_rect { int32_t l, t, r, b; } r10_rect;

enum {   /* dirty bits of r10_state.dirty */
    R10_DIRTY_SHADERS = 1u << 0, R10_DIRTY_LAYOUT = 1u << 1, R10_DIRTY_VB = 1u << 2, R10_DIRTY_IB = 1u << 3,
    R10_DIRTY_TOPOLOGY = 1u << 4, R10_DIRTY_BLEND = 1u << 5, R10_DIRTY_DEPTH = 1u << 6, R10_DIRTY_RASTER = 1u << 7,
    R10_DIRTY_VIEWPORT = 1u << 8, R10_DIRTY_SCISSOR = 1u << 9, R10_DIRTY_TARGETS = 1u << 10,
    R10_DIRTY_VS_RESOURCES = 1u << 11, R10_DIRTY_PS_RESOURCES = 1u << 12, R10_DIRTY_VS_CB = 1u << 13,
    R10_DIRTY_PS_CB = 1u << 14, R10_DIRTY_STENCIL_REF = 1u << 15, R10_DIRTY_BLEND_FACTOR = 1u << 16,
    R10_DIRTY_ALL = 0xFFFFFFFFu
};

typedef struct r10_state {
    r10_layout *layout;
    struct { r10_resource *buf; uint32_t stride, offset; } vb[R10_VB_SLOTS];
    r10_resource *ib; uint32_t ib_format, ib_offset;
    uint32_t topology;
    r10_stage_state vs, ps, gs;
    r10_state_object *raster, *blend, *depth;
    float blend_factor[4];
    uint32_t sample_mask, stencil_ref;
    r10_viewport viewport[16]; uint32_t viewports;
    r10_rect scissor[16]; uint32_t scissors;
    r10_view *rtv[R10_RT_SLOTS]; uint32_t rtv_count;
    r10_view *dsv;
    uint32_t dirty;
} r10_state;

/* One draw as the backends receive it. */
typedef struct r10_draw {
    int indexed;
    uint32_t count, instances, first, first_instance;
    int32_t base_vertex;
    /* The vertices the draw reads (min..max index + base for indexed draws); for uploads of dynamic data. */
    uint32_t vertex_lo, vertex_hi;
} r10_draw;

/* ------------------------------------------------------------------ backend interface */
typedef struct r10_backend {
    const char *name;
    int (*init)(r10_device *d);
    void (*shutdown)(r10_device *d);
    int bc_supported;            /* set by init: BC1-5 sampled directly */
    int (*resource_create)(r10_device *d, r10_resource *r, const uint32_t *init);   /* init: guest D3D10_SUBRESOURCE_DATA[] or NULL */
    void (*resource_destroy)(r10_device *d, r10_resource *r);
    /* Subresource contents from host memory: buffers with sub=0 and box x = byte offset, w = bytes. */
    void (*resource_update)(r10_device *d, r10_resource *r, uint32_t sub, uint32_t x, uint32_t y, uint32_t z,
                            uint32_t w, uint32_t h, uint32_t depth, const void *data, uint32_t row_pitch, uint32_t slice_pitch);
    /* A staging resource's subresource into host memory (Map READ); 0 on failure. */
    int (*resource_read)(r10_device *d, r10_resource *r, uint32_t sub, void *out, uint32_t row_pitch, uint32_t slice_pitch);
    void (*copy)(r10_device *d, r10_resource *dst, uint32_t dst_sub, uint32_t x, uint32_t y, uint32_t z,
                 r10_resource *src, uint32_t src_sub, const uint32_t *box /* l,t,f,r,b,k or NULL */);
    void (*resolve)(r10_device *d, r10_resource *dst, uint32_t dst_sub, r10_resource *src, uint32_t src_sub);
    void (*generate_mips)(r10_device *d, r10_view *srv);
    int (*view_create)(r10_device *d, r10_view *v);
    void (*view_destroy)(r10_device *d, r10_view *v);
    int (*sampler_create)(r10_device *d, r10_state_object *s);
    void (*sampler_destroy)(r10_device *d, r10_state_object *s);
    int (*shader_create)(r10_device *d, r10_shader *s);
    void (*shader_destroy)(r10_device *d, r10_shader *s);
    void (*layout_destroy)(r10_device *d, r10_layout *l);
    void (*state_destroy)(r10_device *d, r10_state_object *s);   /* blend/depth/raster: pipeline cache eviction */
    void (*clear_rtv)(r10_device *d, r10_view *v, const float color[4]);
    void (*clear_dsv)(r10_device *d, r10_view *v, uint32_t flags, float depth, uint8_t stencil);
    void (*draw)(r10_device *d, const r10_draw *draw);
    int (*query_create)(r10_device *d, r10_query *q);
    void (*query_destroy)(r10_device *d, r10_query *q);
    void (*query_begin)(r10_device *d, r10_query *q);
    void (*query_end)(r10_device *d, r10_query *q);
    /* 1: result ready in q->samples (occlusion) or the event passed; 0: not yet. */
    int (*query_poll)(r10_device *d, r10_query *q, int flush);
    void (*flush)(r10_device *d);
    /* Shows the back buffer; returns 0, or DXGI_STATUS_OCCLUDED_ when there is no window. */
    uint32_t (*present)(r10_device *d, r10_resource *backbuffer, uint32_t interval);
    /* RGBA8 copy of a 2D subresource as it would be shown (thumbnails, diagnostics); malloc'ed. */
    uint8_t *(*read_rgba)(r10_device *d, r10_resource *r, uint32_t sub, uint32_t *w, uint32_t *h);
    /* AMD FSR1: the sw x sh top-left region of the back buffer upscaled into all of it (EASU, then RCAS with
     * `sharpness` stops).  bb NULL only prepares the passes.  1 when done (or ready), 0 when unavailable. */
    int (*upscale)(r10_device *d, r10_resource *bb, uint32_t sw, uint32_t sh, float sharpness);
    /* BC formats: 1 when the device samples this format as it is (per format: PowerVR's Vulkan driver samples
     * BC1-3 without the textureCompressionBC feature, which promises all of BC1-7), else it is decoded. */
    int (*samples)(const r10_format *f);
    /* Set by init: the converted pack families (r10_pack.c) the device samples, bit R10_PACK_ETC2 / R10_PACK_ASTC. */
    int pack_formats;
    /* Recreates a texture's storage for its current `converted` state; contents become undefined and views
     * follow the new storage the next time they are bound. 0 on failure. */
    int (*resource_store)(r10_device *d, r10_resource *r);
    /* What the backend's GPU memory holds, one line for the statistics (NULL: not tracked). */
    void (*memory_line)(char *out, size_t size);
    /* A static buffer's whole current content from a home the CPU can read (r->desc.width bytes); 0 when the
     * backend cannot (then its shadow is never released). NULL: not supported. */
    int (*buffer_read)(r10_device *d, r10_resource *r, void *out);
} r10_backend;

/* ------------------------------------------------------------------ converted texture pack (r10_pack.c) */
enum { R10_PACK_ETC2 = 1, R10_PACK_ASTC = 2 };   /* the families of android/texconv/tvtx.h (TVTX_ETC2, TVTX_ASTC) */
#define R10_PACK_BLOCK_BYTES 16u                  /* ASTC 4x4 and ETC2 RGBA8 EAC: what converted storage holds */
int r10_pack_family(void);                        /* the loaded pack's family the backend samples, 0 none */
/* Called before the backend creates a texture: decides `pack_eligible` and the initial `converted`. */
void r10_pack_prepare(r10_device *d, r10_resource *r);
void r10_pack_release(r10_resource *r);
/* Every texture write of the device (UpdateSubresource, initial data): converted blocks from the pack, or the
 * ordinary upload after the texture left the converted storage. */
void r10_texture_write(r10_device *d, r10_resource *r, uint32_t sub, uint32_t x, uint32_t y, uint32_t z, uint32_t w,
                       uint32_t h, uint32_t depth, const void *data, uint32_t row_pitch, uint32_t slice_pitch);
/* Every texture copy of the device: converted pairs copy blocks, a copy the pack cannot follow restores first. */
void r10_texture_copy(r10_device *d, r10_resource *dst, uint32_t dst_sub, uint32_t x, uint32_t y, uint32_t z,
                      r10_resource *src, uint32_t src_sub, const uint32_t *box);

extern r10_backend r10_vulkan_backend;
extern r10_backend r10_gles_backend;

/* ------------------------------------------------------------------ device */
struct r10_device {
    r10_object obj;
    uint32_t dxgi_device;        /* the IDXGIDevice facet (QueryInterface) */
    uint32_t adapter;            /* IDXGIAdapter it was created on (guest VA, one reference) */
    const r10_backend *be;
    void *be_state;
    r10_state st;
    uint64_t next_id;
    uint64_t frame;              /* presents */
    int tracing;                 /* this frame is traced (BIOSHOCK_R10_TRACE, r10-trace-now): backends may probe */
    uint32_t probe_first; int32_t probe_base;   /* the traced draw's first index and base vertex */
    uint64_t submitted, completed;   /* backend submission serials */
    uint32_t exception_mode;
    uint32_t bb_width, bb_height;    /* the swap chain's back buffer */
    r10_resource *backbuffer;        /* the swap chain's back buffer (not referenced) */
    uint32_t display_width, display_height;
    float fps_limit;
    int disabled_gs_logged;
    /* statistics of the current frame (BIOSHOCK_API_STATS) */
    struct { uint32_t draws, maps, updates, passes, pipelines, uploads_kb, submits, wait_us, gpu_us, gpu_frames, query_wait_us, submit_us, status_us, sets, set_writes, draw_ns, set_ns, present_ns, acquire_us, queue_present_us; } stats;
    /* Since start: shaders created, of those found in the library; run-time compiles, of those missing; the engine's
     * cache lookups the library answered (by cache identity, by record, missing and by fallback) and the
     * ShaderCache.pcs10 loads it skipped; shaders and input layouts created from Direct3D bytecode. */
    struct { uint32_t created, found, compiles, compile_misses, compile_fallbacks;
             uint32_t native_identity, native_record, native_misses, native_fallbacks, cache_loads_skipped, from_dxbc; } shader_stats;
    r10_private *priv;
};

extern r10_device *r10_dev;

/* r10_device.c */
uint32_t r10_device_create(uint32_t adapter, r10_device **out);
const char *r10_backend_name(void);
int r10_renderer_selected(void);    /* 1 when a native backend was chosen at start */
void r10_object_init(r10_object *o, r10_kind kind);
uint32_t r10_object_wrap(r10_object *o, void_com_class *cls, r10_kind kind);
r10_object *r10_object_from(uint32_t guest, r10_kind kind);   /* NULL for 0; traps on a wrong kind */
r10_resource *r10_resource_from(uint32_t guest);              /* any resource kind */
void r10_unbind_resource(r10_device *d, r10_resource *r);     /* the resource goes away: drop bindings */
void r10_object_release_guest(uint32_t guest);                /* IUnknown::Release of any of our objects */
void r10_object_addref_guest(uint32_t guest);

/* r10_objects.c: COM classes of resources, views, states, shaders, layouts, queries */
uint32_t r10_create_buffer(r10_device *d, uint32_t desc_va, uint32_t init_va, uint32_t *out);
uint32_t r10_create_texture(r10_device *d, int dim, uint32_t desc_va, uint32_t init_va, uint32_t *out);
uint32_t r10_create_texture_internal(r10_device *d, const r10_resource_desc *desc, r10_resource **out);
uint32_t r10_create_view(r10_device *d, r10_kind kind, uint32_t resource, uint32_t desc_va, uint32_t *out);
uint32_t r10_create_state(r10_device *d, r10_kind kind, uint32_t desc_va, uint32_t *out);
uint32_t r10_create_shader(r10_device *d, int type, uint32_t bytecode, uint32_t size, uint32_t *out);
uint32_t r10_create_layout(r10_device *d, uint32_t elements, uint32_t count, uint32_t bytecode, uint32_t size, uint32_t *out);
uint32_t r10_create_query(r10_device *d, uint32_t desc_va, uint32_t *out);
uint32_t r10_resource_map(r10_device *d, r10_resource *r, uint32_t sub, uint32_t type, uint32_t flags, uint32_t *pitch,
                          uint32_t *slice);
void r10_resource_unmap(r10_device *d, r10_resource *r, uint32_t sub);
void r10_subresource_size(const r10_resource *r, uint32_t sub, uint32_t *w, uint32_t *h, uint32_t *depth);
void r10_resource_layout(r10_resource *r);     /* sub_offset/pitch/slice and shadow_size */
uint32_t r10_resource_guest(r10_resource *r);
/* Guest memory of the resources' Map shadows (r10_objects.c): bytes and count of resources that have one. */
extern uint64_t r10_shadow_bytes;
extern uint32_t r10_shadow_count;
/* Every 300 frames (r10_present.c): releases the shadows of static vertex buffers not mapped for 600 frames
 * and gives their heap's free pages back (BIOSHOCK_R10_DEBUG 8388608 keeps them). */
void r10_shadow_sweep(r10_device *d);
/* A resource's shadow, made (zeroed, or refilled when it was released) where it has none; 1 on success. */
int r10_resource_shadow(r10_device *d, r10_resource *r);
void r10_shadow_free(r10_resource *r);
/* QA census (<data>/r10-memstat-now): the shadows by kind and the guest memory (memory.c), to the log. */
void r10_memory_census(r10_device *d);
/* QA (BIOSHOCK_R10_DEBUG 16777216): texture uploads' guest sources, for the census (r10_device.c). */
void r10_census_texture_source(uint32_t va, uint32_t bytes);
/* Uploads a dynamic buffer's guest shadow range to the backend before a draw reads it. */
const uint8_t *r10_buffer_shadow(r10_resource *r);

/* r10_dxgi.c */
uint32_t r10_dxgi_factory_create(uint32_t *out);
uint32_t r10_swapchain_create(r10_device *d, uint32_t desc_va, uint32_t *out);
void r10_swapchain_device_lost(void);

/* r10_reflect.c: the interface tables of bytecode the game passes (a DXBC container, or the BSIF interface of a
 * library program) */
int r10_dxbc_input_signature(const uint8_t *container, uint32_t size, r10_sig **out, uint32_t *count);
uint32_t r10_reflect_create(const uint8_t *container, uint32_t size, uint32_t *out);
uint32_t r10_blob_create(const void *data, uint32_t size, uint32_t *out);
/* The inputs of a program the game names by bytecode: a token (from the library's interface) or DXBC. */
int r10_shader_input_signature(const uint8_t *bytecode, uint32_t size, r10_sig **out, uint32_t *count);

/* r10_shaderlib.c */
typedef struct r10_define { const char *name, *value; } r10_define;
int r10_shaderlib_open(void);
uint64_t r10_shaderlib_key(void);
/* The program translated from a DXBC container (the engine's own ShaderCache.pcs10, BIOSHOCK_R10_SHADERS=dxbc). */
const struct r10_lib_shader *r10_shaderlib_find(const uint8_t *dxbc, uint32_t size, uint64_t *hash);
const struct r10_lib_shader *r10_shaderlib_shader(uint32_t index);
uint32_t r10_shaderlib_index(const struct r10_lib_shader *s);
/* The program a token names (bytes the game passes as bytecode); NULL for anything else. */
const struct r10_lib_shader *r10_shaderlib_token(const uint8_t *bytes, uint32_t size);
/* Guest address of the token of a program, and of a cache record {size, time, token} for the engine's
 * FShaderCache lookup; made once per program. */
uint32_t r10_shaderlib_token_va(uint32_t index);
uint32_t r10_shaderlib_cache_record(uint32_t index, uint32_t time);
/* Program index or -1: a cache identity (name key, GUID, time stamp), a record key (and time stamp), a request key. */
int32_t r10_shaderlib_material(uint64_t name, const uint32_t guid[4], uint32_t time);
int32_t r10_shaderlib_record(uint64_t key, uint32_t time);
int32_t r10_shaderlib_request(uint64_t key);
uint64_t r10_name_key(const char *name);
/* tools/build_shaderlib10.py record_key: level 0 exact, 1 without the scene's defines, 2 the literals only. */
uint64_t r10_record_key(const char *name, const char *profile, const r10_define *defines, uint32_t count, int level);
/* Fallback key of a request the library lacks: level 1 without the scene's defines, level 2 the material's literals. */
uint64_t r10_request_family(const uint8_t *source, uint32_t size, const char *entry, const char *profile, uint32_t flags,
                            uint32_t defines_va, int level);
uint64_t r10_request_key(const uint8_t *source, uint32_t size, const char *entry, const char *profile, uint32_t flags,
                         uint32_t defines_va);
/* Inflated SPIR-V words (noclip: the variant without SV_ClipDistance), GLSL ES text or the interface container;
 * malloc'ed. */
uint32_t *r10_shaderlib_spirv(const struct r10_lib_shader *s, int noclip, uint32_t *words);
char *r10_shaderlib_glsl(const struct r10_lib_shader *s);
uint8_t *r10_shaderlib_interface(const struct r10_lib_shader *s, uint32_t *size);

/* r10_engine.c: Bioshock.exe's FShaderCache on the library (seams of tools/build_android.py) */
int r10_native_shaders(void);       /* 0 with BIOSHOCK_R10_SHADERS=dxbc: the engine reads ShaderCache.pcs10 itself */

/* r10_present.c: pacing and statistics shared by the backends */
void r10_pace_frame(void);
float r10_frame_limit(void);
int r10_swap_interval(void);
void r10_frame_presented(r10_device *d, const char *backend);
void r10_note_present(void);
/* r10_failure.c: the launcher's renderer failure policy (launcher/native/replacements/renderer_failure.h). A
 * backend whose start fails names the cause first (r10_start_error; the first one is kept). r10_renderer_failed
 * records a start failure (device_lost 0) or a lost GPU device (1) for the launcher and stops the game:
 * GameActivity reads it (nativeRendererFailure), Launch.rendererFailed opens the launcher, which offers the
 * next renderer; no backend or driver replaces the chosen one. Any thread; it does not return. */
void r10_renderer_failure_init(void);
void r10_start_error(const char *format, ...) __attribute__((format(printf, 1, 2)));
const char *r10_start_error_text(void);
void r10_renderer_presented(void);             /* r10_note_present: a frame the display took */
int r10_renderer_has_presented(void);
int r10_renderer_qa_start(void);
__attribute__((noreturn)) void r10_renderer_failed(int device_lost, const char *reason);
uint64_t r10_now_ns(void);
int r10_env_int(const char *name, int fallback, int lo, int hi);
/* r10_device.c: low priority on the little cores for a background renderer worker. */
int r10_background_thread_setup(void);
/* Letterboxed destination of a w x h image in a dw x dh window. */
void r10_fit(uint32_t sw, uint32_t sh, uint32_t dw, uint32_t dh, int32_t *x, int32_t *y, int32_t *w, int32_t *h);
/* Render scale and upscaler of the port settings (bioshock_render10.c). */
int r10_upscaler_mode(void);
/* FsrEasuCon and FsrRcasCon of AMD FSR1: an iw x ih image whose vw x vh top-left region becomes ow x oh. */
void r10_fsr_constants(uint32_t easu[16], uint32_t rcas[4], float vw, float vh, float iw, float ih, float ow, float oh,
                       float sharpness);
/* Called by the render scale (bioshock_render10.c): whether the backend can upscale, and the upscale itself. */
/* The scene area of the render scale (android/host/bioshock_render10.c): while the scaled scene draws, only
 * the top-left w x h of full-size (full_w x full_h) targets is used; zero otherwise. */
typedef struct r10_scene_area_t { uint32_t w, h, full_w, full_h; } r10_scene_area_t;
extern r10_scene_area_t r10_area;
void r10_scene_area(uint32_t w, uint32_t h, uint32_t full_w, uint32_t full_h);
const uint32_t *r10_area_copy_box(const r10_resource *dst, uint32_t dst_sub, uint32_t x, uint32_t y, uint32_t z,
                                  const r10_resource *src, uint32_t src_sub, const uint32_t *box, uint32_t out[6]);
int r10_upscale_available(void);
int r10_upscale_scene(uint32_t sw, uint32_t sh);

/* Guest helpers */
static inline float r10_f32(uint32_t bits) { float f; memcpy(&f, &bits, 4); return f; }

#ifdef __cplusplus
}
#endif
#endif

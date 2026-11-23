/* Layout of the build-time shader library (tools/build_shaderlib10.py, tools/shaderlib10/bs_shaderc.cpp).
 * Little endian; offsets are from the start of the library and 4-byte aligned.
 *
 *   r10_lib_header | r10_lib_shader[shader_count] sorted by (hash, dxbc_size) |
 *   r10_lib_material[material_count] sorted by (name, guid) | r10_lib_key[record_count] sorted by key |
 *   r10_lib_key[request_count] sorted by key | data
 *
 * The library holds no Direct3D bytecode.  A program is its SPIR-V (and the SPIR-V without SV_ClipDistance),
 * its GLSL ES and its interface: the version token and the RDEF, ISGN and OSGN tables of the DXBC it was
 * translated from, in a container of the DXBC layout with the magic BSIF, which is what the engine reads through
 * D3D10ReflectShader and CreateInputLayout.  Every blob is its own zlib stream, inflated when used.
 *
 * The engine finds a program three ways:
 * - a material: the identity under which Bioshock.exe's FShaderCache keeps a shader (sub_109AF8E0): the cache
 *   name (pass file, factory files and entry, sub_109AF770; r10_name_key), the 128-bit permutation GUID and the
 *   time stamp of the shaders.spk files the engine checks (sub_10C41760).  Every record of ShaderCache.pcs10.
 * - a record: a variant the shipped cache lacks, by its cache name, profile and the defines the engine's field
 *   writers recorded for it (r10_record_key); the families (level 1 without the scene's defines, level 2 the
 *   material's literals only) are records too.
 * - a request: a D3DX10CompileFromMemory call (r10_request_key over the text with its includes resolved): the
 *   HLSL Bioshock.exe embeds, and with BIOSHOCK_R10_SHADERS=dxbc the material variants.
 * The table of shaders is also sorted by the FNV-1a 64 of the DXBC container each program was translated from
 * (and its size, confirmed by the container's checksum), for the DXBC of ShaderCache.pcs10 when the engine reads
 * it itself (BIOSHOCK_R10_SHADERS=dxbc). */
#ifndef R10_SHADERLIB_H
#define R10_SHADERLIB_H
#include <stdint.h>

#define R10_LIB_MAGIC "BS10SHL3"
#define R10_LIB_VERSION 3u
#define R10_WARM_MAX 512u
#define R10_DEBUG_RECORD_WARMUP (1u << 26)
#define R10_DEBUG_NO_WARMUP (1u << 27)

typedef struct r10_warm_pair { uint64_t vs, ps; } r10_warm_pair;
typedef struct r10_warmup {
    const uint8_t *pairs, *pipelines;
    uint32_t pair_count, pipeline_count, pipeline_stride;
} r10_warmup;
int r10_shaderlib_warmup(r10_warmup *out);

typedef struct r10_lib_header {
    char magic[8];
    uint32_t version;
    uint32_t shader_count, shaders;
    uint32_t material_count, materials;
    uint32_t record_count, records;
    uint32_t request_count, requests;
    uint32_t data;
    uint64_t key;                /* of the whole library build */
    uint32_t total_size, reserved;
} r10_lib_header;

enum { R10_LIB_PUSH_CONSTANTS = 1, R10_LIB_CLIP_DISTANCE = 2, R10_LIB_VERTEX_ID = 4 };

typedef struct r10_lib_binding {
    uint32_t descriptor_type;    /* VkDescriptorType */
    uint32_t binding;            /* DXVK binding number: 160 x stage + {CB 0.., sampler 16.., SRV 32..} */
    uint32_t view_type;          /* VkImageViewType (VK_IMAGE_VIEW_TYPE_MAX_ENUM for buffers/samplers) */
} r10_lib_binding;

typedef struct r10_lib_combined {
    uint8_t unit;                /* OpenGL ES texture unit (binding of the combined sampler) */
    int8_t texture;              /* SRV slot */
    int8_t sampler;              /* sampler slot, -1: none (texelFetch) */
    uint8_t pad;
} r10_lib_combined;

typedef struct r10_lib_ubo { uint8_t slot, binding; } r10_lib_ubo;

typedef struct r10_lib_shader {
    uint64_t hash;               /* FNV-1a 64 of the DXBC container the program was translated from */
    uint32_t dxbc_size;
    uint8_t checksum[16];
    uint8_t type;                /* 0 pixel, 1 vertex */
    uint8_t flags;               /* R10_LIB_* */
    uint16_t combined_count;
    uint32_t input_mask, output_mask;
    uint32_t bindings, binding_count;
    uint32_t combined;
    uint32_t ubos, ubo_count;
    uint32_t icb, icb_size;      /* immediate constant buffer data (constant buffer slot 14) */
    uint32_t spirv, spirv_size, spirv_raw;
    uint32_t noclip, noclip_size, noclip_raw;
    uint32_t es, es_size, es_raw;
    uint32_t iface, iface_size, iface_raw;
    uint32_t es_cb0_bytes;
    uint32_t reserved[2];
} r10_lib_shader;

uint32_t r10_shaderlib_count(void);
const r10_lib_shader *r10_shaderlib_by_hash(uint64_t hash, int type);

typedef struct r10_lib_material {
    uint64_t name;               /* r10_name_key of the cache name */
    uint32_t guid[4];            /* the engine's permutation key */
    uint32_t time;               /* the time stamp the engine checks */
    uint32_t shader;             /* index into the shader table */
} r10_lib_material;

typedef struct r10_lib_key {
    uint64_t key;
    uint32_t shader;
    uint32_t time;               /* records: the time stamp the engine checks; requests: 0 */
} r10_lib_key;

_Static_assert(sizeof(r10_lib_header) == 64, "library header layout");
_Static_assert(sizeof(r10_lib_shader) == 128, "library shader layout");
_Static_assert(sizeof(r10_lib_material) == 32, "library material layout");
_Static_assert(sizeof(r10_lib_key) == 16, "library key layout");

/* The interface container: "BSIF", the version token of the program (D3D10_SHADER_DESC.Version), 20 bytes of
 * zeros, the chunk count at 28 and the chunk offsets from 32, as in DXBC. */
#define R10_IFACE_MAGIC "BSIF"

/* A program as the game holds it: 32 bytes in guest memory in place of bytecode (ID3D10Blob contents, the
 * engine's shader cache records, CreateVertexShader/CreatePixelShader/CreateInputLayout/D3D10ReflectShader). */
#define R10_TOKEN_MAGIC 0x544E5342u   /* "BSNT" */
#define R10_TOKEN_SIZE 32u
typedef struct r10_token {
    uint32_t magic, version;     /* R10_TOKEN_MAGIC, R10_LIB_VERSION */
    uint32_t shader, type;       /* index into the shader table, 0 pixel / 1 vertex */
    uint64_t library;            /* r10_lib_header.key */
    uint32_t reserved[2];
} r10_token;
_Static_assert(sizeof(r10_token) == R10_TOKEN_SIZE, "token layout");

/* The mapped library (r10_shaderlib.c); NULL before r10_shaderlib_open succeeded. */
extern const uint8_t *r10_lib;
static inline const void *r10_lib_at(uint32_t offset) { return r10_lib + offset; }

#endif

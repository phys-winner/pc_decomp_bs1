/* D3D10ReflectShader over the interface chunks (RDEF, ISGN, OSGN), ID3D10Blob and the three D3DX10 functions
 * the game imports.  The game's bytecode is a token of a library program (r10_shaderlib.h): its interface comes
 * from the library, translated from the DXBC on the PC; DXBC itself is read only when the engine loads
 * ShaderCache.pcs10 (BIOSHOCK_R10_SHADERS=dxbc).  D3DX10CompileFromMemory answers with tokens: Windows'
 * compiler produced the program of each of the game's run-time compile requests on the PC. */
#include "r10.h"
#include "r10_shaderlib.h"
#include "r10_resample.h"
#include <stdlib.h>
#include <strings.h>
#include <sys/stat.h>

/* ------------------------------------------------------------------ interface chunks (DXBC or BSIF) */
static const uint8_t *dxbc_chunk(const uint8_t *dxbc, uint32_t size, uint32_t fourcc, uint32_t *chunk_size)
{
    if (size < 32 || (memcmp(dxbc, "DXBC", 4) && memcmp(dxbc, R10_IFACE_MAGIC, 4))) return NULL;
    uint32_t count;
    memcpy(&count, dxbc + 28, 4);
    for (uint32_t i = 0; i < count && 32 + 4 * i + 4 <= size; ++i) {
        uint32_t at;
        memcpy(&at, dxbc + 32 + 4 * i, 4);
        if (at + 8 > size) continue;
        uint32_t cc, n;
        memcpy(&cc, dxbc + at, 4);
        memcpy(&n, dxbc + at + 4, 4);
        if (cc == fourcc && at + 8 + n <= size) { *chunk_size = n; return dxbc + at + 8; }
    }
    return NULL;
}
#define FOURCC(a, b, c, d) ((uint32_t)(a) | (uint32_t)(b) << 8 | (uint32_t)(c) << 16 | (uint32_t)(d) << 24)

static uint32_t rd32(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return v; }

int r10_dxbc_input_signature(const uint8_t *dxbc, uint32_t size, r10_sig **out, uint32_t *count)
{
    uint32_t n;
    const uint8_t *c = dxbc_chunk(dxbc, size, FOURCC('I', 'S', 'G', 'N'), &n);
    *out = NULL;
    *count = 0;
    if (!c || n < 8) return 0;
    uint32_t entries = rd32(c);
    if (entries > 64 || 8 + entries * 24 > n) return 0;
    r10_sig *sig = (r10_sig *)calloc(entries ? entries : 1, sizeof *sig);
    for (uint32_t i = 0; i < entries; ++i) {
        const uint8_t *e = c + 8 + 24 * i;
        uint32_t name = rd32(e);
        size_t len = name < n ? strnlen((const char *)c + name, n - name) : 0;
        if (len >= sizeof sig[i].name) len = sizeof sig[i].name - 1;
        memcpy(sig[i].name, c + name, len);
        sig[i].index = rd32(e + 4);
        sig[i].sysval = rd32(e + 8);
        sig[i].comp = rd32(e + 12);
        sig[i].reg = rd32(e + 16);
        sig[i].mask = e[20];
    }
    *out = sig;
    *count = entries;
    return 1;
}

int r10_shader_input_signature(const uint8_t *bytecode, uint32_t size, r10_sig **out, uint32_t *count)
{
    const r10_lib_shader *lib = r10_shaderlib_token(bytecode, size);
    if (!lib) return r10_dxbc_input_signature(bytecode, size, out, count);
    uint32_t n = 0;
    uint8_t *iface = r10_shaderlib_interface(lib, &n);
    int ok = iface && r10_dxbc_input_signature(iface, n, out, count);
    free(iface);
    return ok;
}

/* ------------------------------------------------------------------ ID3D10Blob */
typedef struct r10_blob { void_com_object com; uint32_t data, size; } r10_blob;
static void blob_destroy(void_com_object *o) { r10_blob *b = (r10_blob *)o; if (b->data) void_gfree(b->data); free(b); }
COM_METHOD(blob, QueryInterface, 3)
{
    r10_blob *b = (r10_blob *)void_com_host(THIS_);
    void_com_addref(&b->com);
    if (ARG(2)) MEM32(ARG(2)) = THIS_;
    return 0;
}
COM_METHOD(blob, AddRef, 1) { return void_com_addref((void_com_object *)void_com_host(THIS_)); }
COM_METHOD(blob, Release, 1) { return void_com_release((void_com_object *)void_com_host(THIS_)); }
COM_METHOD(blob, GetBufferPointer, 1) { return ((r10_blob *)void_com_host(THIS_))->data; }
COM_METHOD(blob, GetBufferSize, 1) { return ((r10_blob *)void_com_host(THIS_))->size; }
static const void_fn blob_methods[] = { blob_QueryInterface, blob_AddRef, blob_Release, blob_GetBufferPointer, blob_GetBufferSize };
static void_com_class blob_class = { "ID3D10Blob", 5, blob_methods, 0 };

uint32_t r10_blob_create(const void *data, uint32_t size, uint32_t *out)
{
    r10_blob *b = (r10_blob *)calloc(1, sizeof *b);
    b->size = size;
    b->data = void_galloc(size ? size : 1);
    if (!b->data) { free(b); return E_OUTOFMEMORY_; }
    if (data && size) memcpy(GPTR(b->data), data, size);
    void_com_wrap(&b->com, &blob_class, 0);
    b->com.destroy = blob_destroy;
    *out = b->com.guest;
    return 0;
}

/* ------------------------------------------------------------------ reflection */
typedef struct r10_reflect_type { void_com_object com; uint32_t desc[6]; } r10_reflect_type;
typedef struct r10_reflect_var { void_com_object com; uint32_t desc[5]; r10_reflect_type *type; } r10_reflect_var;
typedef struct r10_reflect_cb { void_com_object com; uint32_t desc[5]; r10_reflect_var **vars; uint32_t var_count; } r10_reflect_cb;
typedef struct r10_reflection {
    void_com_object com;
    uint32_t copy, size;         /* guest copy of the DXBC: the names point into it */
    const uint8_t *rdef; uint32_t rdef_size, rdef_va;
    const uint8_t *isgn, *osgn; uint32_t isgn_size, osgn_size, isgn_va, osgn_va;
    uint32_t version, creator, flags;
    r10_reflect_cb **cbs; uint32_t cb_count;
    uint32_t res_count, in_count, out_count;
    uint32_t null_cb, null_var;
    r10_reflect_cb *null_cb_obj;
} r10_reflection;

/* ID3D10ShaderReflectionType: GetDesc (D3D10_SHADER_TYPE_DESC: Class, Type, Rows, Columns, Elements, Members, Offset) */
static void *plain(void) { return void_com_host(THIS_); }
COM_METHOD(rtype, GetDesc, 2)
{
    r10_reflect_type *t = (r10_reflect_type *)plain();
    if (ARG(1)) memcpy(GPTR(ARG(1)), t->desc, sizeof t->desc);
    return 0;
}
COM_METHOD(rtype, GetMemberTypeByIndex, 2) { (void)plain(); return 0; }
COM_METHOD(rtype, GetMemberTypeByName, 2) { (void)plain(); return 0; }
COM_METHOD(rtype, GetMemberTypeName, 2) { (void)plain(); return 0; }
static const void_fn rtype_methods[] = { rtype_GetDesc, rtype_GetMemberTypeByIndex, rtype_GetMemberTypeByName, rtype_GetMemberTypeName };
static void_com_class rtype_class = { "ID3D10ShaderReflectionType", 4, rtype_methods, 0 };

COM_METHOD(rvar, GetDesc, 2)
{
    r10_reflect_var *v = (r10_reflect_var *)plain();
    if (ARG(1)) memcpy(GPTR(ARG(1)), v->desc, sizeof v->desc);
    return 0;
}
COM_METHOD(rvar, GetType, 1) { return ((r10_reflect_var *)plain())->type->com.guest; }
static const void_fn rvar_methods[] = { rvar_GetDesc, rvar_GetType };
static void_com_class rvar_class = { "ID3D10ShaderReflectionVariable", 2, rvar_methods, 0 };

COM_METHOD(rcb, GetDesc, 2)
{
    r10_reflect_cb *c = (r10_reflect_cb *)plain();
    if (ARG(1)) memcpy(GPTR(ARG(1)), c->desc, sizeof c->desc);
    return 0;
}
static r10_reflection *reflection_of_cb;   /* set while a reflector is alive; variables of a bad index use its null variable */
COM_METHOD(rcb, GetVariableByIndex, 2)
{
    r10_reflect_cb *c = (r10_reflect_cb *)plain();
    if (ARG(1) < c->var_count) return c->vars[ARG(1)]->com.guest;
    return reflection_of_cb ? reflection_of_cb->null_var : 0;
}
COM_METHOD(rcb, GetVariableByName, 2)
{
    r10_reflect_cb *c = (r10_reflect_cb *)plain();
    if (ARG(1)) for (uint32_t i = 0; i < c->var_count; ++i)
        if (c->vars[i]->desc[0] && !strcmp((const char *)GPTR(c->vars[i]->desc[0]), (const char *)GPTR(ARG(1)))) return c->vars[i]->com.guest;
    return reflection_of_cb ? reflection_of_cb->null_var : 0;
}
static const void_fn rcb_methods[] = { rcb_GetDesc, rcb_GetVariableByIndex, rcb_GetVariableByName };
static void_com_class rcb_class = { "ID3D10ShaderReflectionConstantBuffer", 3, rcb_methods, 0 };

static void plain_free(void_com_object *o)
{
    o->destroy = NULL;
    o->refs = 1;
    void_com_release(o);
}

static void var_free(r10_reflect_var *v)
{
    if (!v) return;
    if (v->type) plain_free(&v->type->com);
    plain_free(&v->com);
}

static void cb_free(r10_reflect_cb *c)
{
    if (!c) return;
    for (uint32_t k = 0; k < c->var_count; ++k) var_free(c->vars[k]);
    free(c->vars);
    plain_free(&c->com);
}

static void reflection_destroy(void_com_object *o)
{
    r10_reflection *r = (r10_reflection *)o;
    for (uint32_t i = 0; i < r->cb_count; ++i) cb_free(r->cbs[i]);
    cb_free(r->null_cb_obj);
    free(r->cbs);
    if (reflection_of_cb == r) reflection_of_cb = NULL;
    if (r->copy) void_gfree(r->copy);
    free(r);
}

static void sig_desc(r10_reflection *r, const uint8_t *c, uint32_t n, uint32_t base_va, uint32_t index, uint32_t out)
{
    memset(GPTR(out), 0, 24);
    if (!c || index >= rd32(c) || 8 + 24 * (index + 1) > n) return;
    const uint8_t *e = c + 8 + 24 * index;
    MEM32(out) = base_va + rd32(e);       /* SemanticName */
    MEM32(out + 4) = rd32(e + 4);         /* SemanticIndex */
    MEM32(out + 8) = rd32(e + 16);        /* Register */
    MEM32(out + 12) = rd32(e + 8);        /* SystemValueType */
    MEM32(out + 16) = rd32(e + 12);       /* ComponentType */
    MEM8(out + 20) = e[20];               /* Mask */
    MEM8(out + 21) = e[21];               /* ReadWriteMask */
    (void)r;
}

COM_METHOD(refl, QueryInterface, 3)
{
    r10_reflection *r = (r10_reflection *)void_com_host(THIS_);
    void_com_addref(&r->com);
    if (ARG(2)) MEM32(ARG(2)) = THIS_;
    return 0;
}
COM_METHOD(refl, AddRef, 1) { return void_com_addref((void_com_object *)void_com_host(THIS_)); }
COM_METHOD(refl, Release, 1) { return void_com_release((void_com_object *)void_com_host(THIS_)); }
COM_METHOD(refl, GetDesc, 2)
{
    r10_reflection *r = (r10_reflection *)void_com_host(THIS_);
    uint32_t out = ARG(1);
    if (!out) return E_INVALIDARG_;
    memset(GPTR(out), 0, 112);
    MEM32(out) = r->version;
    MEM32(out + 4) = r->creator;
    MEM32(out + 8) = r->flags;
    MEM32(out + 12) = r->cb_count;
    MEM32(out + 16) = r->res_count;
    MEM32(out + 20) = r->in_count;
    MEM32(out + 24) = r->out_count;
    return 0;
}
COM_METHOD(refl, GetConstantBufferByIndex, 2)
{
    r10_reflection *r = (r10_reflection *)void_com_host(THIS_);
    reflection_of_cb = r;
    return ARG(1) < r->cb_count ? r->cbs[ARG(1)]->com.guest : r->null_cb;
}
COM_METHOD(refl, GetConstantBufferByName, 2)
{
    r10_reflection *r = (r10_reflection *)void_com_host(THIS_);
    reflection_of_cb = r;
    if (ARG(1)) for (uint32_t i = 0; i < r->cb_count; ++i)
        if (!strcmp((const char *)GPTR(r->cbs[i]->desc[0]), (const char *)GPTR(ARG(1)))) return r->cbs[i]->com.guest;
    return r->null_cb;
}
COM_METHOD(refl, GetResourceBindingDesc, 3)
{
    r10_reflection *r = (r10_reflection *)void_com_host(THIS_);
    uint32_t index = ARG(1), out = ARG(2);
    if (!out || index >= r->res_count) return E_INVALIDARG_;
    const uint8_t *e = r->rdef + rd32(r->rdef + 12) + 32 * index;
    MEM32(out) = r->rdef_va + rd32(e);    /* Name */
    MEM32(out + 4) = rd32(e + 4);         /* Type */
    MEM32(out + 8) = rd32(e + 20);        /* BindPoint */
    MEM32(out + 12) = rd32(e + 24);       /* BindCount */
    MEM32(out + 16) = rd32(e + 28);       /* uFlags */
    MEM32(out + 20) = rd32(e + 8);        /* ReturnType */
    MEM32(out + 24) = rd32(e + 12);       /* Dimension */
    MEM32(out + 28) = rd32(e + 16);       /* NumSamples */
    return 0;
}
COM_METHOD(refl, GetInputParameterDesc, 3)
{
    r10_reflection *r = (r10_reflection *)void_com_host(THIS_);
    if (!ARG(2) || ARG(1) >= r->in_count) return E_INVALIDARG_;
    sig_desc(r, r->isgn, r->isgn_size, r->isgn_va, ARG(1), ARG(2));
    return 0;
}
COM_METHOD(refl, GetOutputParameterDesc, 3)
{
    r10_reflection *r = (r10_reflection *)void_com_host(THIS_);
    if (!ARG(2) || ARG(1) >= r->out_count) return E_INVALIDARG_;
    sig_desc(r, r->osgn, r->osgn_size, r->osgn_va, ARG(1), ARG(2));
    return 0;
}
static const void_fn refl_methods[] = { refl_QueryInterface, refl_AddRef, refl_Release, refl_GetDesc, refl_GetConstantBufferByIndex,
                                        refl_GetConstantBufferByName, refl_GetResourceBindingDesc, refl_GetInputParameterDesc,
                                        refl_GetOutputParameterDesc };
static void_com_class refl_class = { "ID3D10ShaderReflection", 9, refl_methods, 0 };

static r10_reflect_var *make_var(r10_reflection *r, const uint8_t *v, uint32_t rdef_n)
{
    r10_reflect_var *var = (r10_reflect_var *)calloc(1, sizeof *var);
    if (v) {
        var->desc[0] = r->rdef_va + rd32(v);  /* Name */
        var->desc[1] = rd32(v + 4);           /* StartOffset */
        var->desc[2] = rd32(v + 8);           /* Size */
        var->desc[3] = rd32(v + 12);          /* uFlags */
        uint32_t def = rd32(v + 20);
        var->desc[4] = def ? r->rdef_va + def : 0;
    }
    var->type = (r10_reflect_type *)calloc(1, sizeof *var->type);
    uint32_t t = v ? rd32(v + 16) : 0;
    if (t && t + 12 <= rdef_n) {
        uint16_t w[6];
        memcpy(w, r->rdef + t, 12);
        for (int i = 0; i < 6; ++i) var->type->desc[i] = w[i];   /* Class, Type, Rows, Columns, Elements, Members */
    }
    void_com_wrap(&var->type->com, &rtype_class, 0);
    void_com_wrap(&var->com, &rvar_class, 0);
    return var;
}

uint32_t r10_reflect_create(const uint8_t *dxbc, uint32_t size, uint32_t *out)
{
    r10_reflection *r = (r10_reflection *)calloc(1, sizeof *r);
    r->copy = void_galloc(size);
    if (!r->copy) { free(r); return E_OUTOFMEMORY_; }
    memcpy(GPTR(r->copy), dxbc, size);
    r->size = size;
    const uint8_t *copy = (const uint8_t *)GPTR(r->copy);
    r->rdef = dxbc_chunk(copy, size, FOURCC('R', 'D', 'E', 'F'), &r->rdef_size);
    r->isgn = dxbc_chunk(copy, size, FOURCC('I', 'S', 'G', 'N'), &r->isgn_size);
    r->osgn = dxbc_chunk(copy, size, FOURCC('O', 'S', 'G', 'N'), &r->osgn_size);
    if (r->rdef) r->rdef_va = r->copy + (uint32_t)(r->rdef - copy);
    if (r->isgn) { r->isgn_va = r->copy + (uint32_t)(r->isgn - copy); r->in_count = rd32(r->isgn); }
    if (r->osgn) { r->osgn_va = r->copy + (uint32_t)(r->osgn - copy); r->out_count = rd32(r->osgn); }
    uint32_t shex_n, token = 0;
    const uint8_t *shex = dxbc_chunk(copy, size, FOURCC('S', 'H', 'D', 'R'), &shex_n);
    if (!shex) shex = dxbc_chunk(copy, size, FOURCC('S', 'H', 'E', 'X'), &shex_n);
    if (shex && shex_n >= 4) token = rd32(shex);
    else if (!memcmp(copy, R10_IFACE_MAGIC, 4)) token = rd32(copy + 4);   /* the program's version token */
    if (token) r->version = ((token >> 16) << 16) | (((token >> 4) & 15) << 4) | (token & 15);
    if (r->rdef && r->rdef_size >= 28) {
        uint32_t cb_count = rd32(r->rdef), cb_off = rd32(r->rdef + 4);
        r->res_count = rd32(r->rdef + 8);
        r->flags = rd32(r->rdef + 20);
        uint32_t creator = rd32(r->rdef + 24);
        r->creator = creator < r->rdef_size ? r->rdef_va + creator : 0;
        if ((uint64_t)rd32(r->rdef + 12) + 32ull * r->res_count > r->rdef_size) r->res_count = 0;
        if ((uint64_t)cb_off + 24ull * cb_count > r->rdef_size) cb_count = 0;
        r->cbs = (r10_reflect_cb **)calloc(cb_count ? cb_count : 1, sizeof *r->cbs);
        for (uint32_t i = 0; i < cb_count; ++i) {
            const uint8_t *c = r->rdef + cb_off + 24 * i;
            r10_reflect_cb *cb = (r10_reflect_cb *)calloc(1, sizeof *cb);
            cb->desc[0] = r->rdef_va + rd32(c);       /* Name */
            cb->desc[1] = rd32(c + 20);               /* Type */
            cb->desc[2] = rd32(c + 4);                /* Variables */
            cb->desc[3] = rd32(c + 12);               /* Size */
            cb->desc[4] = rd32(c + 16);               /* uFlags */
            uint32_t vars = rd32(c + 4), var_off = rd32(c + 8);
            if ((uint64_t)var_off + 24ull * vars > r->rdef_size) vars = 0;
            cb->vars = (r10_reflect_var **)calloc(vars ? vars : 1, sizeof *cb->vars);
            for (uint32_t k = 0; k < vars; ++k) cb->vars[cb->var_count++] = make_var(r, r->rdef + var_off + 24 * k, r->rdef_size);
            void_com_wrap(&cb->com, &rcb_class, 0);
            r->cbs[r->cb_count++] = cb;
        }
    }
    /* The objects D3D returns for an index out of range: descriptions of zeros. */
    r10_reflect_cb *null_cb = (r10_reflect_cb *)calloc(1, sizeof *null_cb);
    null_cb->vars = (r10_reflect_var **)calloc(1, sizeof *null_cb->vars);
    null_cb->vars[0] = make_var(r, NULL, 0);
    null_cb->var_count = 1;
    void_com_wrap(&null_cb->com, &rcb_class, 0);
    r->null_cb_obj = null_cb;
    r->null_cb = null_cb->com.guest;
    r->null_var = null_cb->vars[0]->com.guest;
    void_com_wrap(&r->com, &refl_class, 0);
    r->com.destroy = reflection_destroy;
    reflection_of_cb = r;
    *out = r->com.guest;
    return 0;
}

/* ------------------------------------------------------------------ exports */
WINAPI_EXPORT("d3d10.dll", D3D10ReflectShader, 3)
{
    uint32_t code = ARG(0), size = ARG(1), out = ARG(2);
    if (!code || !out || size < 32 || !void_vm_readable(code, size)) return E_INVALIDARG_;
    const r10_lib_shader *lib = r10_shaderlib_token((const uint8_t *)GPTR(code), size);
    if (!lib) {
        if (r10_dev) ++r10_dev->shader_stats.from_dxbc;
        return r10_reflect_create((const uint8_t *)GPTR(code), size, (uint32_t *)GPTR(out));
    }
    uint32_t n = 0;
    uint8_t *iface = r10_shaderlib_interface(lib, &n);
    if (!iface) return E_FAIL_;
    uint32_t hr = r10_reflect_create(iface, n, (uint32_t *)GPTR(out));
    free(iface);
    return hr;
}

static uint32_t error_blob(const char *text, uint32_t out)
{
    if (!out) return 0;
    uint32_t blob = 0;
    if (!r10_blob_create(text, (uint32_t)strlen(text) + 1, &blob)) MEM32(out) = blob;
    return 0;
}

/* ------------------------------------------------------------------ D3DX10CompileFromMemory */
/* The engine compiles the HLSL Bioshock.exe embeds (the Flash UI and the Bink movie shaders) and, only when
 * its own cache lookup is not answered by the library (BIOSHOCK_R10_SHADERS=dxbc, r10_engine.c), a material
 * shader ShaderCache.pcs10 lacks, passing its ID3D10Include, which resolves <MATERIALFACTORY>,
 * <VERTEXFACTORY> and the files of shaders.spk.  The request is made self-contained here: every #include line
 * is replaced by what the game's include handler returns, recursively.  The key is the FNV-1a 64 of that
 * text, the defines, the entry, the profile and the flags (r10_request_key, tools/build_shaderlib10.py
 * request_key); the library answers with the token of the program the PC compiled from the same text.  A
 * request the library lacks is written to <userdata>/r10-requests/<key>.req for tools/build_shaderlib10.py,
 * and the game gets a program of the same material, else a placeholder shader of the stage (draws with it
 * are skipped) instead of a compile error it does not survive. */
typedef struct text_buf { char *data; size_t size, cap; } text_buf;

static void buf_append(text_buf *b, const void *data, size_t n)
{
    if (b->size + n + 1 > b->cap) {
        size_t cap = b->cap ? b->cap : 4096;
        while (b->size + n + 1 > cap) cap *= 2;
        b->data = (char *)realloc(b->data, cap);
        b->cap = cap;
    }
    memcpy(b->data + b->size, data, n);
    b->size += n;
    b->data[b->size] = 0;
}

/* ID3D10Include::Open(this, type, name, parent, ppData, pBytes) and Close(this, pData): stdcall, no IUnknown. */
static int include_open(uint32_t include, int system, const char *name, uint32_t parent, uint32_t *data, uint32_t *bytes)
{
    uint32_t scratch = void_galloc(8), name_va = void_gstrdup(name);
    if (!scratch || !name_va) return 0;
    MEM32(scratch) = 0; MEM32(scratch + 4) = 0;
    uint32_t args[6] = { include, system ? 1u : 0u, name_va, parent, scratch, scratch + 4 };
    void_regs saved;
    void_regs_save(&saved);
    uint32_t hr = void_call_guest(MEM32(MEM32(include)), 6, args, 1);
    void_regs_load(&saved);
    *data = MEM32(scratch);
    *bytes = MEM32(scratch + 4);
    void_gfree(scratch);
    void_gfree(name_va);
    return hr == 0 && *data && void_vm_readable(*data, *bytes);
}

static void include_close(uint32_t include, uint32_t data)
{
    uint32_t args[2] = { include, data };
    void_regs saved;
    void_regs_save(&saved);
    (void)void_call_guest(MEM32(MEM32(include) + 4), 2, args, 1);
    void_regs_load(&saved);
}

static void resolve_includes(uint32_t include, const char *text, size_t len, uint32_t parent, text_buf *out, int depth)
{
    size_t at = 0;
    while (at < len) {
        size_t end = at;
        while (end < len && text[end] != '\n') ++end;
        size_t next = end < len ? end + 1 : end;
        const char *p = text + at, *stop = text + end;
        while (p < stop && (*p == ' ' || *p == '\t')) ++p;
        int handled = 0;
        if (include && depth < 16 && p < stop && *p == '#') {
            ++p;
            while (p < stop && (*p == ' ' || *p == '\t')) ++p;
            if (stop - p > 7 && !memcmp(p, "include", 7)) {
                p += 7;
                while (p < stop && (*p == ' ' || *p == '\t')) ++p;
                char open = p < stop ? *p : 0, close = open == '<' ? '>' : '"';
                const char *name = p + 1, *name_end = name;
                while (name_end < stop && *name_end != close) ++name_end;
                if ((open == '<' || open == '"') && name_end < stop && name_end - name < 256) {
                    char file[260];
                    memcpy(file, name, (size_t)(name_end - name));
                    file[name_end - name] = 0;
                    uint32_t data = 0, bytes = 0;
                    if (include_open(include, open == '<', file, parent, &data, &bytes)) {
                        /* The included text is copied before Close frees it. */
                        char *copy = (char *)malloc(bytes + 1);
                        memcpy(copy, GPTR(data), bytes);
                        copy[bytes] = 0;
                        buf_append(out, "\n", 1);
                        resolve_includes(include, copy, bytes, data, out, depth + 1);
                        buf_append(out, "\n", 1);
                        free(copy);
                        include_close(include, data);
                        handled = 1;
                    }
                }
            }
        }
        if (!handled) buf_append(out, text + at, next - at);
        at = next;
    }
}

/* Placeholder shaders of the library: a valid shader of the stage that the renderer does not draw. */
#define R10_PLACEHOLDER_VS 0xFFFFFFFFFFFFFF01ull
#define R10_PLACEHOLDER_PS 0xFFFFFFFFFFFFFF02ull

WINAPI_EXPORT("d3dx10_33.dll", D3DX10CompileFromMemory, 13)
{
    uint32_t src = ARG(0), len = ARG(1), defines = ARG(3), include = ARG(4), entry = ARG(5), profile = ARG(6), flags1 = ARG(7);
    uint32_t shader_out = ARG(10), errors_out = ARG(11), hr_out = ARG(12);
    if (shader_out) MEM32(shader_out) = 0;
    if (errors_out) MEM32(errors_out) = 0;
    if (!src || !entry || !profile || !void_vm_readable(src, len)) return E_INVALIDARG_;
    r10_shaderlib_open();
    const char *entry_name = (const char *)GPTR(entry), *profile_name = (const char *)GPTR(profile);
    text_buf text = { 0 };
    resolve_includes(include, (const char *)GPTR(src), len, 0, &text, 0);
    if (!text.data) buf_append(&text, "", 0);
    uint64_t key = r10_request_key((const uint8_t *)text.data, (uint32_t)text.size, entry_name, profile_name, flags1, defines);
    int32_t program = r10_shaderlib_request(key);
    if (r10_dev) ++r10_dev->shader_stats.compiles;
    if (program < 0) {
        static unsigned misses;
        ++misses;
        if (r10_dev) ++r10_dev->shader_stats.compile_misses;
        const char *dir = void_data_dir();
        char path[1200];
        int recorded = 0;
        if (dir) {
            snprintf(path, sizeof path, "%s/r10-requests", dir);
            mkdir(path, 0775);
            snprintf(path, sizeof path, "%s/r10-requests/%016llx.req", dir, (unsigned long long)key);
            FILE *f = fopen(path, "wb");
            if (f) {
                /* key, entry, profile, flags, then the defines as name/value lines, then the text. */
                fprintf(f, "R10REQ1\nkey=%016llx\nentry=%s\nprofile=%s\nflags=%u\n", (unsigned long long)key, entry_name, profile_name, flags1);
                for (uint32_t at = defines; at && MEM32(at); at += 8) {
                    const char *name = (const char *)GPTR(MEM32(at)), *value = MEM32(at + 4) ? (const char *)GPTR(MEM32(at + 4)) : "";
                    fprintf(f, "define=%zu:%s%zu:%s\n", strlen(name), name, strlen(value), value);
                }
                fprintf(f, "source=%zu\n", text.size);
                fwrite(text.data, 1, text.size, f);
                recorded = !fclose(f);
            }
        }
        /* The same material compiled for other scene options (light count, shadow options), else the same
         * material's literals in any pass variant, else an invisible placeholder. */
        int level = 0;
        for (int l = 1; l <= 2 && program < 0; ++l) {
            program = r10_shaderlib_request(r10_request_family((const uint8_t *)text.data, (uint32_t)text.size, entry_name,
                                                               profile_name, flags1, defines, l));
            if (program >= 0) level = l;
        }
        if (misses <= 256)
            R10_LOG("COMPILE_MISS %016llx %s %s flags %08X (%zu bytes after includes)%s: %s", (unsigned long long)key, entry_name,
                    profile_name, flags1, text.size, recorded ? ", request recorded" : "",
                    level == 1 ? "the material's shader for other scene options is used" :
                    level == 2 ? "a shader of the same material is used" : "a placeholder shader is used");
        if (level && r10_dev) ++r10_dev->shader_stats.compile_fallbacks;
        if (program < 0) program = r10_shaderlib_request(profile_name[0] == 'v' ? R10_PLACEHOLDER_VS : R10_PLACEHOLDER_PS);
    }
    free(text.data);
    uint32_t token = program >= 0 ? r10_shaderlib_token_va((uint32_t)program) : 0;
    if (!token) {
        error_blob("native renderer: shader request not in the build-time library", errors_out);
        if (hr_out) MEM32(hr_out) = E_FAIL_;
        return E_FAIL_;
    }
    uint32_t blob = 0, hr = r10_blob_create(GPTR(token), R10_TOKEN_SIZE, &blob);
    if (!hr && shader_out) MEM32(shader_out) = blob;
    if (hr_out) MEM32(hr_out) = hr;
    return hr;
}

/* The engine preprocesses only to log or hash the text; the text itself is returned. */
WINAPI_EXPORT("d3dx10_33.dll", D3DX10PreprocessShaderFromMemory, 9)
{
    uint32_t src = ARG(0), len = ARG(1), text_out = ARG(6), errors_out = ARG(7), hr_out = ARG(8);
    if (text_out) MEM32(text_out) = 0;
    if (errors_out) MEM32(errors_out) = 0;
    if (!src || !void_vm_readable(src, len)) return E_INVALIDARG_;
    static unsigned logged;
    if (logged++ < 4) R10_LOG("D3DX10PreprocessShaderFromMemory: %u bytes returned unchanged", len);
    uint32_t blob = 0;
    uint8_t *copy = (uint8_t *)malloc(len + 1);
    memcpy(copy, GPTR(src), len);
    copy[len] = 0;
    uint32_t hr = r10_blob_create(copy, len + 1, &blob);
    free(copy);
    if (!hr && text_out) MEM32(text_out) = blob;
    if (hr_out) MEM32(hr_out) = hr;
    return hr;
}

/* 0 when the format is not 8-bit four-channel UNORM, else 1 for R8G8B8A8 and 2 for B8G8R8A8/X8 byte order. */
static int rgba8_order(const r10_format *f)
{
    switch (f ? f->dxgi : 0) {
    case 28: case 29: return 1;
    case 87: case 88: case 91: case 93: return 2;
    default: return 0;
    }
}

/* One level of D3DX10LoadTextureFromTexture whose boxes differ in size: the source rectangle read back, resampled on
 * the CPU (r10_resample.h) and written into the destination rectangle. 1 when done. */
static int load_resampled(r10_device *d, r10_resource *dst, uint32_t dst_sub, const uint32_t dr[4], r10_resource *src,
                          uint32_t src_sub, const uint32_t sr[4], uint32_t filter)
{
    int so = rgba8_order(src->fmt), dso = rgba8_order(dst->fmt);
    if (!so || !dso || src->desc.dim != R10_DIM_TEXTURE2D || dst->desc.dim != R10_DIM_TEXTURE2D) return 0;
    uint64_t start = r10_now_ns();
    const uint8_t *texels = NULL;
    uint8_t *read = NULL;
    size_t pitch = (size_t)sr[2] * 4u;
    if (src->desc.usage == D3D10_USAGE_STAGING) {
        if (!src->shadow) return 0;
        pitch = src->sub_pitch[src_sub];
        texels = (const uint8_t *)GPTR(src->shadow + src->sub_offset[src_sub]) + (size_t)sr[1] * pitch + (size_t)sr[0] * 4u;
    } else {
        if (!d->be->read_region || !(read = d->be->read_region(d, src, src_sub, sr[0], sr[1], sr[2], sr[3]))) return 0;
        texels = read;
    }
    size_t out_pitch = (size_t)dr[2] * 4u;
    uint8_t *out = (uint8_t *)malloc(out_pitch * dr[3]);
    int ok = out && r10_resample_rgba8(texels, sr[2], sr[3], pitch, out, dr[2], dr[3], out_pitch, r10_resample_point(filter),
                                       so != dso, src->fmt->gl_alpha_one, dst->fmt->gl_alpha_one);
    free(read);
    if (ok && dst->desc.usage == D3D10_USAGE_STAGING) {
        /* A staging texture lives in its shadow, which Map returns. */
        ok = dst->shadow || r10_resource_shadow(d, dst);
        if (ok) {
            uint8_t *base = (uint8_t *)GPTR(dst->shadow + dst->sub_offset[dst_sub]);
            uint32_t dp = dst->sub_pitch[dst_sub];
            for (uint32_t y = 0; y < dr[3]; ++y)
                memcpy(base + (size_t)(dr[1] + y) * dp + (size_t)dr[0] * 4u, out + (size_t)y * out_pitch, out_pitch);
        }
    } else if (ok) {
        r10_texture_write(d, dst, dst_sub, dr[0], dr[1], 0, dr[2], dr[3], 1, out, (uint32_t)out_pitch, (uint32_t)(out_pitch * dr[3]));
    }
    free(out);
    static unsigned logged;
    if (logged++ < 16)
        R10_LOG("D3DX10LoadTextureFromTexture: %ux%u at %u,%u of %s -> %ux%u at %u,%u of %s, filter %08X: %s in %.1f ms", sr[2], sr[3],
                sr[0], sr[1], src->fmt->name, dr[2], dr[3], dr[0], dr[1], dst->fmt->name, filter, ok ? "resampled" : "failed",
                (double)(r10_now_ns() - start) / 1e6);
    return ok;
}

/* D3DX10_TEXTURE_LOAD_INFO: pSrcBox, pDstBox, SrcFirstMip, DstFirstMip, NumMips, SrcFirstElement,
 * DstFirstElement, NumElements, Filter, MipFilter.  The engine copies between textures of one size (ReadPixels,
 * 0x1097B260) and loads the research camera's photo: the subject's box of the back buffer into a 256x256 staging
 * texture (0x109B75A0), which D3DX scales.  The boxes apply to the first level. */
WINAPI_EXPORT("d3dx10_33.dll", D3DX10LoadTextureFromTexture, 3)
{
    r10_resource *src = r10_resource_from(ARG(0)), *dst = r10_resource_from(ARG(2));
    uint32_t info = ARG(1);
    if (!src || !dst || !r10_dev) return E_INVALIDARG_;
    uint32_t src_box = info ? MEM32(info) : 0, dst_box = info ? MEM32(info + 4) : 0;
    uint32_t src_mip = info ? MEM32(info + 8) : 0, dst_mip = info ? MEM32(info + 12) : 0;
    uint32_t mips = info ? MEM32(info + 16) : 0;
    uint32_t src_el = info ? MEM32(info + 20) : 0, dst_el = info ? MEM32(info + 24) : 0, elements = info ? MEM32(info + 28) : 0;
    uint32_t filter = info ? MEM32(info + 32) : 0xFFFFFFFFu;
    if (src_mip >= src->desc.mips || dst_mip >= dst->desc.mips) return E_INVALIDARG_;
    if (!mips || mips == 0xFFFFFFFFu) mips = src->desc.mips - src_mip < dst->desc.mips - dst_mip ? src->desc.mips - src_mip : dst->desc.mips - dst_mip;
    if (!elements || elements == 0xFFFFFFFFu) elements = src->desc.layers < dst->desc.layers ? src->desc.layers : dst->desc.layers;
    static unsigned logged;
    if (logged++ < 8)
        R10_LOG("D3DX10LoadTextureFromTexture %ux%u %s -> %ux%u %s mips %u elements %u%s%s", src->desc.width, src->desc.height,
                src->fmt->name, dst->desc.width, dst->desc.height, dst->fmt->name, mips, elements, src_box ? ", source box" : "",
                dst_box ? ", destination box" : "");
    uint32_t hr = 0;
    for (uint32_t e = 0; e < elements; ++e)
        for (uint32_t m = 0; m < mips; ++m) {
            uint32_t ssub = (src_el + e) * src->desc.mips + src_mip + m, dsub = (dst_el + e) * dst->desc.mips + dst_mip + m;
            if (ssub >= src->subresources || dsub >= dst->subresources) return E_INVALIDARG_;
            uint32_t sw, sh, sd, dw, dh, dd, sr[4], dr[4];
            r10_subresource_size(src, ssub, &sw, &sh, &sd);
            r10_subresource_size(dst, dsub, &dw, &dh, &dd);
            const uint32_t *sb = m == 0 && src_box ? (const uint32_t *)GPTR(src_box) : NULL;
            const uint32_t *db = m == 0 && dst_box ? (const uint32_t *)GPTR(dst_box) : NULL;
            if (!r10_resample_rect(sb, sw, sh, sr) || !r10_resample_rect(db, dw, dh, dr)) continue;
            if (sr[2] == dr[2] && sr[3] == dr[3]) {
                /* Same size: a copy (whole levels without a box, so the texture pack follows them). */
                int whole = !sr[0] && !sr[1] && sr[2] == sw && sr[3] == sh;
                uint32_t box[6] = { sr[0], sr[1], 0, sr[0] + sr[2], sr[1] + sr[3], 1 };
                r10_texture_copy(r10_dev, dst, dsub, dr[0], dr[1], 0, src, ssub, whole ? NULL : box);
            } else if (!load_resampled(r10_dev, dst, dsub, dr, src, ssub, sr, filter)) {
                hr = E_FAIL_;
            }
        }
    return hr;
}

/* ID3D10Device: state tracking with Direct3D 10's binding rules, object creation, and the backend
 * selection (BIOSHOCK_RENDERER: gles | vulkan; the launcher's renderer mode in android_main.c). */
#define _GNU_SOURCE
#include "r10.h"
#include "r10_shaderlib.h"
#include "r10_prune.h"
#include <sched.h>
#include <stdlib.h>
#include <sys/resource.h>
#include <strings.h>
#include <stdio.h>
#include <unistd.h>
#include <time.h>

r10_device *r10_dev;

const char *r10_backend_name(void)
{
    const char *mode = getenv("BIOSHOCK_RENDERER");
    return mode && *mode ? mode : "gles";
}

int r10_renderer_selected(void) { return 1; }

void r10_object_init(r10_object *o, r10_kind kind)
{
    o->kind = kind;
    o->id = r10_dev ? ++r10_dev->next_id : 0;
}

uint32_t r10_object_wrap(r10_object *o, void_com_class *cls, r10_kind kind)
{
    r10_object_init(o, kind);
    return void_com_wrap(&o->com, cls, 0);
}

static void_com_class *const *known_classes;
r10_object *r10_object_from(uint32_t guest, r10_kind kind)
{
    if (!guest) return NULL;
    r10_object *o = (r10_object *)void_com_host(guest);
    if (o->kind != kind) {
        R10_LOG("object %08X is kind %d, expected %d (caller %08X)", guest, o->kind, kind, g_cur_func);
        void_lift_trap(g_cur_func, "Direct3D 10 object of the wrong interface");
    }
    return o;
}

int void_r10_single_level_depth_srv(uint32_t guest)
{
    const r10_view *view = (const r10_view *)r10_object_from(guest, R10_KIND_SRV);
    return view && view->mips == 1 && view->fmt->kind == R10_FMT_DEPTH;
}

/* The first page of [va, va + size) the guest cannot read (write: write), or va + size when all can. */
static uint64_t first_inaccessible(uint32_t va, uint32_t size, int write)
{
    uint64_t end = (uint64_t)va + size;
    for (uint64_t at = va; at < end; at = (at & ~0xFFFull) + 0x1000) {
        uint32_t n = (uint32_t)((((at & ~0xFFFull) + 0x1000) < end ? ((at & ~0xFFFull) + 0x1000) : end) - at);
        if (!(write ? void_vm_writable((uint32_t)at, n) : void_vm_readable((uint32_t)at, n))) return at;
    }
    return end;
}

/* The engine's movie draw (UD3DRenderDevice10, 0x109AEE20) copies the current Bink frame into its movie texture:
 * BinkCopyToBuffer(bink, mapped data, row pitch, bink height, 0, 0, 0x80000006) at esp (seam at 0x109AF166). The
 * PlasmiNow background movie faulted inside that copy, reading the frame planes or writing the mapped texture
 * (Pixel 10a, 25 November 2026). This checks both sides: the BINKFRAMEBUFFERS planes (Y, cR, cB, A of the current
 * frame) and pitch x rows of the destination, against the movie texture's shadow. A copy that would fault is
 * skipped (the texture keeps its previous frame) and the inaccessible page is explained. */
int void_bioshock_bink_copy_guard(uint32_t esp, uint32_t device)
{
    static unsigned logs, problems;
    uint32_t bink = MEM32(esp), dest = MEM32(esp + 4), pitch = MEM32(esp + 8), rows = MEM32(esp + 12);
    const char *problem = NULL;
    uint64_t bad = 0;
    char planes[200] = "", *pp = planes;
    uint32_t w = 0, h = 0, fb = 0, frame = 0, total = 0;
    if (!bink || !void_vm_readable(bink, 0x330)) { problem = "movie"; bad = bink; }
    else {
        w = MEM32(bink); h = MEM32(bink + 4); fb = MEM32(bink + 0xB8);
        if (!fb || !void_vm_readable(fb, 24)) { problem = "frame buffers"; bad = fb; }
        else {
            total = MEM32(fb); frame = MEM32(fb + 20);
            uint32_t set = fb + 24 + 48 * frame;
            if (frame >= total || frame >= 8 || !void_vm_readable(set, 48)) { problem = "frame index"; bad = fb; }
            for (int k = 0; k < 4 && !problem; ++k) {
                uint32_t buffer = MEM32(set + 12 * k + 4), plane_pitch = MEM32(set + 12 * k + 8);
                uint32_t plane_rows = (k == 0 || k == 3) ? MEM32(fb + 8) : MEM32(fb + 16);
                pp += snprintf(pp, (size_t)(planes + sizeof planes - pp), " %c=%08X/%u", "YrbA"[k], buffer, plane_pitch);
                if (!buffer) { if (k != 3) { problem = "plane"; bad = 0; } continue; }
                uint64_t span = (uint64_t)plane_pitch * plane_rows;
                if (span > 0x4000000u) { problem = "plane size"; bad = buffer; continue; }
                uint64_t at = first_inaccessible(buffer, (uint32_t)span, 0);
                if (at < (uint64_t)buffer + span) { problem = k == 0 ? "Y plane" : k == 1 ? "cR plane" : k == 2 ? "cB plane" : "A plane"; bad = at; }
            }
        }
    }
    uint64_t dest_span = (uint64_t)pitch * rows;
    if (!problem) {
        if (!dest || dest_span > 0x4000000u) { problem = "destination"; bad = dest; }
        else {
            uint64_t at = first_inaccessible(dest, (uint32_t)dest_span, 1);
            if (at < (uint64_t)dest + dest_span) { problem = "destination"; bad = at; }
        }
    }
    uint32_t state = void_vm_readable(device + 0x56DC, 4) ? MEM32(device + 0x56DC) : 0;
    uint32_t texture = state && void_vm_readable(state + 0x14, 4) ? MEM32(state + 0x14) : 0;
    const r10_resource *r = texture ? r10_resource_from(texture) : NULL;
    /* Past the shadow but in accessible memory: the copy runs (it would on Windows) and is reported. */
    int outside = r && r->shadow && (dest < r->shadow || (uint64_t)dest + dest_span > (uint64_t)r->shadow + r->shadow_size);
    static uint32_t last_bink;
    int changed = bink != last_bink;
    last_bink = bink;
    if (problem || outside ? problems++ < 32 : (changed || logs < 6) && logs++ < 200)
        void_log("[bink-copy] movie %08X %ux%u frame %u of %u%s -> %08X pitch %u rows %u; texture %08X %ux%u shadow %08X+%X%s%s%s",
                 bink, w, h, frame, total, planes, dest, pitch, rows, texture, r ? r->desc.width : 0, r ? r->desc.height : 0,
                 r ? r->shadow : 0, r ? r->shadow_size : 0, outside ? "; destination outside the texture" : "",
                 problem ? "; SKIPPED: " : "", problem ? problem : "");
    if (problem && problems <= 32 && bad && bad < 0x100000000ull) void_vm_explain((uint32_t)bad, problem);
    return !problem;
}

r10_resource *r10_resource_from(uint32_t guest)
{
    if (!guest) return NULL;
    r10_object *o = (r10_object *)void_com_host(guest);
    if (o->kind != R10_KIND_BUFFER && o->kind != R10_KIND_TEXTURE1D && o->kind != R10_KIND_TEXTURE2D && o->kind != R10_KIND_TEXTURE3D) {
        R10_LOG("object %08X of kind %d is no resource (caller %08X)", guest, o->kind, g_cur_func);
        void_lift_trap(g_cur_func, "Direct3D 10 resource expected");
    }
    return (r10_resource *)o;
}

void r10_object_addref_guest(uint32_t guest)
{
    if (guest) void_com_addref((void_com_object *)void_com_host(guest));
}

void r10_object_release_guest(uint32_t guest)
{
    if (guest) void_com_release((void_com_object *)void_com_host(guest));
}

/* Bound objects hold a reference, as the Direct3D 10 runtime does: a released object stays alive while
 * a slot still holds it. */
static inline void hold(r10_object *o) { if (o) void_com_addref(&o->com); }
static inline void drop(r10_object *o) { if (o) void_com_release(&o->com); }
#define SET_REF(slot, value) do { r10_object *_n = (r10_object *)(value), *_o = (r10_object *)(slot); \
    if (_n != _o) { hold(_n); (slot) = (void *)_n; drop(_o); } } while (0)

void r10_unbind_resource(r10_device *d, r10_resource *r)
{
    (void)d; (void)r;   /* bindings hold references, so a bound resource is never destroyed */
}

/* ------------------------------------------------------------------ hazards */
static int views_overlap(const r10_view *a, const r10_view *b)
{
    if (!a || !b || a->res != b->res) return 0;
    if (a->res->desc.dim == R10_DIM_BUFFER) return 1;
    uint32_t a_mip_end = a->first_mip + a->mips, b_mip_end = b->first_mip + b->mips;
    uint32_t a_layer_end = a->first_layer + a->layers, b_layer_end = b->first_layer + b->layers;
    return a->first_mip < b_mip_end && b->first_mip < a_mip_end && a->first_layer < b_layer_end && b->first_layer < a_layer_end;
}

static int bound_as_target(r10_device *d, const r10_view *srv)
{
    for (uint32_t i = 0; i < d->st.rtv_count; ++i) if (views_overlap(d->st.rtv[i], srv)) return 1;
    return views_overlap(d->st.dsv, srv);
}

/* OMSetRenderTargets unbinds every shader resource that views one of the new targets. */
static void unbind_srvs_of_targets(r10_device *d)
{
    r10_stage_state *stages[2] = { &d->st.vs, &d->st.ps };
    for (int s = 0; s < 2; ++s)
        for (uint32_t i = 0; i < stages[s]->srv_max; ++i)
            if (stages[s]->srv[i] && bound_as_target(d, stages[s]->srv[i])) {
                SET_REF(stages[s]->srv[i], NULL);
                d->st.dirty |= s ? R10_DIRTY_PS_RESOURCES : R10_DIRTY_VS_RESOURCES;
            }
}

/* ------------------------------------------------------------------ IUnknown */
static r10_device *dev(void) { return (r10_device *)r10_object_from(THIS_, R10_KIND_DEVICE); }

static const uint8_t IID_IUnknown_[16] = { 0, 0, 0, 0, 0, 0, 0, 0, 0xC0, 0, 0, 0, 0, 0, 0, 0x46 };
static const uint8_t IID_ID3D10Device_[16] = { 0x0F, 0x4C, 0x7E, 0x9B, 0x2C, 0x34, 0x06, 0x41, 0xA1, 0x9F, 0x4F, 0x27, 0x04, 0xF6, 0x89, 0xF0 };
static const uint8_t IID_IDXGIDevice_[16] = { 0xFA, 0x77, 0xEC, 0x54, 0x77, 0x13, 0xE6, 0x44, 0x8C, 0x32, 0x88, 0xFD, 0x5F, 0x44, 0xC8, 0x4C };
static const uint8_t IID_IDXGIObject_[16] = { 0xB8, 0x2F, 0xC2, 0xAE, 0xF3, 0x76, 0x39, 0x46, 0x9B, 0xE0, 0x28, 0xEB, 0x43, 0xA6, 0x7A, 0x2E };
static const uint8_t IID_ID3D10Multithread_[16] = { 0x00, 0x4E, 0x7E, 0x9B, 0x2C, 0x34, 0x06, 0x41, 0xA1, 0x9F, 0x4F, 0x27, 0x04, 0xF6, 0x89, 0xF0 };

COM_METHOD(dev, QueryInterface, 3)
{
    r10_device *d = dev();
    uint32_t out = ARG(2);
    if (!out) return E_POINTER_;
    if (void_guid_equal(ARG(1), IID_IUnknown_) || void_guid_equal(ARG(1), IID_ID3D10Device_)) {
        void_com_addref(&d->obj.com);
        MEM32(out) = THIS_;
        return 0;
    }
    if ((void_guid_equal(ARG(1), IID_IDXGIDevice_) || void_guid_equal(ARG(1), IID_IDXGIObject_)) && d->dxgi_device) {
        void_com_addref((void_com_object *)void_com_host(d->dxgi_device));
        MEM32(out) = d->dxgi_device;
        return 0;
    }
    char text[40];
    void_guid_format(ARG(1), text);
    R10_LOG("ID3D10Device::QueryInterface %s: E_NOINTERFACE", text);
    MEM32(out) = 0;
    return E_NOINTERFACE_;
}
COM_METHOD(dev, AddRef, 1) { return void_com_addref(&dev()->obj.com); }
COM_METHOD(dev, Release, 1)
{
    r10_device *d = dev();
    /* The device lives for the process: the swap chain, the textures of the engine and the backend all
     * keep using it; only the count follows the game. */
    if (d->obj.com.refs > 1) return (uint32_t)--d->obj.com.refs;
    return 0;
}

/* ------------------------------------------------------------------ shader stages */
static void set_cbs(r10_device *d, r10_stage_state *s, uint32_t dirty, uint32_t start, uint32_t num, uint32_t array)
{
    for (uint32_t i = 0; i < num && start + i < R10_CB_SLOTS; ++i) {
        uint32_t g = array ? MEM32(array + 4 * i) : 0;
        r10_resource *r = g ? r10_resource_from(g) : NULL;
        if (r != s->cb[start + i]) { SET_REF(s->cb[start + i], r); d->st.dirty |= dirty; }
    }
}

static void set_srvs(r10_device *d, r10_stage_state *s, uint32_t dirty, uint32_t start, uint32_t num, uint32_t array)
{
    for (uint32_t i = 0; i < num && start + i < R10_SRV_SLOTS; ++i) {
        uint32_t g = array ? MEM32(array + 4 * i) : 0;
        r10_view *v = g ? (r10_view *)r10_object_from(g, R10_KIND_SRV) : NULL;
        if (v && bound_as_target(d, v)) {
            static unsigned logged;
            if (logged++ < 8) R10_LOG("shader resource slot %u views a bound render target: set to NULL (D3D10 hazard rule)", start + i);
            v = NULL;
        }
        if (v != s->srv[start + i]) { SET_REF(s->srv[start + i], v); d->st.dirty |= dirty; }
        if (v && start + i + 1 > s->srv_max) s->srv_max = start + i + 1;
    }
    while (s->srv_max && !s->srv[s->srv_max - 1]) --s->srv_max;
}

static void set_samplers(r10_device *d, r10_stage_state *s, uint32_t dirty, uint32_t start, uint32_t num, uint32_t array)
{
    for (uint32_t i = 0; i < num && start + i < R10_SAMPLER_SLOTS; ++i) {
        uint32_t g = array ? MEM32(array + 4 * i) : 0;
        r10_state_object *v = g ? (r10_state_object *)r10_object_from(g, R10_KIND_SAMPLER) : NULL;
        if (v != s->sampler[start + i]) { SET_REF(s->sampler[start + i], v); d->st.dirty |= dirty; }
    }
}

static void get_slots(void *const *slots, uint32_t limit, uint32_t start, uint32_t num, uint32_t out)
{
    for (uint32_t i = 0; i < num; ++i) {
        r10_object *o = start + i < limit ? (r10_object *)slots[start + i] : NULL;
        MEM32(out + 4 * i) = o ? o->com.guest : 0;
        hold(o);
    }
}

COM_METHOD(dev, VSSetConstantBuffers, 4) { r10_device *d = dev(); set_cbs(d, &d->st.vs, R10_DIRTY_VS_CB, ARG(1), ARG(2), ARG(3)); return 0; }
COM_METHOD(dev, PSSetConstantBuffers, 4) { r10_device *d = dev(); set_cbs(d, &d->st.ps, R10_DIRTY_PS_CB, ARG(1), ARG(2), ARG(3)); return 0; }
COM_METHOD(dev, GSSetConstantBuffers, 4) { r10_device *d = dev(); set_cbs(d, &d->st.gs, 0, ARG(1), ARG(2), ARG(3)); return 0; }
COM_METHOD(dev, VSSetShaderResources, 4) { r10_device *d = dev(); set_srvs(d, &d->st.vs, R10_DIRTY_VS_RESOURCES, ARG(1), ARG(2), ARG(3)); return 0; }
COM_METHOD(dev, PSSetShaderResources, 4) { r10_device *d = dev(); set_srvs(d, &d->st.ps, R10_DIRTY_PS_RESOURCES, ARG(1), ARG(2), ARG(3)); return 0; }
COM_METHOD(dev, GSSetShaderResources, 4) { r10_device *d = dev(); set_srvs(d, &d->st.gs, 0, ARG(1), ARG(2), ARG(3)); return 0; }
COM_METHOD(dev, VSSetSamplers, 4) { r10_device *d = dev(); set_samplers(d, &d->st.vs, R10_DIRTY_VS_RESOURCES, ARG(1), ARG(2), ARG(3)); return 0; }
COM_METHOD(dev, PSSetSamplers, 4) { r10_device *d = dev(); set_samplers(d, &d->st.ps, R10_DIRTY_PS_RESOURCES, ARG(1), ARG(2), ARG(3)); return 0; }
COM_METHOD(dev, GSSetSamplers, 4) { r10_device *d = dev(); set_samplers(d, &d->st.gs, 0, ARG(1), ARG(2), ARG(3)); return 0; }
COM_METHOD(dev, VSSetShader, 2)
{
    r10_device *d = dev();
    r10_shader *s = ARG(1) ? (r10_shader *)r10_object_from(ARG(1), R10_KIND_VS) : NULL;
    if (s != d->st.vs.shader) { SET_REF(d->st.vs.shader, s); d->st.dirty |= R10_DIRTY_SHADERS; }
    return 0;
}
COM_METHOD(dev, PSSetShader, 2)
{
    r10_device *d = dev();
    r10_shader *s = ARG(1) ? (r10_shader *)r10_object_from(ARG(1), R10_KIND_PS) : NULL;
    if (s != d->st.ps.shader) { SET_REF(d->st.ps.shader, s); d->st.dirty |= R10_DIRTY_SHADERS; }
    return 0;
}
COM_METHOD(dev, GSSetShader, 2)
{
    r10_device *d = dev();
    r10_shader *s = ARG(1) ? (r10_shader *)r10_object_from(ARG(1), R10_KIND_GS) : NULL;
    if (s && !d->disabled_gs_logged++) R10_LOG("geometry shader bound: its draws are skipped");
    SET_REF(d->st.gs.shader, s);
    return 0;
}
COM_METHOD(dev, VSGetConstantBuffers, 4) { r10_device *d = dev(); get_slots((void *const *)d->st.vs.cb, R10_CB_SLOTS, ARG(1), ARG(2), ARG(3)); return 0; }
COM_METHOD(dev, PSGetConstantBuffers, 4) { r10_device *d = dev(); get_slots((void *const *)d->st.ps.cb, R10_CB_SLOTS, ARG(1), ARG(2), ARG(3)); return 0; }
COM_METHOD(dev, GSGetConstantBuffers, 4) { r10_device *d = dev(); get_slots((void *const *)d->st.gs.cb, R10_CB_SLOTS, ARG(1), ARG(2), ARG(3)); return 0; }
COM_METHOD(dev, VSGetShaderResources, 4) { r10_device *d = dev(); get_slots((void *const *)d->st.vs.srv, R10_SRV_SLOTS, ARG(1), ARG(2), ARG(3)); return 0; }
COM_METHOD(dev, PSGetShaderResources, 4) { r10_device *d = dev(); get_slots((void *const *)d->st.ps.srv, R10_SRV_SLOTS, ARG(1), ARG(2), ARG(3)); return 0; }
COM_METHOD(dev, GSGetShaderResources, 4) { r10_device *d = dev(); get_slots((void *const *)d->st.gs.srv, R10_SRV_SLOTS, ARG(1), ARG(2), ARG(3)); return 0; }
COM_METHOD(dev, VSGetSamplers, 4) { r10_device *d = dev(); get_slots((void *const *)d->st.vs.sampler, R10_SAMPLER_SLOTS, ARG(1), ARG(2), ARG(3)); return 0; }
COM_METHOD(dev, PSGetSamplers, 4) { r10_device *d = dev(); get_slots((void *const *)d->st.ps.sampler, R10_SAMPLER_SLOTS, ARG(1), ARG(2), ARG(3)); return 0; }
COM_METHOD(dev, GSGetSamplers, 4) { r10_device *d = dev(); get_slots((void *const *)d->st.gs.sampler, R10_SAMPLER_SLOTS, ARG(1), ARG(2), ARG(3)); return 0; }
static void get_one(r10_object *o, uint32_t out) { if (out) { MEM32(out) = o ? o->com.guest : 0; hold(o); } }
COM_METHOD(dev, VSGetShader, 2) { get_one((r10_object *)dev()->st.vs.shader, ARG(1)); return 0; }
COM_METHOD(dev, PSGetShader, 2) { get_one((r10_object *)dev()->st.ps.shader, ARG(1)); return 0; }
COM_METHOD(dev, GSGetShader, 2) { get_one((r10_object *)dev()->st.gs.shader, ARG(1)); return 0; }

/* ------------------------------------------------------------------ input assembler */
COM_METHOD(dev, IASetInputLayout, 2)
{
    r10_device *d = dev();
    r10_layout *l = ARG(1) ? (r10_layout *)r10_object_from(ARG(1), R10_KIND_LAYOUT) : NULL;
    if (l != d->st.layout) { SET_REF(d->st.layout, l); d->st.dirty |= R10_DIRTY_LAYOUT; }
    return 0;
}
COM_METHOD(dev, IASetVertexBuffers, 6)
{
    r10_device *d = dev();
    uint32_t start = ARG(1), num = ARG(2), buffers = ARG(3), strides = ARG(4), offsets = ARG(5);
    for (uint32_t i = 0; i < num && start + i < R10_VB_SLOTS; ++i) {
        uint32_t g = buffers ? MEM32(buffers + 4 * i) : 0;
        r10_resource *r = g ? r10_resource_from(g) : NULL;
        uint32_t stride = strides ? MEM32(strides + 4 * i) : 0, offset = offsets ? MEM32(offsets + 4 * i) : 0;
        if (r != d->st.vb[start + i].buf || stride != d->st.vb[start + i].stride || offset != d->st.vb[start + i].offset) {
            SET_REF(d->st.vb[start + i].buf, r);
            d->st.vb[start + i].stride = stride;
            d->st.vb[start + i].offset = offset;
            d->st.dirty |= R10_DIRTY_VB;
        }
    }
    return 0;
}
COM_METHOD(dev, IASetIndexBuffer, 4)
{
    r10_device *d = dev();
    r10_resource *r = ARG(1) ? r10_resource_from(ARG(1)) : NULL;
    SET_REF(d->st.ib, r);
    d->st.ib_format = ARG(2);
    d->st.ib_offset = ARG(3);
    d->st.dirty |= R10_DIRTY_IB;
    return 0;
}
COM_METHOD(dev, IASetPrimitiveTopology, 2)
{
    r10_device *d = dev();
    if (d->st.topology != ARG(1)) { d->st.topology = ARG(1); d->st.dirty |= R10_DIRTY_TOPOLOGY; }
    return 0;
}
COM_METHOD(dev, IAGetInputLayout, 2) { get_one((r10_object *)dev()->st.layout, ARG(1)); return 0; }
COM_METHOD(dev, IAGetVertexBuffers, 6)
{
    r10_device *d = dev();
    for (uint32_t i = 0; i < ARG(2); ++i) {
        uint32_t s = ARG(1) + i;
        r10_object *o = s < R10_VB_SLOTS ? (r10_object *)d->st.vb[s].buf : NULL;
        if (ARG(3)) { MEM32(ARG(3) + 4 * i) = o ? o->com.guest : 0; hold(o); }
        if (ARG(4)) MEM32(ARG(4) + 4 * i) = s < R10_VB_SLOTS ? d->st.vb[s].stride : 0;
        if (ARG(5)) MEM32(ARG(5) + 4 * i) = s < R10_VB_SLOTS ? d->st.vb[s].offset : 0;
    }
    return 0;
}
COM_METHOD(dev, IAGetIndexBuffer, 4)
{
    r10_device *d = dev();
    get_one((r10_object *)d->st.ib, ARG(1));
    if (ARG(2)) MEM32(ARG(2)) = d->st.ib_format;
    if (ARG(3)) MEM32(ARG(3)) = d->st.ib_offset;
    return 0;
}
COM_METHOD(dev, IAGetPrimitiveTopology, 2) { if (ARG(1)) MEM32(ARG(1)) = dev()->st.topology; return 0; }

/* ------------------------------------------------------------------ draws */
static void stream_flags(r10_device *d)
{
    d->verify_stream = r10_env_int("BIOSHOCK_R10_VERIFY_STREAM", 0, 0, 1);
    d->stream_fix = r10_env_int("BIOSHOCK_R10_STREAM_FIX", 1, 0, 1);
}

typedef struct stream_indices {
    const uint8_t *bytes;
    uint32_t begin, count, size;
} stream_indices;

static stream_indices get_stream_indices(r10_device *d, uint32_t first, uint32_t count)
{
    r10_resource *ib = d->st.ib;
    stream_indices in = { NULL, 0, 0, d->st.ib_format == 42 ? 4u : 2u };
    uint64_t begin = d->st.ib_offset + (uint64_t)first * in.size;
    if (!ib || begin > ib->desc.width) return in;
    const uint8_t *data = r10_buffer_shadow(ib);
    if (!data) data = ib->host_copy;
    in.begin = (uint32_t)begin;
    in.count = (ib->desc.width - in.begin) / in.size;
    if (in.count > count) in.count = count;
    if (data) in.bytes = data + in.begin;
    return in;
}

static uint32_t stream_index(const stream_indices *in, uint32_t i)
{
    const uint8_t *p = in->bytes + (size_t)i * in->size;
    if (in->size == 4) { uint32_t v; memcpy(&v, p, 4); return v; }
    uint16_t v;
    memcpy(&v, p, 2);
    return v;
}

static uint64_t stream_restart(const r10_device *d, uint32_t size, int gles)
{
    if (gles || d->st.topology == D3D10_PRIMITIVE_TOPOLOGY_LINESTRIP || d->st.topology == D3D10_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP)
        return size == 4 ? UINT32_MAX : UINT16_MAX;
    return UINT64_MAX;
}

static void scan_index_range(const stream_indices *in, uint64_t restart, uint32_t *lo, uint32_t *hi)
{
    uint32_t mn = UINT32_MAX, mx = 0;
    for (uint32_t i = 0; i < in->count; ++i) {
        uint32_t v = stream_index(in, i);
        if (v == restart) continue;
        if (v < mn) mn = v;
        if (v > mx) mx = v;
    }
    if (mn > mx) mn = mx = 0;
    *lo = mn;
    *hi = mx == UINT32_MAX ? UINT32_MAX : mx + 1;
}

/* The vertices an indexed draw reads, from the index data the CPU sees (host copy or shadow). */
static void index_range(r10_device *d, uint32_t count, uint32_t first, uint32_t *lo, uint32_t *hi)
{
    r10_resource *ib = d->st.ib;
    stream_indices in = get_stream_indices(d, first, count);
    if (!in.bytes) { *lo = 0; *hi = UINT32_MAX; return; }
    uint64_t restart = stream_restart(d, in.size, !d->stream_fix || d->be == &r10_gles_backend);
    uint64_t id = d->stream_fix ? ib->obj.id : 0, writes = d->stream_fix ? ib->write_serial : 0;
    /* A small cache: the same range of a static index buffer is drawn many times. */
    typedef struct { r10_resource *ib; uint64_t id, version, writes, restart; uint32_t begin, count, size, lo, hi; } entry;
    static entry cache[256];
    entry *e = &cache[(((uintptr_t)ib >> 4) ^ in.begin * 2654435761u ^ in.count) & 255u];
    if (e->ib != ib || e->id != id || e->version != ib->version || e->writes != writes || e->restart != restart ||
        e->begin != in.begin || e->count != in.count || e->size != in.size) {
        *e = (entry){ ib, id, ib->version, writes, restart, in.begin, in.count, in.size, 0, 0 };
        scan_index_range(&in, restart, &e->lo, &e->hi);
    }
    *lo = e->lo;
    *hi = e->hi;
}

typedef struct stream_slot {
    uint32_t offset, stride, width, divisor;
    uint64_t extent;
} stream_slot;

static stream_slot stream_slot_layout(const r10_device *d, uint32_t slot, int gles)
{
    const r10_layout *l = d->st.layout;
    uint32_t stride = d->st.vb[slot].stride;
    stream_slot s = { d->st.vb[slot].offset, stride, d->st.vb[slot].buf->desc.width, 0, stride };
    for (uint32_t i = 0; i < l->count; ++i) {
        const r10_element *e = &l->el[i];
        if (e->slot != slot) continue;
        if (e->instanced) s.divisor = gles ? e->step : 1;
        const r10_format *f = gles ? NULL : r10_format_get(e->format);
        uint32_t bytes = gles ? r10_gles_vertex_bytes(e->format) : f ? f->bytes : 16;
        uint64_t end = (uint64_t)e->offset + bytes;
        if (end > s.extent) s.extent = end;   /* an input may extend beyond (or have a zero) stride */
    }
    return s;
}

static uint32_t stream_record_offset(const stream_slot *s, uint64_t vertex)
{
    if (vertex > UINT32_MAX && s->stride) return s->width;
    uint64_t at = s->offset + vertex * s->stride;
    return at < s->width ? (uint32_t)at : s->width;
}

static void stream_record_range(const stream_slot *s, uint64_t first, uint64_t end, uint32_t *lo, uint32_t *hi)
{
    *lo = stream_record_offset(s, first);
    if (end <= first) { *hi = *lo; return; }
    uint64_t last = stream_record_offset(s, end - 1) + s->extent;
    *hi = last < s->width ? (uint32_t)last : s->width;
}

static void stream_instance_range(const stream_slot *s, const r10_draw *dr, int gles, uint32_t *lo, uint32_t *hi)
{
    /* GLES issues Draw*Instanced without baseInstance. Vulkan binds one record per instance. */
    uint64_t first = gles ? 0 : dr->first_instance;
    stream_record_range(s, first, first + (dr->instances - 1) / s->divisor + 1, lo, hi);
}

void r10_stream_vertex_range(const r10_device *d, const r10_draw *dr, uint32_t slot, int gles, uint32_t *lo, uint32_t *hi)
{
    stream_slot s = stream_slot_layout(d, slot, gles);
    if (s.divisor) stream_instance_range(&s, dr, gles, lo, hi);
    else if (!dr->indexed) stream_record_range(&s, dr->first, (uint64_t)dr->first + dr->count, lo, hi);
    else if (dr->index_hi == UINT32_MAX) { *lo = s.offset < s.width ? s.offset : s.width; *hi = s.width; }
    else stream_record_range(&s, dr->vertex_lo, dr->vertex_hi, lo, hi);
}

static int stream_mapped(const r10_resource *r)
{
    return r && r->desc.usage == D3D10_USAGE_DYNAMIC && r->write_serial;
}

/* Compare only consumed records. A hole between sparse indices need not match the shadow. */
static int verify_stream_bytes(r10_device *d, r10_resource *r, int slot, const uint8_t *cpu,
                               const uint8_t *gpu, uint32_t lo, uint32_t hi)
{
    if (hi <= lo || !memcmp(cpu + lo, gpu + lo, hi - lo)) return 0;
    if (d->stream_stats.details < 20) {
        ++d->stream_stats.details;
        uint32_t at = lo;
        while (cpu[at] == gpu[at]) ++at;
        uint32_t offset = slot < 0 ? d->st.ib_offset : d->st.vb[slot].offset;
        R10_LOG("STREAM_STALE frame %llu draw %u buf %llu/%u/%u %s offset %u range [%u,%u) first_bad_byte %u cpu %02x gpu %02x "
                "map_history %08x last_frame %llu writes %llu slot %d",
                (unsigned long long)d->frame, d->stats.draws, (unsigned long long)r->obj.id, r->desc.width, r->desc.usage,
                slot < 0 ? "ib" : "vb", offset, lo, hi, at, cpu[at], gpu[at], r->map_history, (unsigned long long)r->map_frame,
                (unsigned long long)r->write_serial, slot);
    }
    return 1;
}

void r10_verify_stream(r10_device *d, const r10_draw *dr, r10_stream_visible visible, int gles)
{
    r10_resource *ib = dr->indexed ? d->st.ib : NULL;
    uint32_t vb_mask = 0;
    for (uint32_t slot = 0; d->st.layout && slot < R10_VB_SLOTS; ++slot)
        if ((d->st.layout->slot_mask & (1u << slot)) && stream_mapped(d->st.vb[slot].buf)) vb_mask |= 1u << slot;
    if (!vb_mask && !stream_mapped(ib)) return;

    int bad_vb = 0, bad_ib = 0, bounds = 0, unavailable = 0;
    stream_indices in = { 0 };
    uint64_t restart = UINT64_MAX;
    if (ib) {
        in = get_stream_indices(d, dr->first, dr->count);
        if (in.count < dr->count) unavailable = 1;
        restart = stream_restart(d, in.size, gles);
        if (in.bytes) {
            uint32_t lo, hi;
            scan_index_range(&in, restart, &lo, &hi);
            bounds = lo != dr->index_lo || hi != dr->index_hi;
            uint32_t end = in.begin + in.count * in.size;
            if (bounds && d->stream_stats.details < 20) {
                ++d->stream_stats.details;
                R10_LOG("STREAM_STALE frame %llu draw %u buf %llu/%u/%u ib offset %u range [%u,%u) first_bad_byte n/a "
                        "map_history %08x bounds cached [%u,%u) cpu [%u,%u)",
                        (unsigned long long)d->frame, d->stats.draws, (unsigned long long)ib->obj.id, ib->desc.width,
                        ib->desc.usage, d->st.ib_offset, in.begin, end, ib->map_history, dr->index_lo, dr->index_hi, lo, hi);
            }
            if (stream_mapped(ib)) {
                const uint8_t *gpu = visible(d, dr, -1);
                if (gpu) bad_ib = verify_stream_bytes(d, ib, -1, in.bytes - in.begin, gpu, in.begin, end);
                else unavailable = 1;
            }
        } else unavailable = 1;
    }
    for (uint32_t slot = 0; slot < R10_VB_SLOTS; ++slot) {
        if (!(vb_mask & (1u << slot))) continue;
        r10_resource *vb = d->st.vb[slot].buf;
        const uint8_t *cpu = r10_buffer_shadow(vb), *gpu = visible(d, dr, (int)slot);
        if (!cpu || !gpu) { unavailable = 1; continue; }
        stream_slot s = stream_slot_layout(d, slot, gles);
        uint32_t lo, hi;
        if (s.divisor || !dr->indexed) {
            if (s.divisor) stream_instance_range(&s, dr, gles, &lo, &hi);
            else stream_record_range(&s, dr->first, (uint64_t)dr->first + dr->count, &lo, &hi);
            bad_vb |= verify_stream_bytes(d, vb, (int)slot, cpu, gpu, lo, hi);
        } else if (in.bytes) {
            for (uint32_t i = 0; i < in.count; ++i) {
                uint32_t index = stream_index(&in, i);
                int64_t vertex = (int64_t)index + dr->base_vertex;
                if (index == restart || vertex < 0) continue;
                stream_record_range(&s, (uint64_t)vertex, (uint64_t)vertex + 1, &lo, &hi);
                if (verify_stream_bytes(d, vb, (int)slot, cpu, gpu, lo, hi)) { bad_vb = 1; break; }
            }
        }
    }
    ++d->stream_stats.checked;
    d->stream_stats.stale += bad_vb || bad_ib || bounds;
    d->stream_stats.vb += bad_vb;
    d->stream_stats.ib += bad_ib;
    d->stream_stats.bounds += bounds;
    d->stream_stats.unavailable += unavailable;
}

static void draw(r10_device *d, int indexed, uint32_t count, uint32_t instances, uint32_t first, int32_t base, uint32_t first_instance)
{
    if (!count || !instances) return;
    if (d->st.gs.shader) return;
    if (!d->st.vs.shader || !d->st.vs.shader->lib) return;
    if (d->st.ps.shader && !d->st.ps.shader->lib) return;
    r10_draw dr;
    dr.indexed = indexed;
    dr.count = count;
    dr.instances = instances;
    dr.first = first;
    dr.first_instance = first_instance;
    dr.base_vertex = base;
    if (indexed) {
        index_range(d, count, first, &dr.index_lo, &dr.index_hi);
        int64_t a = (int64_t)dr.index_lo + base, b = (int64_t)dr.index_hi + base;
        dr.vertex_lo = a < 0 ? 0 : (uint32_t)a;
        dr.vertex_hi = b < 0 ? 0 : b > 0xFFFFFFFFll ? 0xFFFFFFFFu : (uint32_t)b;
    } else {
        dr.vertex_lo = first;
        dr.vertex_hi = first + count;
    }
    ++d->stats.draws;
    /* BIOSHOCK_R10_TRACE=<frame>: every draw of that frame with its shaders, textures and blend state. */
    static long trace_frame = -2;
    static uint64_t trace_polled = ~0ull;
    if (trace_frame == -2) { const char *e = getenv("BIOSHOCK_R10_TRACE"); trace_frame = e ? atol(e) : -1; }
    /* <data>/r10-trace-now (adb shell touch) traces the next frame: gameplay frame numbers drift. */
    if (trace_polled != d->frame && !(d->frame & 63)) {
        trace_polled = d->frame;
        char path[1200];
        snprintf(path, sizeof path, "%s/r10-trace-now", void_data_dir() ? void_data_dir() : ".");
        if (!unlink(path)) { trace_frame = (long)d->frame + 1; R10_LOG("TRACE requested: frame %ld", trace_frame); }
    }
    d->tracing = trace_frame >= 0 && d->frame == (uint64_t)trace_frame;
    d->probe_first = first; d->probe_base = base;
    if (d->tracing) {
        if (d->st.rtv_count || d->st.dsv) {
            const r10_view *r0 = d->st.rtv_count ? d->st.rtv[0] : NULL, *ds = d->st.dsv;
            R10_LOG("TRACE   rt %u: %s %ux%u%s ds %s %ux%u vp %d,%d %dx%d", d->st.rtv_count, r0 ? r0->fmt->name : "-",
                    r0 ? r0->res->desc.width : 0, r0 ? r0->res->desc.height : 0, r0 && r0->res->is_backbuffer ? " (back buffer)" : "",
                    ds ? ds->fmt->name : "-", ds ? ds->res->desc.width : 0, ds ? ds->res->desc.height : 0,
                    d->st.viewports ? (int)d->st.viewport[0].x : 0, d->st.viewports ? (int)d->st.viewport[0].y : 0,
                    d->st.viewports ? (int)d->st.viewport[0].w : 0, d->st.viewports ? (int)d->st.viewport[0].h : 0);
        }
        const r10_state_object *bl = d->st.blend;
        char srvs[256];
        size_t n = 0;
        srvs[0] = 0;
        for (unsigned i = 0; i < 4 && n < sizeof srvs - 64; ++i) {
            const r10_view *v = d->st.ps.srv[i];
            if (v) n += (size_t)snprintf(srvs + n, sizeof srvs - n, " t%u=%s/%ux%u", i, v->fmt ? v->fmt->name : "?", v->res->desc.width, v->res->desc.height);
        }
        for (unsigned i = 0; i < 4 && n < sizeof srvs - 24; ++i) {
            const r10_state_object *sampler = d->st.ps.sampler[i];
            if (sampler) n += (size_t)snprintf(srvs + n, sizeof srvs - n, " s%u=%02x", i, sampler->d.sampler.filter);
        }
        R10_LOG("TRACE draw %u: vs %016llx ps %016llx count %u%s blend %u %u/%u %u/%u mask %X topo %u%s", d->stats.draws,
                (unsigned long long)d->st.vs.shader->hash, d->st.ps.shader ? (unsigned long long)d->st.ps.shader->hash : 0ull, count,
                indexed ? " indexed" : "", bl ? bl->d.blend.enable[0] : 0, bl ? bl->d.blend.src : 0, bl ? bl->d.blend.dst : 0,
                bl ? bl->d.blend.src_alpha : 0, bl ? bl->d.blend.dst_alpha : 0, bl ? bl->d.blend.mask[0] : 15, d->st.topology, srvs);
        {
            const r10_state_object *ds = d->st.depth, *rs = d->st.raster;
            R10_LOG("TRACE   depth %u write %u func %u stencil %u ref %u; cull %u ccw %u clip %u scissor %u", ds ? ds->d.depth.depth_enable : 1,
                    ds ? ds->d.depth.depth_write_mask : 1, ds ? ds->d.depth.depth_func : 2, ds ? ds->d.depth.stencil_enable : 0, d->st.stencil_ref,
                    rs ? rs->d.raster.cull : 3, rs ? rs->d.raster.front_ccw : 0, rs ? rs->d.raster.depth_clip : 1, rs ? rs->d.raster.scissor : 0);
        }
        for (int stage = 0; stage < 2; ++stage) {
            r10_resource *cb = stage ? d->st.vs.cb[0] : d->st.ps.cb[0];
            if (!cb) continue;
            const uint8_t *data = r10_buffer_shadow(cb);
            float m[3][4] = { { 0 } };
            const uint32_t at[3] = { 0, 5 * 16, 11 * 16 };
            for (int k = 0; k < 3; ++k) if (data && at[k] + 16 <= cb->desc.width) memcpy(m[k], data + at[k], 16);
            R10_LOG("TRACE   %s cb0: %u bytes usage %u v%llu shadow %d m0 %g %g %g %g m5 %g %g %g %g m11 %g %g %g %g", stage ? "vs" : "ps",
                    cb->desc.width, cb->desc.usage, (unsigned long long)cb->version, data != NULL, m[0][0], m[0][1], m[0][2], m[0][3],
                    m[1][0], m[1][1], m[1][2], m[1][3], m[2][0], m[2][1], m[2][2], m[2][3]);
        }
        const r10_layout *l = d->st.layout;
        for (uint32_t i = 0; l && i < l->count; ++i) {
            const r10_element *e = &l->el[i];
            const r10_resource *vb = d->st.vb[e->slot].buf;
            uint32_t stride = d->st.vb[e->slot].stride, at = d->st.vb[e->slot].offset + dr.vertex_lo * stride + e->offset;
            const uint8_t *shadow = vb ? r10_buffer_shadow((r10_resource *)vb) : NULL;
            uint32_t w[4] = { 0 };
            if (shadow && at + 16 <= vb->desc.width) memcpy(w, shadow + at, 16);
            R10_LOG("TRACE   el %u: v%u %s slot %u +%u stride %u vb %ux%s v%llu d%u first %08X %08X %08X %08X", i, e->location,
                    r10_format_get(e->format) ? r10_format_get(e->format)->name : "?", e->slot, e->offset, stride,
                    vb ? vb->desc.width : 0, vb && vb->desc.usage == D3D10_USAGE_DYNAMIC ? "dyn" : "", vb ? (unsigned long long)vb->version : 0ull,
                    vb ? vb->discards : 0, w[0], w[1], w[2], w[3]);
        }
    }
    d->be->draw(d, &dr);
}

COM_METHOD(dev, DrawIndexed, 4) { draw(dev(), 1, ARG(1), 1, ARG(2), (int32_t)ARG(3), 0); return 0; }
COM_METHOD(dev, Draw, 3) { draw(dev(), 0, ARG(1), 1, ARG(2), 0, 0); return 0; }
COM_METHOD(dev, DrawIndexedInstanced, 6) { draw(dev(), 1, ARG(1), ARG(2), ARG(3), (int32_t)ARG(4), ARG(5)); return 0; }
COM_METHOD(dev, DrawInstanced, 5) { draw(dev(), 0, ARG(1), ARG(2), ARG(3), 0, ARG(4)); return 0; }
COM_METHOD(dev, DrawAuto, 1) { (void)dev(); return 0; }

/* ------------------------------------------------------------------ output merger and rasterizer */
COM_METHOD(dev, OMSetRenderTargets, 4)
{
    r10_device *d = dev();
    uint32_t num = ARG(1) > R10_RT_SLOTS ? R10_RT_SLOTS : ARG(1), array = ARG(2);
    r10_view *next[R10_RT_SLOTS] = { 0 };
    for (uint32_t i = 0; i < num; ++i) {
        uint32_t g = array ? MEM32(array + 4 * i) : 0;
        next[i] = g ? (r10_view *)r10_object_from(g, R10_KIND_RTV) : NULL;
    }
    r10_view *dsv = ARG(3) ? (r10_view *)r10_object_from(ARG(3), R10_KIND_DSV) : NULL;
    int changed = dsv != d->st.dsv || num != d->st.rtv_count;
    for (uint32_t i = 0; i < R10_RT_SLOTS && !changed; ++i) changed = next[i] != d->st.rtv[i];
    if (!changed) return 0;
    for (uint32_t i = 0; i < R10_RT_SLOTS; ++i) SET_REF(d->st.rtv[i], next[i]);
    SET_REF(d->st.dsv, dsv);
    d->st.rtv_count = num;
    while (d->st.rtv_count && !d->st.rtv[d->st.rtv_count - 1]) --d->st.rtv_count;
    d->st.dirty |= R10_DIRTY_TARGETS;
    unbind_srvs_of_targets(d);
    return 0;
}
COM_METHOD(dev, OMSetBlendState, 4)
{
    r10_device *d = dev();
    r10_state_object *s = ARG(1) ? (r10_state_object *)r10_object_from(ARG(1), R10_KIND_BLEND) : NULL;
    if (s != d->st.blend) { SET_REF(d->st.blend, s); d->st.dirty |= R10_DIRTY_BLEND; }
    float f[4] = { 1, 1, 1, 1 };
    if (ARG(2)) memcpy(f, GPTR(ARG(2)), 16);
    if (memcmp(f, d->st.blend_factor, 16)) { memcpy(d->st.blend_factor, f, 16); d->st.dirty |= R10_DIRTY_BLEND_FACTOR; }
    if (d->st.sample_mask != ARG(3)) { d->st.sample_mask = ARG(3); d->st.dirty |= R10_DIRTY_BLEND; }
    return 0;
}
COM_METHOD(dev, OMSetDepthStencilState, 3)
{
    r10_device *d = dev();
    r10_state_object *s = ARG(1) ? (r10_state_object *)r10_object_from(ARG(1), R10_KIND_DEPTH) : NULL;
    if (s != d->st.depth) { SET_REF(d->st.depth, s); d->st.dirty |= R10_DIRTY_DEPTH; }
    if (d->st.stencil_ref != ARG(2)) { d->st.stencil_ref = ARG(2); d->st.dirty |= R10_DIRTY_STENCIL_REF; }
    return 0;
}
COM_METHOD(dev, OMGetRenderTargets, 4)
{
    r10_device *d = dev();
    for (uint32_t i = 0; i < ARG(1) && ARG(2); ++i) {
        r10_object *o = i < R10_RT_SLOTS ? (r10_object *)d->st.rtv[i] : NULL;
        MEM32(ARG(2) + 4 * i) = o ? o->com.guest : 0;
        hold(o);
    }
    get_one((r10_object *)d->st.dsv, ARG(3));
    return 0;
}
COM_METHOD(dev, OMGetBlendState, 4)
{
    r10_device *d = dev();
    get_one((r10_object *)d->st.blend, ARG(1));
    if (ARG(2)) memcpy(GPTR(ARG(2)), d->st.blend_factor, 16);
    if (ARG(3)) MEM32(ARG(3)) = d->st.sample_mask;
    return 0;
}
COM_METHOD(dev, OMGetDepthStencilState, 3)
{
    r10_device *d = dev();
    get_one((r10_object *)d->st.depth, ARG(1));
    if (ARG(2)) MEM32(ARG(2)) = d->st.stencil_ref;
    return 0;
}
COM_METHOD(dev, RSSetState, 2)
{
    r10_device *d = dev();
    r10_state_object *s = ARG(1) ? (r10_state_object *)r10_object_from(ARG(1), R10_KIND_RASTER) : NULL;
    if (s != d->st.raster) { SET_REF(d->st.raster, s); d->st.dirty |= R10_DIRTY_RASTER; }
    return 0;
}
COM_METHOD(dev, RSSetViewports, 3)
{
    r10_device *d = dev();
    uint32_t n = ARG(1) > 16 ? 16 : ARG(1);
    if (n && ARG(2)) {
        r10_viewport v[16];
        memcpy(v, GPTR(ARG(2)), n * sizeof(r10_viewport));
        if (n != d->st.viewports || memcmp(v, d->st.viewport, n * sizeof(r10_viewport))) {
            memcpy(d->st.viewport, v, n * sizeof(r10_viewport));
            d->st.dirty |= R10_DIRTY_VIEWPORT;
        }
    }
    d->st.viewports = n;
    return 0;
}
COM_METHOD(dev, RSSetScissorRects, 3)
{
    r10_device *d = dev();
    uint32_t n = ARG(1) > 16 ? 16 : ARG(1);
    if (n && ARG(2)) {
        r10_rect r[16];
        memcpy(r, GPTR(ARG(2)), n * sizeof(r10_rect));
        if (n != d->st.scissors || memcmp(r, d->st.scissor, n * sizeof(r10_rect))) {
            memcpy(d->st.scissor, r, n * sizeof(r10_rect));
            d->st.dirty |= R10_DIRTY_SCISSOR;
        }
    }
    d->st.scissors = n;
    return 0;
}
COM_METHOD(dev, RSGetState, 2) { get_one((r10_object *)dev()->st.raster, ARG(1)); return 0; }
COM_METHOD(dev, RSGetViewports, 3)
{
    r10_device *d = dev();
    if (!ARG(1)) return 0;
    if (ARG(2)) {
        uint32_t n = MEM32(ARG(1)) < d->st.viewports ? MEM32(ARG(1)) : d->st.viewports;
        memcpy(GPTR(ARG(2)), d->st.viewport, n * sizeof(r10_viewport));
    }
    MEM32(ARG(1)) = d->st.viewports;
    return 0;
}
COM_METHOD(dev, RSGetScissorRects, 3)
{
    r10_device *d = dev();
    if (!ARG(1)) return 0;
    if (ARG(2)) {
        uint32_t n = MEM32(ARG(1)) < d->st.scissors ? MEM32(ARG(1)) : d->st.scissors;
        memcpy(GPTR(ARG(2)), d->st.scissor, n * sizeof(r10_rect));
    }
    MEM32(ARG(1)) = d->st.scissors;
    return 0;
}

/* ------------------------------------------------------------------ predication, stream output */
COM_METHOD(dev, SetPredication, 3)
{
    (void)dev();
    static unsigned logged;
    if (ARG(1) && logged++ < 4) R10_LOG("SetPredication ignored: draws are never skipped");
    return 0;
}
COM_METHOD(dev, GetPredication, 3) { (void)dev(); if (ARG(1)) MEM32(ARG(1)) = 0; if (ARG(2)) MEM32(ARG(2)) = 0; return 0; }
COM_METHOD(dev, SOSetTargets, 4) { (void)dev(); return 0; }
COM_METHOD(dev, SOGetTargets, 4) { (void)dev(); for (uint32_t i = 0; i < ARG(1) && ARG(2); ++i) MEM32(ARG(2) + 4 * i) = 0; return 0; }

/* ------------------------------------------------------------------ copies, clears */
COM_METHOD(dev, CopySubresourceRegion, 9)
{
    r10_device *d = dev();
    r10_resource *dst = r10_resource_from(ARG(1)), *src = r10_resource_from(ARG(6));
    if (!dst || !src) return 0;
    uint32_t box[6], *pbox = NULL;
    if (ARG(8)) { memcpy(box, GPTR(ARG(8)), 24); pbox = box; }
    uint32_t area[6];
    r10_texture_copy(d, dst, ARG(2), ARG(3), ARG(4), ARG(5), src, ARG(7), r10_area_copy_box(dst, ARG(2), ARG(3), ARG(4), ARG(5), src, ARG(7), pbox, area));
    return 0;
}
COM_METHOD(dev, CopyResource, 3)
{
    r10_device *d = dev();
    r10_resource *dst = r10_resource_from(ARG(1)), *src = r10_resource_from(ARG(2));
    if (!dst || !src) return 0;
    uint32_t subs = dst->subresources < src->subresources ? dst->subresources : src->subresources;
    uint32_t area[6];
    for (uint32_t s = 0; s < subs; ++s) r10_texture_copy(d, dst, s, 0, 0, 0, src, s, r10_area_copy_box(dst, s, 0, 0, 0, src, s, NULL, area));
    return 0;
}
COM_METHOD(dev, UpdateSubresource, 7)
{
    r10_device *d = dev();
    r10_resource *r = r10_resource_from(ARG(1));
    uint32_t sub = ARG(2), box = ARG(3), data = ARG(4), row = ARG(5), slice = ARG(6);
    if (!r || !data || sub >= r->subresources) return 0;
    ++d->stats.updates;
    if (r->desc.dim == R10_DIM_BUFFER) {
        uint32_t x = box ? MEM32(box) : 0, w = box ? MEM32(box + 12) - MEM32(box) : r->desc.width;
        if (x > r->desc.width || w > r->desc.width - x) return 0;
        if (r->host_copy) memcpy(r->host_copy + x, GPTR(data), w);
        if (r->shadow) memcpy(GPTR(r->shadow + x), GPTR(data), w);
        ++r->version;
        d->be->resource_update(d, r, 0, x, 0, 0, w, 1, 1, GPTR(data), w, w);
        return 0;
    }
    uint32_t w, h, depth;
    r10_subresource_size(r, sub, &w, &h, &depth);
    uint32_t x = 0, y = 0, z = 0;
    if (box) {
        x = MEM32(box); y = MEM32(box + 4); z = MEM32(box + 8);
        uint32_t r_ = MEM32(box + 12), b = MEM32(box + 16), k = MEM32(box + 20);
        if (r_ <= x || b <= y || k <= z || r_ > w || b > h || k > depth) return 0;
        w = r_ - x; h = b - y; depth = k - z;
    }
    ++r->version;
    static int sources = -1;
    if (sources < 0) { const char *e = getenv("BIOSHOCK_R10_DEBUG"); sources = e && (atoi(e) & 16777216) ? 1 : 0; }
    if (sources)
        r10_census_texture_source(data, depth > 1 ? slice * depth : row * r10_format_rows(r->fmt, h));
    r10_texture_write(d, r, sub, x, y, z, w, h, depth, GPTR(data), row, slice);
    return 0;
}
COM_METHOD(dev, ClearRenderTargetView, 3)
{
    r10_device *d = dev();
    r10_view *v = ARG(1) ? (r10_view *)r10_object_from(ARG(1), R10_KIND_RTV) : NULL;
    if (!v || !ARG(2)) return 0;
    float c[4];
    memcpy(c, GPTR(ARG(2)), 16);
    d->be->clear_rtv(d, v, c);
    return 0;
}
COM_METHOD(dev, ClearDepthStencilView, 5)
{
    r10_device *d = dev();
    r10_view *v = ARG(1) ? (r10_view *)r10_object_from(ARG(1), R10_KIND_DSV) : NULL;
    if (!v) return 0;
    d->be->clear_dsv(d, v, ARG(2), r10_f32(ARG(3)), (uint8_t)ARG(4));
    return 0;
}
COM_METHOD(dev, GenerateMips, 2)
{
    r10_device *d = dev();
    r10_view *v = ARG(1) ? (r10_view *)r10_object_from(ARG(1), R10_KIND_SRV) : NULL;
    if (v) d->be->generate_mips(d, v);
    return 0;
}
COM_METHOD(dev, ResolveSubresource, 6)
{
    r10_device *d = dev();
    r10_resource *dst = r10_resource_from(ARG(1)), *src = r10_resource_from(ARG(3));
    if (dst && src) d->be->resolve(d, dst, ARG(2), src, ARG(4));
    return 0;
}

/* ------------------------------------------------------------------ misc */
COM_METHOD(dev, GetDeviceRemovedReason, 1) { (void)dev(); return 0; }
COM_METHOD(dev, SetExceptionMode, 2) { dev()->exception_mode = ARG(1); return 0; }
COM_METHOD(dev, GetExceptionMode, 1) { return dev()->exception_mode; }
extern uint32_t r10_private_get(r10_private **head, uint32_t guid, uint32_t size_va, uint32_t data);
extern uint32_t r10_private_set(r10_private **head, uint32_t guid, uint32_t size, uint32_t data, int interface);
COM_METHOD(dev, GetPrivateData, 4) { return r10_private_get(&dev()->priv, ARG(1), ARG(2), ARG(3)); }
COM_METHOD(dev, SetPrivateData, 4) { return r10_private_set(&dev()->priv, ARG(1), ARG(2), ARG(3), 0); }
COM_METHOD(dev, SetPrivateDataInterface, 3) { return r10_private_set(&dev()->priv, ARG(1), 0, ARG(2), 1); }
COM_METHOD(dev, ClearState, 1)
{
    r10_device *d = dev();
    r10_state *s = &d->st;
    SET_REF(s->layout, NULL);
    for (int i = 0; i < R10_VB_SLOTS; ++i) { SET_REF(s->vb[i].buf, NULL); s->vb[i].stride = s->vb[i].offset = 0; }
    SET_REF(s->ib, NULL);
    s->ib_format = s->ib_offset = 0;
    s->topology = 0;
    r10_stage_state *stages[3] = { &s->vs, &s->ps, &s->gs };
    for (int k = 0; k < 3; ++k) {
        SET_REF(stages[k]->shader, NULL);
        for (int i = 0; i < R10_CB_SLOTS; ++i) SET_REF(stages[k]->cb[i], NULL);
        for (int i = 0; i < R10_SRV_SLOTS; ++i) SET_REF(stages[k]->srv[i], NULL);
        for (int i = 0; i < R10_SAMPLER_SLOTS; ++i) SET_REF(stages[k]->sampler[i], NULL);
        stages[k]->srv_max = 0;
    }
    SET_REF(s->raster, NULL); SET_REF(s->blend, NULL); SET_REF(s->depth, NULL);
    for (int i = 0; i < 4; ++i) s->blend_factor[i] = 1.0f;
    s->sample_mask = 0xFFFFFFFFu;
    s->stencil_ref = 0;
    s->viewports = s->scissors = 0;
    for (int i = 0; i < R10_RT_SLOTS; ++i) SET_REF(s->rtv[i], NULL);
    SET_REF(s->dsv, NULL);
    s->rtv_count = 0;
    s->dirty = R10_DIRTY_ALL;
    return 0;
}
COM_METHOD(dev, Flush, 1) { r10_device *d = dev(); d->be->flush(d); return 0; }

COM_METHOD(dev, CreateBuffer, 4) { return r10_create_buffer(dev(), ARG(1), ARG(2), ARG(3) ? (uint32_t *)GPTR(ARG(3)) : NULL); }
COM_METHOD(dev, CreateTexture1D, 4) { return r10_create_texture(dev(), R10_DIM_TEXTURE1D, ARG(1), ARG(2), ARG(3) ? (uint32_t *)GPTR(ARG(3)) : NULL); }
COM_METHOD(dev, CreateTexture2D, 4) { return r10_create_texture(dev(), R10_DIM_TEXTURE2D, ARG(1), ARG(2), ARG(3) ? (uint32_t *)GPTR(ARG(3)) : NULL); }
COM_METHOD(dev, CreateTexture3D, 4) { return r10_create_texture(dev(), R10_DIM_TEXTURE3D, ARG(1), ARG(2), ARG(3) ? (uint32_t *)GPTR(ARG(3)) : NULL); }
COM_METHOD(dev, CreateShaderResourceView, 4) { return r10_create_view(dev(), R10_KIND_SRV, ARG(1), ARG(2), ARG(3) ? (uint32_t *)GPTR(ARG(3)) : NULL); }
COM_METHOD(dev, CreateRenderTargetView, 4) { return r10_create_view(dev(), R10_KIND_RTV, ARG(1), ARG(2), ARG(3) ? (uint32_t *)GPTR(ARG(3)) : NULL); }
COM_METHOD(dev, CreateDepthStencilView, 4) { return r10_create_view(dev(), R10_KIND_DSV, ARG(1), ARG(2), ARG(3) ? (uint32_t *)GPTR(ARG(3)) : NULL); }
COM_METHOD(dev, CreateInputLayout, 6) { return r10_create_layout(dev(), ARG(1), ARG(2), ARG(3), ARG(4), ARG(5) ? (uint32_t *)GPTR(ARG(5)) : NULL); }
COM_METHOD(dev, CreateVertexShader, 4) { return r10_create_shader(dev(), 1, ARG(1), ARG(2), ARG(3) ? (uint32_t *)GPTR(ARG(3)) : NULL); }
COM_METHOD(dev, CreateGeometryShader, 4) { return r10_create_shader(dev(), 2, ARG(1), ARG(2), ARG(3) ? (uint32_t *)GPTR(ARG(3)) : NULL); }
COM_METHOD(dev, CreateGeometryShaderWithStreamOutput, 7) { return r10_create_shader(dev(), 2, ARG(1), ARG(2), ARG(6) ? (uint32_t *)GPTR(ARG(6)) : NULL); }
COM_METHOD(dev, CreatePixelShader, 4) { return r10_create_shader(dev(), 0, ARG(1), ARG(2), ARG(3) ? (uint32_t *)GPTR(ARG(3)) : NULL); }
COM_METHOD(dev, CreateBlendState, 3) { return r10_create_state(dev(), R10_KIND_BLEND, ARG(1), ARG(2) ? (uint32_t *)GPTR(ARG(2)) : NULL); }
COM_METHOD(dev, CreateDepthStencilState, 3) { return r10_create_state(dev(), R10_KIND_DEPTH, ARG(1), ARG(2) ? (uint32_t *)GPTR(ARG(2)) : NULL); }
COM_METHOD(dev, CreateRasterizerState, 3) { return r10_create_state(dev(), R10_KIND_RASTER, ARG(1), ARG(2) ? (uint32_t *)GPTR(ARG(2)) : NULL); }
COM_METHOD(dev, CreateSamplerState, 3) { return r10_create_state(dev(), R10_KIND_SAMPLER, ARG(1), ARG(2) ? (uint32_t *)GPTR(ARG(2)) : NULL); }
COM_METHOD(dev, CreateQuery, 3) { return r10_create_query(dev(), ARG(1), ARG(2) ? (uint32_t *)GPTR(ARG(2)) : NULL); }
COM_METHOD(dev, CreatePredicate, 3) { return r10_create_query(dev(), ARG(1), ARG(2) ? (uint32_t *)GPTR(ARG(2)) : NULL); }
COM_METHOD(dev, CreateCounter, 3) { (void)dev(); if (ARG(2)) MEM32(ARG(2)) = 0; return DXGI_ERROR_UNSUPPORTED_; }

/* D3D10_FORMAT_SUPPORT bits the engine asks about. */
COM_METHOD(dev, CheckFormatSupport, 3)
{
    (void)dev();
    const r10_format *f = r10_format_get(ARG(1));
    if (!ARG(2)) return E_INVALIDARG_;
    if (!f) { MEM32(ARG(2)) = 0; return E_FAIL_; }
    uint32_t bits = 0x1 | 0x2 | 0x20 | 0x40 | 0x80 | 0x100 | 0x200 | 0x400 | 0x800;   /* buffer, IA, tex1D/2D/3D/cube, ld, sample */
    if (f->kind == R10_FMT_DEPTH) bits = 0x20 | 0x40 | 0x10000 | 0x200 | 0x2000;   /* depth stencil + comparison sampling */
    else if (f->kind != R10_FMT_BC && !f->is_int) bits |= 0x4000 | 0x8000 | 0x20000 | 0x40000 | 0x80000;   /* RT, blend, mips, resolve */
    else if (f->is_int) bits |= 0x4000;
    MEM32(ARG(2)) = bits;
    return 0;
}
/* Multisampling is not offered: the engine then keeps one sample everywhere. */
COM_METHOD(dev, CheckMultisampleQualityLevels, 4)
{
    (void)dev();
    if (ARG(3)) MEM32(ARG(3)) = ARG(2) == 1 ? 1 : 0;
    return 0;
}
COM_METHOD(dev, CheckCounterInfo, 2) { (void)dev(); if (ARG(1)) memset(GPTR(ARG(1)), 0, 12); return 0; }
COM_METHOD(dev, CheckCounter, 10) { (void)dev(); return E_INVALIDARG_; }
COM_METHOD(dev, GetCreationFlags, 1) { (void)dev(); return 0; }
COM_METHOD(dev, OpenSharedResource, 4) { (void)dev(); if (ARG(3)) MEM32(ARG(3)) = 0; return E_NOTIMPL_; }
COM_METHOD(dev, SetTextFilterSize, 3) { (void)dev(); return 0; }
COM_METHOD(dev, GetTextFilterSize, 3) { (void)dev(); if (ARG(1)) MEM32(ARG(1)) = 1; if (ARG(2)) MEM32(ARG(2)) = 1; return 0; }

static const void_fn device_methods[] = {
    dev_QueryInterface, dev_AddRef, dev_Release,
    dev_VSSetConstantBuffers, dev_PSSetShaderResources, dev_PSSetShader, dev_PSSetSamplers, dev_VSSetShader, dev_DrawIndexed,
    dev_Draw, dev_PSSetConstantBuffers, dev_IASetInputLayout, dev_IASetVertexBuffers, dev_IASetIndexBuffer,
    dev_DrawIndexedInstanced, dev_DrawInstanced, dev_GSSetConstantBuffers, dev_GSSetShader, dev_IASetPrimitiveTopology,
    dev_VSSetShaderResources, dev_VSSetSamplers, dev_SetPredication, dev_GSSetShaderResources, dev_GSSetSamplers,
    dev_OMSetRenderTargets, dev_OMSetBlendState, dev_OMSetDepthStencilState, dev_SOSetTargets, dev_DrawAuto, dev_RSSetState,
    dev_RSSetViewports, dev_RSSetScissorRects, dev_CopySubresourceRegion, dev_CopyResource, dev_UpdateSubresource,
    dev_ClearRenderTargetView, dev_ClearDepthStencilView, dev_GenerateMips, dev_ResolveSubresource,
    dev_VSGetConstantBuffers, dev_PSGetShaderResources, dev_PSGetShader, dev_PSGetSamplers, dev_VSGetShader,
    dev_PSGetConstantBuffers, dev_IAGetInputLayout, dev_IAGetVertexBuffers, dev_IAGetIndexBuffer, dev_GSGetConstantBuffers,
    dev_GSGetShader, dev_IAGetPrimitiveTopology, dev_VSGetShaderResources, dev_VSGetSamplers, dev_GetPredication,
    dev_GSGetShaderResources, dev_GSGetSamplers, dev_OMGetRenderTargets, dev_OMGetBlendState, dev_OMGetDepthStencilState,
    dev_SOGetTargets, dev_RSGetState, dev_RSGetViewports, dev_RSGetScissorRects, dev_GetDeviceRemovedReason,
    dev_SetExceptionMode, dev_GetExceptionMode, dev_GetPrivateData, dev_SetPrivateData, dev_SetPrivateDataInterface,
    dev_ClearState, dev_Flush, dev_CreateBuffer, dev_CreateTexture1D, dev_CreateTexture2D, dev_CreateTexture3D,
    dev_CreateShaderResourceView, dev_CreateRenderTargetView, dev_CreateDepthStencilView, dev_CreateInputLayout,
    dev_CreateVertexShader, dev_CreateGeometryShader, dev_CreateGeometryShaderWithStreamOutput, dev_CreatePixelShader,
    dev_CreateBlendState, dev_CreateDepthStencilState, dev_CreateRasterizerState, dev_CreateSamplerState, dev_CreateQuery,
    dev_CreatePredicate, dev_CreateCounter, dev_CheckFormatSupport, dev_CheckMultisampleQualityLevels, dev_CheckCounterInfo,
    dev_CheckCounter, dev_GetCreationFlags, dev_OpenSharedResource, dev_SetTextFilterSize, dev_GetTextFilterSize,
};
_Static_assert(sizeof device_methods / sizeof device_methods[0] == 98, "ID3D10Device has 98 slots");
static void_com_class device_class = { "ID3D10Device", 98, device_methods, 0 };

/* ------------------------------------------------------------------ IDXGIDevice facet */
typedef struct r10_dxgi_device { void_com_object com; } r10_dxgi_device;
COM_METHOD(dxdev, QueryInterface, 3)
{
    (void)void_com_host(THIS_);
    if (!ARG(2)) return E_POINTER_;
    if (void_guid_equal(ARG(1), IID_ID3D10Device_) && r10_dev) {
        void_com_addref(&r10_dev->obj.com);
        MEM32(ARG(2)) = r10_dev->obj.com.guest;
        return 0;
    }
    void_com_addref((void_com_object *)void_com_host(THIS_));
    MEM32(ARG(2)) = THIS_;
    return 0;
}
COM_METHOD(dxdev, AddRef, 1) { return void_com_addref((void_com_object *)void_com_host(THIS_)); }
COM_METHOD(dxdev, Release, 1)
{
    void_com_object *o = (void_com_object *)void_com_host(THIS_);
    return o->refs > 1 ? (uint32_t)--o->refs : 1;
}
COM_METHOD(dxdev, SetPrivateData, 4) { (void)void_com_host(THIS_); return 0; }
COM_METHOD(dxdev, SetPrivateDataInterface, 3) { (void)void_com_host(THIS_); return 0; }
COM_METHOD(dxdev, GetPrivateData, 4) { (void)void_com_host(THIS_); return DXGI_ERROR_NOT_FOUND_; }
COM_METHOD(dxdev, GetParent, 3)
{
    (void)void_com_host(THIS_);
    if (!ARG(2) || !r10_dev || !r10_dev->adapter) return E_NOINTERFACE_;
    r10_object_addref_guest(r10_dev->adapter);
    MEM32(ARG(2)) = r10_dev->adapter;
    return 0;
}
COM_METHOD(dxdev, GetAdapter, 2)
{
    (void)void_com_host(THIS_);
    if (!ARG(1) || !r10_dev || !r10_dev->adapter) return E_FAIL_;
    r10_object_addref_guest(r10_dev->adapter);
    MEM32(ARG(1)) = r10_dev->adapter;
    return 0;
}
COM_METHOD(dxdev, CreateSurface, 6) { (void)void_com_host(THIS_); return E_NOTIMPL_; }
COM_METHOD(dxdev, QueryResourceResidency, 4) { (void)void_com_host(THIS_); return 0; }
COM_METHOD(dxdev, SetGPUThreadPriority, 2) { (void)void_com_host(THIS_); return 0; }
COM_METHOD(dxdev, GetGPUThreadPriority, 2) { (void)void_com_host(THIS_); if (ARG(1)) MEM32(ARG(1)) = 0; return 0; }
static const void_fn dxdev_methods[] = { dxdev_QueryInterface, dxdev_AddRef, dxdev_Release, dxdev_SetPrivateData,
    dxdev_SetPrivateDataInterface, dxdev_GetPrivateData, dxdev_GetParent, dxdev_GetAdapter, dxdev_CreateSurface,
    dxdev_QueryResourceResidency, dxdev_SetGPUThreadPriority, dxdev_GetGPUThreadPriority };
static void_com_class dxdev_class = { "IDXGIDevice", 12, dxdev_methods, 0 };

/* ------------------------------------------------------------------ creation */
/* The launcher's backend, the only one of this start: one that cannot start stops the start (the launcher's
 * renderer failure policy, r10_failure.c), the other backend never takes its place. */
static const r10_backend *choose_backend(void)
{
    const char *mode = r10_backend_name();
    if (!strncmp(mode, "vulkan", 6)) return &r10_vulkan_backend;
    return &r10_gles_backend;
}

uint32_t r10_device_create(uint32_t adapter, r10_device **out)
{
    if (r10_dev) {
        void_com_addref(&r10_dev->obj.com);
        *out = r10_dev;
        return 0;
    }
    r10_renderer_failure_init();
#ifndef _WIN32
    if (void_data_dir()) {
        unsigned removed = r10_prune_shader_misses(void_data_dir(), (int64_t)time(NULL));
        if (removed) R10_LOG("pruned %u old shader-miss dumps", removed);
    }
#endif
    if (!r10_shaderlib_open()) return E_FAIL_;
    r10_device *d = (r10_device *)calloc(1, sizeof *d);
    stream_flags(d);
    d->be = choose_backend();
    r10_dev = d;
    d->st.sample_mask = 0xFFFFFFFFu;
    for (int i = 0; i < 4; ++i) d->st.blend_factor[i] = 1.0f;
    d->st.dirty = R10_DIRTY_ALL;
    if (r10_renderer_qa_start()) r10_start_error("QA debug.bioshock.renderer_fail: %s did not start", d->be->name);
    if (r10_start_error_text()[0] || !d->be->init(d)) {
        R10_LOG("RENDERER_UNAVAILABLE backend %s did not start: %s", d->be->name,
                r10_start_error_text()[0] ? r10_start_error_text() : "no reason given");
        if (!r10_start_error_text()[0]) r10_start_error("%s did not start", d->be->name);
        r10_renderer_failed(0, r10_start_error_text());
    }
    r10_object_wrap(&d->obj, &device_class, R10_KIND_DEVICE);
    r10_dxgi_device *x = (r10_dxgi_device *)calloc(1, sizeof *x);
    void_com_wrap(&x->com, &dxdev_class, 0);
    d->dxgi_device = x->com.guest;
    if (adapter) { r10_object_addref_guest(adapter); d->adapter = adapter; }
    R10_LOG("RENDERER_ACTIVE native Direct3D 10 on %s", d->be->name);
    *out = d;
    return 0;
}

/* D3D10CreateDevice(adapter, driver type, software, flags, SDK version, ppDevice) */
WINAPI_EXPORT("d3d10.dll", D3D10CreateDevice, 6)
{
    r10_device *d = NULL;
    uint32_t hr = r10_device_create(ARG(0), &d);
    if (ARG(5)) MEM32(ARG(5)) = hr ? 0 : d->obj.com.guest;
    if (!hr && !ARG(5)) void_com_release(&d->obj.com);
    return hr;
}

/* D3D10CreateDeviceAndSwapChain(adapter, driver type, software, flags, SDK, desc, ppSwapChain, ppDevice) */
WINAPI_EXPORT("d3d10.dll", D3D10CreateDeviceAndSwapChain, 8)
{
    r10_device *d = NULL;
    if (ARG(6)) MEM32(ARG(6)) = 0;
    if (ARG(7)) MEM32(ARG(7)) = 0;
    uint32_t hr = r10_device_create(ARG(0), &d);
    if (hr) return hr;
    uint32_t chain = 0;
    hr = r10_swapchain_create(d, ARG(5), &chain);
    if (hr) return hr;
    if (ARG(6)) MEM32(ARG(6)) = chain;
    if (ARG(7)) MEM32(ARG(7)) = d->obj.com.guest;
    else void_com_release(&d->obj.com);
    return 0;
}

/* Background renderer workers (pipeline and program warm-up): nice 10 and the slowest cluster (the lowest
 * cpuinfo_max_freq) when it has at least two CPUs and is not every CPU, so the game, render and audio threads
 * keep the big cores. Returns the number of CPUs in that cluster. */
int r10_background_thread_setup(void)
{
    setpriority(PRIO_PROCESS, 0, 10);
    cpu_set_t set;
    CPU_ZERO(&set);
    long cpus = sysconf(_SC_NPROCESSORS_CONF);
    unsigned long low = ~0ul, freq[64] = {0};
    for (long c = 0; c < cpus && c < 64; ++c) {
        char path[96];
        snprintf(path, sizeof path, "/sys/devices/system/cpu/cpu%ld/cpufreq/cpuinfo_max_freq", c);
        FILE *f = fopen(path, "r");
        if (f) { if (fscanf(f, "%lu", &freq[c]) != 1) freq[c] = 0; fclose(f); }
        if (freq[c] && freq[c] < low) low = freq[c];
    }
    int count = 0;
    for (long c = 0; c < cpus && c < 64; ++c)
        if (freq[c] && freq[c] == low) { CPU_SET((int)c, &set); ++count; }
    if (count >= 2 && count < cpus) sched_setaffinity(0, sizeof set, &set);
    return count;
}

/* Vulkan 1.1 backend of the native Direct3D 10 renderer.
 *
 * Core Vulkan 1.1 only, so it runs on the system drivers of 2019-2020 phones (Adreno 6xx, Vulkan
 * 1.1.128): render passes and framebuffers instead of dynamic rendering, fences instead of timeline
 * semaphores, full pipeline keys instead of extended dynamic state.  The same code drives Mesa
 * Turnip and PanVK, loaded through adrenotools (r10_vk_loader).
 *
 * - Direct3D's orientation is kept with a negative viewport height (core 1.1 or VK_KHR_maintenance1).
 * - Render passes begin lazily at the first draw; a clear before it becomes the pass's load op, a
 *   clear inside it vkCmdClearAttachments, a clear of a target that is never drawn an empty pass.
 * - Image layouts are tracked per image; a draw whose shader resource is not readable ends the pass.
 * - Constant buffers are dynamic uniform buffers in the frame's ring, so a descriptor set survives
 *   constant changes; dynamic vertex/index buffers own a ring slice of their full size per
 *   WRITE_DISCARD and draws copy the bytes they read (the same scheme as r10_gles.c).
 * - Textures created with data are uploaded by a command buffer that runs before the frame's. */
#define _GNU_SOURCE
#define VK_NO_PROTOTYPES
#include "r10.h"
#include "r10_shaderlib.h"
#include <vulkan/vulkan.h>
#include <vulkan/vulkan_android.h>
#include "replacements/vulkan_driver.h"
#include <android/native_window.h>
#include <dlfcn.h>
#include <pthread.h>
#include <stdlib.h>
#include <unistd.h>
#include "r10_vk_shaders.h"

extern void *void_android_window_current(uint32_t *serial, int wait) __attribute__((weak));
extern void void_android_window_ack(uint32_t serial) __attribute__((weak));
extern void *ut3_dxvk_native_window(void) __attribute__((weak));
extern void void_android_gpu_strings(const char *renderer, const char *version) __attribute__((weak));
void void_native_worker_affinity_begin(void);   /* threads.c: threads created meanwhile get Android's allowed cores */
void void_native_worker_offload_begin(void);    /* ... or the big cores beside the guest core; both end with _end */
void void_native_worker_affinity_end(void);
extern int void_bioshock_srgb_textures(void) __attribute__((weak));
PFN_vkGetInstanceProcAddr r10_vk_loader(char *error, size_t size);
int r10_vk_driver_active(PFN_vkGetInstanceProcAddr get, VkInstance instance, VkPhysicalDevice gpu,
                         const VkPhysicalDeviceProperties *properties, uint32_t instance_api);

/* ------------------------------------------------------------------ function table */
#define VK_INSTANCE_FUNCS(X) \
    X(vkDestroyInstance) X(vkEnumeratePhysicalDevices) X(vkGetPhysicalDeviceProperties) X(vkGetPhysicalDeviceFeatures) \
    X(vkGetPhysicalDeviceMemoryProperties) X(vkGetPhysicalDeviceQueueFamilyProperties) X(vkGetPhysicalDeviceFormatProperties) \
    X(vkCreateDevice) X(vkGetDeviceProcAddr) X(vkEnumerateDeviceExtensionProperties) X(vkCreateAndroidSurfaceKHR) \
    X(vkDestroySurfaceKHR) X(vkGetPhysicalDeviceSurfaceSupportKHR) X(vkGetPhysicalDeviceSurfaceCapabilitiesKHR) \
    X(vkGetPhysicalDeviceSurfaceFormatsKHR) X(vkGetPhysicalDeviceSurfacePresentModesKHR) X(vkGetPhysicalDeviceProperties2)
#define VK_DEVICE_FUNCS(X) \
    X(vkDestroyDevice) X(vkGetDeviceQueue) X(vkQueueSubmit) X(vkQueueWaitIdle) X(vkDeviceWaitIdle) X(vkAllocateMemory) \
    X(vkFreeMemory) X(vkMapMemory) X(vkUnmapMemory) X(vkInvalidateMappedMemoryRanges) X(vkCmdFillBuffer) X(vkBindBufferMemory) X(vkBindImageMemory) \
    X(vkGetBufferMemoryRequirements) X(vkGetImageMemoryRequirements) X(vkCreateFence) X(vkDestroyFence) X(vkResetFences) \
    X(vkWaitForFences) X(vkGetFenceStatus) X(vkCreateSemaphore) X(vkDestroySemaphore) X(vkCreateQueryPool) \
    X(vkDestroyQueryPool) X(vkGetQueryPoolResults) X(vkCreateBuffer) X(vkDestroyBuffer) X(vkCreateImage) X(vkDestroyImage) \
    X(vkCreateImageView) X(vkDestroyImageView) X(vkCreateShaderModule) X(vkDestroyShaderModule) X(vkCreatePipelineCache) \
    X(vkDestroyPipelineCache) X(vkGetPipelineCacheData) X(vkCreateGraphicsPipelines) X(vkDestroyPipeline) \
    X(vkCreatePipelineLayout) X(vkDestroyPipelineLayout) X(vkCreateSampler) X(vkDestroySampler) \
    X(vkCreateDescriptorSetLayout) X(vkDestroyDescriptorSetLayout) X(vkCreateDescriptorPool) X(vkDestroyDescriptorPool) \
    X(vkResetDescriptorPool) X(vkAllocateDescriptorSets) X(vkUpdateDescriptorSets) X(vkCreateFramebuffer) \
    X(vkDestroyFramebuffer) X(vkCreateRenderPass) X(vkDestroyRenderPass) X(vkCreateCommandPool) X(vkDestroyCommandPool) \
    X(vkResetCommandPool) X(vkAllocateCommandBuffers) X(vkBeginCommandBuffer) X(vkEndCommandBuffer) \
    X(vkCmdBindPipeline) X(vkCmdSetViewport) X(vkCmdSetScissor) X(vkCmdSetBlendConstants) X(vkCmdSetStencilReference) \
    X(vkCmdBindDescriptorSets) X(vkCmdBindIndexBuffer) X(vkCmdBindVertexBuffers) X(vkCmdDraw) X(vkCmdDrawIndexed) \
    X(vkCmdCopyBuffer) X(vkCmdCopyImage) X(vkCmdBlitImage) X(vkCmdCopyBufferToImage) X(vkCmdCopyImageToBuffer) \
    X(vkCmdClearColorImage) X(vkCmdClearDepthStencilImage) X(vkCmdClearAttachments) X(vkCmdResolveImage) \
    X(vkCmdPipelineBarrier) X(vkCmdBeginQuery) X(vkCmdEndQuery) X(vkCmdResetQueryPool) X(vkCmdPushConstants) X(vkCmdWriteTimestamp) \
    X(vkCmdBeginRenderPass) X(vkCmdEndRenderPass) X(vkCreateSwapchainKHR) X(vkDestroySwapchainKHR) \
    X(vkGetSwapchainImagesKHR) X(vkAcquireNextImageKHR) X(vkQueuePresentKHR)
#define DECLARE(name) static PFN_##name name;
VK_INSTANCE_FUNCS(DECLARE)
VK_DEVICE_FUNCS(DECLARE)
static PFN_vkGetInstanceProcAddr vkGetInstanceProcAddr_;
static int half_shaders;   /* shaderFloat16 + shaderInt16 enabled */
static PFN_vkCreateInstance vkCreateInstance_;

/* ------------------------------------------------------------------ device globals */
static VkInstance instance;
static VkPhysicalDevice gpu;
static VkDevice device;
static VkQueue queue;
static uint32_t queue_family;
static VkPhysicalDeviceProperties gpu_props;
static VkPhysicalDeviceFeatures gpu_features;
static VkPhysicalDeviceMemoryProperties mem_props;
static VkPipelineCache pipeline_cache;
static char pipeline_cache_path[1200];
static int bc_ok, d24s8_ok;

/* ------------------------------------------------------------------ memory */
typedef struct vk_range { VkDeviceSize offset, size; } vk_range;
typedef struct vk_block {
    VkDeviceMemory memory;
    VkDeviceSize size, used;
    uint32_t type;
    int linear;
    int own;                     /* one allocation in its own device memory (large ones, the frame ring) */
    uint8_t *map;
    vk_range *free; uint32_t free_count, free_cap;
    struct vk_block *next;
} vk_block;
typedef struct vk_alloc { vk_block *block; VkDeviceSize offset, size; uint8_t *map; uint8_t kind; } vk_alloc;
static vk_block *blocks;
#define BLOCK_SIZE (64ull << 20)           /* images */
#define LINEAR_BLOCK_SIZE (16ull << 20)    /* buffers: small homes, many of them come and go */

/* What each allocation holds (the memory line of the statistics, vk_memory_line). */
enum { MK_TEXTURE, MK_DECODED, MK_CONVERTED, MK_TARGET, MK_DEPTH, MK_BUFFER, MK_RING, MK_STAGING, MK_OTHER, MK_KINDS };
static const char *const mk_name[MK_KINDS] = { "textures", "decoded BC", "converted", "targets", "depth", "buffers", "ring",
                                               "staging", "other" };
static uint64_t mk_bytes[MK_KINDS];
static uint32_t mk_count[MK_KINDS];
static uint32_t blocks_released;
static uint32_t lazy_waiting;          /* large targets that were never used: no memory yet */
static uint64_t lazy_waiting_bytes;
static int debug_flags(void);

static int memory_type(uint32_t bits, VkMemoryPropertyFlags want, VkMemoryPropertyFlags avoid)
{
    for (uint32_t i = 0; i < mem_props.memoryTypeCount; ++i)
        if ((bits & (1u << i)) && (mem_props.memoryTypes[i].propertyFlags & want) == want &&
            !(mem_props.memoryTypes[i].propertyFlags & avoid)) return (int)i;
    for (uint32_t i = 0; i < mem_props.memoryTypeCount; ++i)
        if ((bits & (1u << i)) && (mem_props.memoryTypes[i].propertyFlags & want) == want) return (int)i;
    return -1;
}

static void range_insert(vk_block *b, VkDeviceSize offset, VkDeviceSize size)
{
    uint32_t i = 0;
    while (i < b->free_count && b->free[i].offset < offset) ++i;
    /* merge with the neighbours */
    int left = i > 0 && b->free[i - 1].offset + b->free[i - 1].size == offset;
    int right = i < b->free_count && offset + size == b->free[i].offset;
    if (left && right) {
        b->free[i - 1].size += size + b->free[i].size;
        memmove(&b->free[i], &b->free[i + 1], (b->free_count - i - 1) * sizeof(vk_range));
        --b->free_count;
    } else if (left) b->free[i - 1].size += size;
    else if (right) { b->free[i].offset = offset; b->free[i].size += size; }
    else {
        if (b->free_count == b->free_cap) { b->free_cap = b->free_cap ? b->free_cap * 2 : 16; b->free = (vk_range *)realloc(b->free, b->free_cap * sizeof(vk_range)); }
        memmove(&b->free[i + 1], &b->free[i], (b->free_count - i) * sizeof(vk_range));
        b->free[i].offset = offset; b->free[i].size = size;
        ++b->free_count;
    }
}

static int block_take(vk_block *b, VkDeviceSize size, VkDeviceSize align, VkDeviceSize *out)
{
    for (uint32_t i = 0; i < b->free_count; ++i) {
        VkDeviceSize start = (b->free[i].offset + align - 1) & ~(align - 1), end = b->free[i].offset + b->free[i].size;
        if (start + size > end) continue;
        VkDeviceSize before = start - b->free[i].offset, after = end - (start + size);
        VkDeviceSize original = b->free[i].offset;
        memmove(&b->free[i], &b->free[i + 1], (b->free_count - i - 1) * sizeof(vk_range));
        --b->free_count;
        if (before) range_insert(b, original, before);
        if (after) range_insert(b, start + size, after);
        b->used += size;
        *out = start;
        return 1;
    }
    return 0;
}

/* own: the allocation gets device memory of its own, returned to the driver when it is freed (the frame ring:
 * its chunks interleaved with the buffers' in shared blocks kept every block partly used). */
static int mem_alloc_in(VkMemoryRequirements req, VkMemoryPropertyFlags want, int linear, int kind, int own, vk_alloc *out)
{
    int type = memory_type(req.memoryTypeBits, want, 0);
    if (type < 0) type = memory_type(req.memoryTypeBits, want & ~(VkMemoryPropertyFlags)VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0);
    if (type < 0) return 0;
    VkDeviceSize align = req.alignment > gpu_props.limits.nonCoherentAtomSize ? req.alignment : gpu_props.limits.nonCoherentAtomSize;
    if (align < 16) align = 16;
    out->kind = (uint8_t)kind;
    VkDeviceSize block_size = linear ? LINEAR_BLOCK_SIZE : BLOCK_SIZE;
    if (req.size > block_size / 2) own = 1;
    if (!own) {
        /* The fullest block that takes it: allocations gather in few blocks, and the others empty out and go
         * back to the driver (first fit spread 40 MiB of vertex buffers over 192 MiB of blocks). */
        vk_block *candidates[64];
        unsigned n = 0;
        for (vk_block *b = blocks; b && n < 64; b = b->next)
            if (b->type == (uint32_t)type && b->linear == linear && !b->own && b->size - b->used >= req.size) candidates[n++] = b;
        for (unsigned i = 1; i < n; ++i)
            for (unsigned j = i; j > 0 && candidates[j - 1]->used < candidates[j]->used; --j) {
                vk_block *t = candidates[j]; candidates[j] = candidates[j - 1]; candidates[j - 1] = t;
            }
        for (unsigned i = 0; i < n; ++i) {
            vk_block *b = candidates[i];
            VkDeviceSize at;
            if (block_take(b, req.size, align, &at)) {
                out->block = b; out->offset = at; out->size = req.size; out->map = b->map ? b->map + at : NULL;
                mk_bytes[kind] += req.size; ++mk_count[kind];
                return 1;
            }
        }
    }
    VkDeviceSize size = own ? req.size : block_size;
    VkMemoryAllocateInfo ai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, NULL, size, (uint32_t)type };
    VkDeviceMemory memory;
    if (vkAllocateMemory(device, &ai, NULL, &memory) != VK_SUCCESS) {
        R10_LOG("Vulkan: out of device memory (%llu bytes, type %d)", (unsigned long long)size, type);
        return 0;
    }
    vk_block *b = (vk_block *)calloc(1, sizeof *b);
    b->memory = memory; b->size = size; b->type = (uint32_t)type; b->linear = linear; b->own = own;
    if (mem_props.memoryTypes[type].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)
        vkMapMemory(device, memory, 0, VK_WHOLE_SIZE, 0, (void **)&b->map);
    range_insert(b, 0, size);
    b->next = blocks;
    blocks = b;
    VkDeviceSize at;
    block_take(b, req.size, align, &at);
    out->block = b; out->offset = at; out->size = req.size; out->map = b->map ? b->map + at : NULL;
    mk_bytes[kind] += req.size; ++mk_count[kind];
    return 1;
}

static int mem_alloc(VkMemoryRequirements req, VkMemoryPropertyFlags want, int linear, int kind, vk_alloc *out)
{
    return mem_alloc_in(req, want, linear, kind, 0, out);
}

/* An empty block goes back to the driver: dedicated ones at once, a 64 MiB block when another empty block of
 * its memory type and tiling is kept already (the next allocation of that kind takes the spare).  Blocks used to
 * stay for the whole session: a level load's uploads grew the frame ring through overflow chunks, and on the
 * Mali-G615 (the test tablet, Arcadia save) 237 of 938 MiB of device memory were empty blocks during play.  Callers free
 * only memory the GPU is done with (frame fences, dead_flush, or after vkDeviceWaitIdle). */
static void mem_free(vk_alloc *a)
{
    if (!a->block) return;
    vk_block *b = a->block;
    range_insert(b, a->offset, a->size);
    b->used -= a->size;
    a->block = NULL;
    if (a->kind < MK_KINDS) { mk_bytes[a->kind] -= a->size; --mk_count[a->kind]; }
    if (b->used) return;
    if (!b->own && !(debug_flags() & 262144)) {
        int spare = 0;
        for (vk_block *o = blocks; o && !spare; o = o->next)
            spare = o != b && !o->used && !o->own && o->type == b->type && o->linear == b->linear;
        if (!spare) return;
    } else if (!b->own) {
        return;   /* BIOSHOCK_R10_DEBUG 262144: blocks stay for the session (measurements) */
    }
    for (vk_block **p = &blocks; *p; p = &(*p)->next) if (*p == b) { *p = b->next; break; }
    vkFreeMemory(device, b->memory, NULL);
    if (!b->own) ++blocks_released;
    free(b->free);
    free(b);
}

/* ------------------------------------------------------------------ deferred destruction */
typedef enum { DEAD_BUFFER, DEAD_IMAGE, DEAD_VIEW, DEAD_FRAMEBUFFER, DEAD_PIPELINE, DEAD_MEMORY, DEAD_SAMPLER, DEAD_QUERYPOOL } dead_kind;
typedef struct vk_dead { dead_kind kind; uint64_t handle; vk_alloc mem; struct vk_dead *next; } vk_dead;

/* ------------------------------------------------------------------ frames */
#define FRAMES 3
/* The frame ring's chunk size follows what the frames take: RING_MIN at first, up to RING_MAX after a frame that
 * needed more (a level load's uploads), back down after 600 frames that would fit a smaller chunk.  It was a fixed
 * 24 MiB per frame (72 MiB); play in the Arcadia save takes 5.6-6.7 MiB per frame on the Mali-G615. */
#define RING_MIN (4u << 20)
#define RING_MAX (24u << 20)
static uint32_t ring_base = RING_MIN;
static uint64_t ring_quiet_peak;
static uint32_t ring_quiet_frames;
typedef struct vk_chunk { VkBuffer buffer; vk_alloc mem; uint32_t size, used; struct vk_chunk *next; } vk_chunk;
typedef struct vk_frame {
    VkCommandPool pool;
    VkCommandBuffer cbs[18]; uint32_t cb_count, cb_next;   /* two extra for an optional depth copy */
    int depth_copy_tried;
    VkFence fences[16]; uint32_t fence_count;   /* one per submission of the frame */
    VkSemaphore acquire, done;
    VkDescriptorPool dpools[64]; uint32_t dpool_count, dpool_next;
    vk_chunk *ring;              /* the frame's ring chunk (the first) and overflow chunks */
    vk_dead *dead;
    uint64_t serial;             /* the last submission of this frame */
    int timed;                   /* GPU timestamps 2i (first command) and 2i+1 (before present) were written */
} vk_frame;
static vk_frame frames[FRAMES];
static uint32_t frame_index;
static uint64_t submit_serial = 1, completed_serial;
static VkCommandBuffer cmd, upload_cmd;   /* the frame's main and upload command buffers (recording) */
static int upload_used;
static struct {
    VkCommandBuffer prefix, copy;
    VkImage dst;
    uint32_t timer;   /* pass timing entry plus one, if traced */
} depth_copy;

static void keep_depth_copy(VkImage image)
{
    if (depth_copy.dst == image) depth_copy.dst = VK_NULL_HANDLE;
}

static void dead_push(dead_kind kind, uint64_t handle, const vk_alloc *mem)
{
    vk_dead *d = (vk_dead *)calloc(1, sizeof *d);
    d->kind = kind; d->handle = handle;
    if (mem) d->mem = *mem;
    vk_frame *f = &frames[frame_index];
    d->next = f->dead;
    f->dead = d;
}

static void dead_flush(vk_frame *f)
{
    while (f->dead) {
        vk_dead *d = f->dead;
        f->dead = d->next;
        switch (d->kind) {
        case DEAD_BUFFER: vkDestroyBuffer(device, (VkBuffer)d->handle, NULL); break;
        case DEAD_IMAGE: vkDestroyImage(device, (VkImage)d->handle, NULL); break;
        case DEAD_VIEW: vkDestroyImageView(device, (VkImageView)d->handle, NULL); break;
        case DEAD_FRAMEBUFFER: vkDestroyFramebuffer(device, (VkFramebuffer)d->handle, NULL); break;
        case DEAD_PIPELINE: vkDestroyPipeline(device, (VkPipeline)d->handle, NULL); break;
        case DEAD_SAMPLER: vkDestroySampler(device, (VkSampler)d->handle, NULL); break;
        case DEAD_QUERYPOOL: vkDestroyQueryPool(device, (VkQueryPool)d->handle, NULL); break;
        case DEAD_MEMORY: break;
        }
        mem_free(&d->mem);
        free(d);
    }
}

static VkCommandBuffer new_cb(vk_frame *f)
{
    if (f->cb_next == f->cb_count) {
        if (f->cb_count == sizeof f->cbs / sizeof f->cbs[0]) void_fatal("[r10] Vulkan: too many command buffers in one frame");
        VkCommandBufferAllocateInfo ai = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, NULL, f->pool, VK_COMMAND_BUFFER_LEVEL_PRIMARY, 1 };
        vkAllocateCommandBuffers(device, &ai, &f->cbs[f->cb_count++]);
    }
    VkCommandBuffer c = f->cbs[f->cb_next++];
    VkCommandBufferBeginInfo bi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, NULL, VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT, NULL };
    vkBeginCommandBuffer(c, &bi);
    return c;
}

/* The ring: host-visible chunks of the frame, reclaimed when its fences signal. */
static int ring_chunk(vk_frame *f, uint32_t size)
{
    vk_chunk *c = (vk_chunk *)calloc(1, sizeof *c);
    c->size = size;
    VkBufferCreateInfo bi = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, NULL, 0, size,
        VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT |
        VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, VK_SHARING_MODE_EXCLUSIVE, 0, NULL };
    if (vkCreateBuffer(device, &bi, NULL, &c->buffer) != VK_SUCCESS) { free(c); return 0; }
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(device, c->buffer, &req);
    if (!mem_alloc_in(req, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, 1, MK_RING, 1, &c->mem) || !c->mem.map) {
        vkDestroyBuffer(device, c->buffer, NULL);
        free(c);
        return 0;
    }
    vkBindBufferMemory(device, c->buffer, c->mem.block->memory, c->mem.offset);
    c->next = f->ring;
    f->ring = c;
    return 1;
}

typedef struct vk_slice { VkBuffer buffer; uint32_t offset; uint8_t *map; } vk_slice;
static uint64_t ring_frame_bytes, ring_peak_bytes;   /* bytes the frames take from the ring (the memory line) */

static int ring_alloc(uint32_t bytes, uint32_t align, vk_slice *out)
{
    vk_frame *f = &frames[frame_index];
    vk_chunk *c = f->ring;
    uint32_t at = c ? (c->used + align - 1) & ~(align - 1) : 0;
    if (!c || at + bytes > c->size) {
        uint32_t size = bytes > ring_base / 2 ? (bytes + 0xFFFFu) & ~0xFFFFu : ring_base;
        if (!ring_chunk(f, size)) return 0;
        c = f->ring;
        at = 0;
        static unsigned grown;
        if (c->next && grown++ < 8) R10_LOG("Vulkan: frame ring grows by %u KiB", size >> 10);
    }
    c->used = at + bytes;
    ring_frame_bytes += bytes;
    out->buffer = c->buffer;
    out->offset = at;
    out->map = c->mem.map + at;
    return 1;
}

static void frame_reset_ring(vk_frame *f)
{
    uint64_t used = 0;
    for (vk_chunk *c = f->ring; c; c = c->next) used += c->used;
    if (used > ring_base) {
        /* A frame that did not fit: larger chunks (a quarter of headroom), and the quiet window starts again. */
        if (ring_base < RING_MAX) {
            uint64_t want = (used + (used >> 2) + 0xFFFFFu) & ~(uint64_t)0xFFFFFu;
            ring_base = want > RING_MAX ? RING_MAX : (uint32_t)want;
        }
        ring_quiet_frames = 0;
        ring_quiet_peak = 0;
    } else {
        if (used > ring_quiet_peak) ring_quiet_peak = used;
        if (++ring_quiet_frames >= 600) {   /* 600 frames that fit: chunks for their peak with a quarter of headroom */
            uint64_t want = (ring_quiet_peak + (ring_quiet_peak >> 2) + 0xFFFFFu) & ~(uint64_t)0xFFFFFu;
            if (want < RING_MIN) want = RING_MIN;
            if (want < ring_base) ring_base = (uint32_t)want;
            ring_quiet_frames = 0;
            ring_quiet_peak = 0;
        }
    }
    /* Keep one chunk of the current size, free the others (their memory is their own). */
    vk_chunk *keep = NULL;
    while (f->ring) {
        vk_chunk *c = f->ring;
        f->ring = c->next;
        if (!keep && c->size == ring_base) { keep = c; keep->next = NULL; keep->used = 0; continue; }
        vkDestroyBuffer(device, c->buffer, NULL);
        mem_free(&c->mem);
        free(c);
    }
    f->ring = keep;
}

/* ------------------------------------------------------------------ backend objects */
typedef struct vk_image {
    VkImage image;
    vk_alloc mem;
    VkFormat format, unorm_format;
    VkImageAspectFlags aspect;
    VkImageLayout layout;
    uint32_t width, height, depth, mips, layers;
    int depth_kind, decoded, cube, srgb, substituted;
    int converted;               /* the converted pack's blocks (r10_pack.c): ASTC 4x4 or ETC2 RGBA8 EAC */
    uint32_t generation;         /* bumped when the storage is recreated (vk_resource_store): views follow */
    uint64_t gpu_use;
    /* A large render target or depth buffer gets its memory (and its views) at its first use (image_ready). */
    int lazy, lazy_kind;
    VkMemoryRequirements lazy_req;
    uint32_t lazy_w, lazy_h;
    const char *lazy_name;
} vk_image;
static int image_ready(vk_image *im);

typedef struct vk_buffer {
    VkBuffer buffer;             /* static home (never discarded buffers) */
    vk_alloc mem;
    uint64_t gpu_use;
    /* streaming slice of the current WRITE_DISCARD version */
    uint64_t slice_version, slice_frame;
    vk_slice slice;
    uint32_t slice_lo, slice_hi;
    /* constant buffer upload of the current version */
    uint64_t cb_version, cb_frame;
    vk_slice cb_slice;
    uint8_t *data;               /* DEFAULT constant buffers: what UpdateSubresource wrote */
    uint64_t data_version;
} vk_buffer;

typedef struct vk_view {
    VkImageView view, view_unorm;    /* view_unorm: the UNORM view of an sRGB image ("HDR off") */
    VkImageViewType type;
    uint32_t generation;             /* of the image storage the views were made for */
    int lazy_view;                   /* the image had no memory yet: the views are made at its first use */
    uint32_t width, height;
    int pending_clear;
    VkClearValue clear;
    uint32_t clear_aspects;
} vk_view;
static vk_view *view_current(r10_view *v);

typedef struct vk_shader { VkShaderModule module; VkBuffer icb; vk_alloc icb_mem; int module_failed; } vk_shader;
static uint32_t shader_modules;   /* VkShaderModules made so far (the memory line) */
typedef struct vk_sampler { VkSampler sampler; } vk_sampler;

static vk_image *img(r10_resource *r) { return (vk_image *)r->be; }
static vk_buffer *buf(r10_resource *r) { return (vk_buffer *)r->be; }

/* ------------------------------------------------------------------ barriers */
static VkAccessFlags layout_access(VkImageLayout l)
{
    switch (l) {
    case VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL: return VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    case VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL: return VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    case VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL: case VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL: return VK_ACCESS_SHADER_READ_BIT;
    case VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL: return VK_ACCESS_TRANSFER_READ_BIT;
    case VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL: return VK_ACCESS_TRANSFER_WRITE_BIT;
    case VK_IMAGE_LAYOUT_GENERAL: return VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
    default: return 0;
    }
}

static void transition(VkCommandBuffer c, vk_image *im, VkImageLayout to)
{
    keep_depth_copy(im->image);
    if (im->lazy && !image_ready(im)) return;
    if (im->layout == to) return;
    VkImageMemoryBarrier b = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, NULL, layout_access(im->layout), layout_access(to),
                               im->layout, to, VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, im->image,
                               { im->aspect, 0, VK_REMAINING_MIP_LEVELS, 0, VK_REMAINING_ARRAY_LAYERS } };
    vkCmdPipelineBarrier(c, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, NULL, 0, NULL, 1, &b);
    im->layout = to;
}

static VkImageLayout read_layout(const vk_image *im)
{
    return im->depth_kind ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
}

/* ------------------------------------------------------------------ render passes */
typedef struct pass_key { uint32_t color[8], color_count, depth; uint8_t color_load[8], depth_load, stencil_load; } pass_key;
typedef struct pass_entry { pass_key key; VkRenderPass pass; struct pass_entry *next; } pass_entry;
static pass_entry *passes;

static VkRenderPass render_pass(const pass_key *k)
{
    for (pass_entry *e = passes; e; e = e->next) if (!memcmp(&e->key, k, sizeof *k)) return e->pass;
    VkAttachmentDescription att[9];
    VkAttachmentReference refs[8], depth_ref;
    uint32_t n = 0;
    for (uint32_t i = 0; i < k->color_count; ++i) {
        refs[i].attachment = k->color[i] ? n : VK_ATTACHMENT_UNUSED;
        refs[i].layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        if (!k->color[i]) continue;
        VkAttachmentDescription a = { 0, (VkFormat)k->color[i], VK_SAMPLE_COUNT_1_BIT, (VkAttachmentLoadOp)k->color_load[i],
                                      VK_ATTACHMENT_STORE_OP_STORE, VK_ATTACHMENT_LOAD_OP_DONT_CARE, VK_ATTACHMENT_STORE_OP_DONT_CARE,
                                      VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
        att[n++] = a;
    }
    if (k->depth) {
        VkAttachmentDescription a = { 0, (VkFormat)k->depth, VK_SAMPLE_COUNT_1_BIT, (VkAttachmentLoadOp)k->depth_load,
                                      VK_ATTACHMENT_STORE_OP_STORE, (VkAttachmentLoadOp)k->stencil_load, VK_ATTACHMENT_STORE_OP_STORE,
                                      VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL };
        depth_ref.attachment = n;
        depth_ref.layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        att[n++] = a;
    }
    VkSubpassDescription sub = { 0, VK_PIPELINE_BIND_POINT_GRAPHICS, 0, NULL, k->color_count, refs, NULL, k->depth ? &depth_ref : NULL, 0, NULL };
    VkSubpassDependency deps[2] = {
        { VK_SUBPASS_EXTERNAL, 0, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT,
          VK_ACCESS_MEMORY_WRITE_BIT, VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT, 0 },
        { 0, VK_SUBPASS_EXTERNAL, VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
          VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT, VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT, 0 } };
    VkRenderPassCreateInfo ci = { VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO, NULL, 0, n, att, 1, &sub, 2, deps };
    pass_entry *e = (pass_entry *)calloc(1, sizeof *e);
    e->key = *k;
    if (vkCreateRenderPass(device, &ci, NULL, &e->pass) != VK_SUCCESS) R10_LOG("Vulkan: render pass creation failed");
    e->next = passes;
    passes = e;
    return e->pass;
}

typedef struct fb_entry { VkImageView views[9]; uint32_t count, width, height; VkRenderPass pass; VkFramebuffer fb; struct fb_entry *next; } fb_entry;
#define FB_BUCKETS 256
static fb_entry *framebuffers[FB_BUCKETS];

static VkFramebuffer framebuffer(VkRenderPass pass, const VkImageView *views, uint32_t count, uint32_t w, uint32_t h)
{
    uint64_t hash = 1469598103934665603ull;
    for (uint32_t i = 0; i < count; ++i) { hash ^= (uint64_t)views[i]; hash *= 1099511628211ull; }
    unsigned b = (unsigned)(hash % FB_BUCKETS);
    for (fb_entry *e = framebuffers[b]; e; e = e->next)
        if (e->count == count && e->width == w && e->height == h && e->pass == pass && !memcmp(e->views, views, count * sizeof *views)) return e->fb;
    fb_entry *e = (fb_entry *)calloc(1, sizeof *e);
    memcpy(e->views, views, count * sizeof *views);
    e->count = count; e->width = w; e->height = h; e->pass = pass;
    VkFramebufferCreateInfo ci = { VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO, NULL, 0, pass, count, views, w, h, 1 };
    vkCreateFramebuffer(device, &ci, NULL, &e->fb);
    e->next = framebuffers[b];
    framebuffers[b] = e;
    return e->fb;
}

static void forget_framebuffers(VkImageView view)
{
    for (unsigned b = 0; b < FB_BUCKETS; ++b)
        for (fb_entry **p = &framebuffers[b]; *p;) {
            int uses = 0;
            for (uint32_t i = 0; i < (*p)->count; ++i) if ((*p)->views[i] == view) uses = 1;
            if (uses) { fb_entry *e = *p; *p = e->next; dead_push(DEAD_FRAMEBUFFER, (uint64_t)e->fb, NULL); free(e); }
            else p = &(*p)->next;
        }
}

/* The pass being recorded. */
static struct {
    int active;
    r10_view *rtv[8]; uint32_t rtv_count; r10_view *dsv;
    VkRenderPass pass;           /* compatible pass of the formats (LOAD) for the pipelines */
    uint32_t width, height;      /* the render area (top-left); the scene area when `limited` */
    int limited;
    r10_query *occlusion;        /* an occlusion query running in this pass */
} cur;

/* The render scale draws the scene into the top-left of full-size targets (android/host/bioshock_render10.c).
 * While the host reports that scene area, passes on targets of the full size get it as their render area, so
 * a tiler bins, clears, loads and stores only that region: every pass of the Adreno 618 at 50 % covered all
 * 2400x1080 (an empty clear pass 1.8 ms).  A draw reaching outside restarts its pass on the whole target. */
static int area_full_next;
static unsigned area_restarts;

static void pending_clear_flush(r10_view *v);
static void occlusion_pass_end(void);
static void occlusion_pass_begin(void);
static void harvest(uint32_t frame);
static r10_query *active_occlusion;
#define QUERY_POOL_SIZE 2048
static VkQueryPool occlusion_pools[3];
static uint32_t occlusion_used[3];

static int debug_flags(void)
{
    static int flags = -1;
    if (flags < 0) { const char *e = getenv("BIOSHOCK_R10_DEBUG"); flags = e ? atoi(e) : 0; }
    return flags;
}

/* GPU time of a frame: timestamps around its commands, read when its fences have signalled. */
static VkQueryPool gpu_timer_pool;
static double gpu_timer_ns_per_tick;

/* Traced frames: GPU time of every render pass (timestamps before its begin and after its end). */
#define PASS_TIMERS 64
static VkQueryPool pass_timer_pool;
static struct { uint32_t color, depth, w, h, load_color, load_depth, first_draw, last_draw; const char *op; } pass_info[PASS_TIMERS];
static uint32_t pass_timer_count;
static int pass_timer_open;

static void end_pass(void)
{
    if (!cur.active) return;
    if (r10_dev && r10_dev->tracing) {
        /* who ends the pass: offsets in the library (llvm-addr2line -f -i -e libbioshock.so) */
        Dl_info a = { 0 };
        void *r0 = __builtin_return_address(0);
        dladdr(r0, &a);
        R10_LOG("PASS end after draw %u from +%lx", r10_dev->stats.draws, (unsigned long)((uintptr_t)r0 - (uintptr_t)a.dli_fbase));
    }
    occlusion_pass_end();
    vkCmdEndRenderPass(cmd);
    cur.active = 0;
    if (pass_timer_open) {
        vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, pass_timer_pool, 2 * pass_timer_count + 1);
        pass_info[pass_timer_count].last_draw = r10_dev ? r10_dev->stats.draws : 0;
        ++pass_timer_count;
        pass_timer_open = 0;
    }
}

/* Traced frames: GPU time of work outside the passes (copies, mips, the present pass). */
static int op_timer_begin(const char *op, uint32_t w, uint32_t h)
{
    if (!r10_dev || !r10_dev->tracing || !pass_timer_pool || pass_timer_count >= PASS_TIMERS) return 0;
    end_pass();
    vkCmdResetQueryPool(cmd, pass_timer_pool, 2 * pass_timer_count, 2);
    vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, pass_timer_pool, 2 * pass_timer_count);
    memset(&pass_info[pass_timer_count], 0, sizeof pass_info[0]);
    pass_info[pass_timer_count].op = op; pass_info[pass_timer_count].w = w; pass_info[pass_timer_count].h = h;
    return 1;
}
static void op_timer_end(int open)
{
    if (!open) return;
    vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, pass_timer_pool, 2 * pass_timer_count + 1);
    ++pass_timer_count;
}

static pass_key targets_key(r10_view **rtv, uint32_t count, r10_view *dsv, int with_loads)
{
    pass_key k;
    memset(&k, 0, sizeof k);
    k.color_count = count;
    for (uint32_t i = 0; i < count; ++i) {
        if (!rtv[i]) continue;
        k.color[i] = img(rtv[i]->res)->format;
        vk_view *v = (vk_view *)rtv[i]->be;
        k.color_load[i] = (uint8_t)(with_loads && v->pending_clear ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD);
    }
    if (dsv) {
        k.depth = img(dsv->res)->format;
        vk_view *v = (vk_view *)dsv->be;
        k.depth_load = (uint8_t)(with_loads && v->pending_clear && (v->clear_aspects & VK_IMAGE_ASPECT_DEPTH_BIT) ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD);
        k.stencil_load = (uint8_t)(with_loads && v->pending_clear && (v->clear_aspects & VK_IMAGE_ASPECT_STENCIL_BIT) ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD);
        if (!img(dsv->res)->depth_kind || !dsv->fmt->stencil) k.stencil_load = (uint8_t)VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    }
    return k;
}

/* Begins a pass on the views; their pending clears become load ops. */
static int begin_pass(r10_view **rtv, uint32_t count, r10_view *dsv)
{
    end_pass();
    uint32_t w = 0xFFFFFFFFu, h = 0xFFFFFFFFu;
    VkImageView views[9];
    VkClearValue clears[9];
    uint32_t n = 0;
    memset(clears, 0, sizeof clears);
    for (uint32_t i = 0; i < count; ++i) {
        if (!rtv[i]) continue;
        vk_image *im = img(rtv[i]->res);
        vk_view *v = view_current(rtv[i]);
        if (!v || !v->view) return 0;
        transition(cmd, im, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
        im->gpu_use = submit_serial;
        if (v->width < w) w = v->width;
        if (v->height < h) h = v->height;
        if (v->pending_clear) clears[n] = v->clear;
        views[n++] = v->view;
    }
    if (dsv) {
        vk_image *im = img(dsv->res);
        vk_view *v = view_current(dsv);
        if (!v || !v->view) return 0;
        transition(cmd, im, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);
        im->gpu_use = submit_serial;
        if (v->width < w) w = v->width;
        if (v->height < h) h = v->height;
        if (v->pending_clear) clears[n] = v->clear;
        views[n++] = v->view;
    }
    if (!n) return 0;
    pass_key k = targets_key(rtv, count, dsv, 1), compatible = targets_key(rtv, count, dsv, 0);
    VkRenderPass pass = render_pass(&k);
    cur.pass = render_pass(&compatible);
    VkFramebuffer fb = framebuffer(cur.pass, views, n, w, h);
    uint32_t rw = w, rh = h;
    int limited = r10_area.w && w == r10_area.full_w && h == r10_area.full_h && !area_full_next;
    area_full_next = 0;
    if (limited) { rw = r10_area.w; rh = r10_area.h; }
    VkRenderPassBeginInfo bi = { VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO, NULL, pass, fb, { { 0, 0 }, { rw, rh } }, n, clears };
    if (r10_dev && r10_dev->tracing && gpu_timer_pool) {
        if (!pass_timer_pool) {
            VkQueryPoolCreateInfo qi = { VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO, NULL, 0, VK_QUERY_TYPE_TIMESTAMP, 2 * PASS_TIMERS, 0 };
            if (vkCreateQueryPool(device, &qi, NULL, &pass_timer_pool) != VK_SUCCESS) pass_timer_pool = VK_NULL_HANDLE;
        }
        if (pass_timer_pool && pass_timer_count < PASS_TIMERS) {
            vkCmdResetQueryPool(cmd, pass_timer_pool, 2 * pass_timer_count, 2);
            vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, pass_timer_pool, 2 * pass_timer_count);
            pass_info[pass_timer_count].color = count && rtv[0] ? (uint32_t)img(rtv[0]->res)->format : 0;
            pass_info[pass_timer_count].depth = dsv ? (uint32_t)img(dsv->res)->format : 0;
            pass_info[pass_timer_count].w = rw; pass_info[pass_timer_count].h = rh;
            pass_info[pass_timer_count].load_color = k.color_load[0]; pass_info[pass_timer_count].load_depth = k.depth_load;
            pass_info[pass_timer_count].first_draw = r10_dev->stats.draws + 1;
            pass_timer_open = 1;
        }
    }
    vkCmdBeginRenderPass(cmd, &bi, VK_SUBPASS_CONTENTS_INLINE);
    for (uint32_t i = 0; i < count; ++i) if (rtv[i]) ((vk_view *)rtv[i]->be)->pending_clear = 0;
    if (dsv) ((vk_view *)dsv->be)->pending_clear = 0;
    cur.active = 1;
    memcpy(cur.rtv, rtv, count * sizeof *rtv);
    cur.rtv_count = count;
    cur.dsv = dsv;
    cur.width = rw;
    cur.height = rh;
    cur.limited = limited;
    if (r10_dev) ++r10_dev->stats.passes;
    occlusion_pass_begin();
    return 1;
}

/* A clear nobody drew into: an empty pass with the clear as its load op. */
static void pending_clear_flush(r10_view *v)
{
    vk_view *bv = (vk_view *)v->be;
    if (!bv || !bv->pending_clear) return;
    if (v->obj.kind == R10_KIND_RTV) begin_pass(&v, 1, NULL);
    else begin_pass(NULL, 0, v);
    end_pass();
}

static int is_attachment(const r10_view *v)
{
    if (!cur.active) return 0;
    for (uint32_t i = 0; i < cur.rtv_count; ++i) if (cur.rtv[i] == v) return 1;
    return cur.dsv == v;
}

static int image_in_pass(const vk_image *im)
{
    if (!cur.active) return 0;
    for (uint32_t i = 0; i < cur.rtv_count; ++i) if (cur.rtv[i] && img(cur.rtv[i]->res) == im) return 1;
    return cur.dsv && img(cur.dsv->res) == im;
}

/* Views with pending clears of one resource (before it is read, copied or destroyed). */
#define MAX_PENDING 64
static r10_view *pending[MAX_PENDING];
static uint32_t pending_count;

static void pending_add(r10_view *v)
{
    for (uint32_t i = 0; i < pending_count; ++i) if (pending[i] == v) return;
    if (pending_count == MAX_PENDING) { pending_clear_flush(pending[0]); memmove(pending, pending + 1, --pending_count * sizeof *pending); }
    pending[pending_count++] = v;
}

/* r NULL: every pending clear (the frame ends), except those of targets that never had memory: their clear waits
 * for their first use (an attachment takes it as its load op, a read or copy flushes it first). */
static void pending_flush_resource(r10_resource *r)
{
    for (uint32_t i = 0; i < pending_count;) {
        if (!r && img(pending[i]->res) && img(pending[i]->res)->lazy) { ++i; continue; }
        if (pending[i]->res == r || !r) {
            r10_view *v = pending[i];
            memmove(pending + i, pending + i + 1, (--pending_count - i) * sizeof *pending);
            pending_clear_flush(v);
        } else ++i;
    }
}

static void pending_forget(r10_view *v)
{
    for (uint32_t i = 0; i < pending_count; ++i)
        if (pending[i] == v) { memmove(pending + i, pending + i + 1, (--pending_count - i) * sizeof *pending); return; }
}

/* ------------------------------------------------------------------ submission */
/* Whether Direct3D work was recorded since the last submission: an EVENT query ended before any
 * completes with that submission (ReduceMouseLag ends one right after Present and waits for it a frame
 * later; tying it to the next submission would serialise the CPU and the GPU). */
static int work_recorded;

static void begin_frame_commands(void)
{
    vk_frame *f = &frames[frame_index];
    cmd = new_cb(f);
    work_recorded = 0;
    if (gpu_timer_pool && !f->timed && f->fence_count == 0) {
        vkCmdResetQueryPool(cmd, gpu_timer_pool, 2 * frame_index, 2);
        vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, gpu_timer_pool, 2 * frame_index);
        f->timed = 1;
    }
    upload_cmd = VK_NULL_HANDLE;
    upload_used = 0;
}

static VkCommandBuffer uploads(void)
{
    if (!upload_cmd) upload_cmd = new_cb(&frames[frame_index]);
    upload_used = 1;
    return upload_cmd;
}

static void wait_frame(vk_frame *f)
{
    if (!f->fence_count) return;
    VkResult r;
    unsigned tries = 0;
    uint64_t wait_start = r10_now_ns();
    void_machine_lend_begin();   /* the audio producer may render meanwhile (threads.c, Loans) */
    do { r = vkWaitForFences(device, f->fence_count, f->fences, VK_TRUE, 5000000000ull); }
    while ((r == VK_TIMEOUT || r == VK_ERROR_INITIALIZATION_FAILED) && ++tries < 100);
    void_machine_lend_end();
    if (r10_dev) r10_dev->stats.wait_us += (uint32_t)((r10_now_ns() - wait_start) / 1000);
    if (r != VK_SUCCESS) R10_LOG("Vulkan: frame fence wait %d", r);
    for (uint32_t i = 0; i < f->fence_count; ++i) vkDestroyFence(device, f->fences[i], NULL);
    f->fence_count = 0;
    if (f->serial > completed_serial) completed_serial = f->serial;
}

/* Submits what was recorded; `wait`/`signal` are the present semaphores of the frame's last submit. */
/* Completion marker: each submission ends with a write of its serial into a small host-visible buffer, so
 * whether the GPU finished it is a memory read.  vkGetFenceStatus of Turnip (PurpleVK) on KGSL waits for the
 * GPU (KGSL_IOCTL_DEVICE_WAITTIMESTAMP_CTXTID): 6.7 ms per frame of the game's query polls on the RMX3851,
 * the CPU and the GPU ran one after the other.  Fences stay for the waits that must block. */
static VkBuffer done_buf;
static VkDeviceMemory done_mem;
static volatile uint32_t *done_map;
static int done_coherent, done_tried;

static void done_marker_init(void)
{
    done_tried = 1;
    if (debug_flags() & 16384) { R10_LOG("Vulkan: completion marker off (fence status polling)"); return; }
    VkBufferCreateInfo bi = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, NULL, 0, 64, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                              VK_SHARING_MODE_EXCLUSIVE, 0, NULL };
    if (vkCreateBuffer(device, &bi, NULL, &done_buf) != VK_SUCCESS) { done_buf = VK_NULL_HANDLE; return; }
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(device, done_buf, &req);
    const VkMemoryPropertyFlags vis = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT, coh = VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                                cached = VK_MEMORY_PROPERTY_HOST_CACHED_BIT;
    int type = memory_type(req.memoryTypeBits, vis | coh | cached, 0);
    if (type < 0) type = memory_type(req.memoryTypeBits, vis | coh, 0);
    if (type < 0) type = memory_type(req.memoryTypeBits, vis, 0);
    VkMemoryAllocateInfo ai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, NULL, req.size, (uint32_t)(type < 0 ? 0 : type) };
    void *map = NULL;
    if (type < 0 || vkAllocateMemory(device, &ai, NULL, &done_mem) != VK_SUCCESS ||
        vkBindBufferMemory(device, done_buf, done_mem, 0) != VK_SUCCESS ||
        vkMapMemory(device, done_mem, 0, VK_WHOLE_SIZE, 0, &map) != VK_SUCCESS) {
        R10_LOG("Vulkan: no completion marker, fence status polling");
        vkDestroyBuffer(device, done_buf, NULL); done_buf = VK_NULL_HANDLE;
        if (done_mem) { vkFreeMemory(device, done_mem, NULL); done_mem = VK_NULL_HANDLE; }
        return;
    }
    done_coherent = (mem_props.memoryTypes[type].propertyFlags & coh) != 0;
    done_map = (volatile uint32_t *)map;
    *done_map = 0;
    R10_LOG("Vulkan: completion marker in memory type %d (%s)", type, done_coherent ? "coherent" : "invalidated");
}

/* The last serial the marker reports finished (0 without a marker). */
static uint64_t done_marker_read(void)
{
    if (!done_map) return 0;
    if (!done_coherent) {
        VkMappedMemoryRange range = { VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE, NULL, done_mem, 0, VK_WHOLE_SIZE };
        vkInvalidateMappedMemoryRanges(device, 1, &range);
    }
    return __atomic_load_n(done_map, __ATOMIC_ACQUIRE);
}

static void present_drain(void);

static uint32_t finish_commands(VkCommandBuffer list[4])
{
    uint32_t n = 0;
    if (upload_used) { vkEndCommandBuffer(upload_cmd); list[n++] = upload_cmd; }
    if (depth_copy.prefix) list[n++] = depth_copy.prefix;
    if (depth_copy.copy) list[n++] = depth_copy.copy;
    vkEndCommandBuffer(cmd);
    list[n++] = cmd;
    memset(&depth_copy, 0, sizeof depth_copy);
    return n;
}

static void submit(VkSemaphore wait, VkSemaphore signal)
{
    end_pass();
    present_drain();
    if (!done_tried) done_marker_init();
    if (done_buf) {
        /* after every command of the submission: its serial (f->serial below) */
        VkMemoryBarrier before = { VK_STRUCTURE_TYPE_MEMORY_BARRIER, NULL, VK_ACCESS_MEMORY_WRITE_BIT, VK_ACCESS_TRANSFER_WRITE_BIT };
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &before, 0, NULL, 0, NULL);
        vkCmdFillBuffer(cmd, done_buf, 0, 4, (uint32_t)submit_serial);
        VkMemoryBarrier after = { VK_STRUCTURE_TYPE_MEMORY_BARRIER, NULL, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_HOST_READ_BIT };
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &after, 0, NULL, 0, NULL);
    }
    vk_frame *f = &frames[frame_index];
    VkCommandBuffer list[4];
    uint32_t n = finish_commands(list);
    VkFence fence;
    VkFenceCreateInfo fi = { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, NULL, 0 };
    vkCreateFence(device, &fi, NULL, &fence);
    VkPipelineStageFlags stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    VkSubmitInfo si = { VK_STRUCTURE_TYPE_SUBMIT_INFO, NULL, wait ? 1u : 0u, wait ? &wait : NULL, &stage, n, list,
                        signal ? 1u : 0u, signal ? &signal : NULL };
    uint64_t submit_start = r10_now_ns();
    VkResult r = vkQueueSubmit(queue, 1, &si, fence);
    if (r10_dev) { ++r10_dev->stats.submits; r10_dev->stats.submit_us += (uint32_t)((r10_now_ns() - submit_start) / 1000); }
    if (r != VK_SUCCESS) R10_LOG("Vulkan: vkQueueSubmit %d", r);
    if (f->fence_count == 16) wait_frame(f);
    f->fences[f->fence_count++] = fence;
    f->serial = submit_serial++;
    if (r10_dev) r10_dev->submitted = f->serial;
}

/* A submit in the middle of a frame (a query or a readback waits for it); the frame continues in a new
 * command buffer of the same frame slot. */
static void submit_partial(void)
{
    submit(VK_NULL_HANDLE, VK_NULL_HANDLE);
    begin_frame_commands();
    if (r10_dev) r10_dev->st.dirty = R10_DIRTY_ALL;
}

static void wait_serial(uint64_t serial)
{
    if (serial <= completed_serial) return;
    for (int i = 0; i < FRAMES; ++i)
        if (frames[i].fence_count && frames[i].serial >= serial && serial > completed_serial) {
            /* wait for the frame's fences in order */
            vk_frame *f = &frames[i];
            uint64_t wait_start = r10_now_ns();
            void_machine_lend_begin();
            VkResult r = vkWaitForFences(device, f->fence_count, f->fences, VK_TRUE, 5000000000ull);
            void_machine_lend_end();
            if (r10_dev) r10_dev->stats.wait_us += (uint32_t)((r10_now_ns() - wait_start) / 1000);
            if (r == VK_SUCCESS && f->serial > completed_serial) completed_serial = f->serial;
        }
}

static void next_frame(void)
{
    if (ring_frame_bytes > ring_peak_bytes) ring_peak_bytes = ring_frame_bytes;
    ring_frame_bytes = 0;
    frame_index = (frame_index + 1) % FRAMES;
    vk_frame *f = &frames[frame_index];
    wait_frame(f);
    if (f->timed == 2 && r10_dev) {
        uint64_t t[2];
        if (vkGetQueryPoolResults(device, gpu_timer_pool, 2 * frame_index, 2, sizeof t, t, sizeof t[0], VK_QUERY_RESULT_64_BIT) == VK_SUCCESS && t[1] > t[0]) {
            r10_dev->stats.gpu_us += (uint32_t)((double)(t[1] - t[0]) * gpu_timer_ns_per_tick / 1000.0);
            ++r10_dev->stats.gpu_frames;
        }
    }
    f->timed = 0;
    harvest(frame_index);
    dead_flush(f);
    vkResetCommandPool(device, f->pool, 0);
    f->cb_next = 0;
    f->depth_copy_tried = 0;
    for (uint32_t i = 0; i < f->dpool_count; ++i) vkResetDescriptorPool(device, f->dpools[i], 0);
    f->dpool_next = 0;
    frame_reset_ring(f);
    begin_frame_commands();
    if (occlusion_pools[frame_index]) { vkCmdResetQueryPool(cmd, occlusion_pools[frame_index], 0, QUERY_POOL_SIZE); occlusion_used[frame_index] = 0; }
}

/* ------------------------------------------------------------------ dummies */
static VkImage dummy_image[4];
static vk_alloc dummy_mem[4];
static VkImageView dummy_view[8];   /* by VkImageViewType */
static VkSampler dummy_sampler;
static VkBuffer dummy_ubo;
static vk_alloc dummy_ubo_mem;

static int create_dummies(void)
{
    /* 2D (also 2D array view), cube (6 layers), 3D */
    struct { VkImageType type; uint32_t layers; VkImageCreateFlags flags; } kinds[3] = {
        { VK_IMAGE_TYPE_2D, 1, 0 }, { VK_IMAGE_TYPE_2D, 6, VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT }, { VK_IMAGE_TYPE_3D, 1, 0 } };
    VkCommandBuffer c = uploads();
    for (int i = 0; i < 3; ++i) {
        VkImageCreateInfo ci = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, NULL, kinds[i].flags, kinds[i].type, VK_FORMAT_R8G8B8A8_UNORM,
                                 { 1, 1, 1 }, 1, kinds[i].layers, VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_TILING_OPTIMAL,
                                 VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, VK_SHARING_MODE_EXCLUSIVE, 0, NULL,
                                 VK_IMAGE_LAYOUT_UNDEFINED };
        if (vkCreateImage(device, &ci, NULL, &dummy_image[i]) != VK_SUCCESS) return 0;
        VkMemoryRequirements req;
        vkGetImageMemoryRequirements(device, dummy_image[i], &req);
        if (!mem_alloc(req, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0, MK_OTHER, &dummy_mem[i])) return 0;
        vkBindImageMemory(device, dummy_image[i], dummy_mem[i].block->memory, dummy_mem[i].offset);
        vk_image tmp = { 0 };
        tmp.image = dummy_image[i]; tmp.aspect = VK_IMAGE_ASPECT_COLOR_BIT; tmp.layout = VK_IMAGE_LAYOUT_UNDEFINED;
        transition(c, &tmp, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        VkClearColorValue zero = { { 0, 0, 0, 0 } };
        VkImageSubresourceRange range = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, kinds[i].layers };
        vkCmdClearColorImage(c, dummy_image[i], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &zero, 1, &range);
        transition(c, &tmp, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    }
    VkImageViewType types[] = { VK_IMAGE_VIEW_TYPE_2D, VK_IMAGE_VIEW_TYPE_2D_ARRAY, VK_IMAGE_VIEW_TYPE_CUBE, VK_IMAGE_VIEW_TYPE_3D };
    int image_of[] = { 0, 0, 1, 2 };
    uint32_t layers_of[] = { 1, 1, 6, 1 };
    for (int i = 0; i < 4; ++i) {
        VkImageViewCreateInfo vi = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, NULL, 0, dummy_image[image_of[i]], types[i],
                                     VK_FORMAT_R8G8B8A8_UNORM, { 0, 0, 0, 0 }, { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, layers_of[i] } };
        if (vkCreateImageView(device, &vi, NULL, &dummy_view[types[i]]) != VK_SUCCESS) return 0;
    }
    VkSamplerCreateInfo si = { VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
    si.magFilter = si.minFilter = VK_FILTER_LINEAR;
    si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    si.maxLod = VK_LOD_CLAMP_NONE;
    vkCreateSampler(device, &si, NULL, &dummy_sampler);
    VkBufferCreateInfo bi = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, NULL, 0, 65536, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                              VK_SHARING_MODE_EXCLUSIVE, 0, NULL };
    vkCreateBuffer(device, &bi, NULL, &dummy_ubo);
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(device, dummy_ubo, &req);
    if (!mem_alloc(req, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, 1, MK_OTHER, &dummy_ubo_mem)) return 0;
    memset(dummy_ubo_mem.map, 0, 65536);
    vkBindBufferMemory(device, dummy_ubo, dummy_ubo_mem.block->memory, dummy_ubo_mem.offset);
    return 1;
}

/* ------------------------------------------------------------------ swap chain */
static VkSurfaceKHR surface;
static VkSwapchainKHR swapchain;
/* Render-done semaphores per swap chain image: the presentation engine holds one until that image is
 * acquired again, which a per-frame-slot semaphore cannot guarantee. */
static struct swap_image {
    VkImageView view;
    VkFramebuffer framebuffer;
    VkSemaphore done;
} *swap_images;
static uint32_t swap_count, swap_w, swap_h;
static VkFormat swap_format;
static VkSurfaceTransformFlagBitsKHR swap_transform;
static VkRenderPass present_pass;
static VkPipeline present_pipeline;
static VkPipelineLayout present_layout;
static VkDescriptorSetLayout present_set_layout;
static VkSampler present_sampler;
static ANativeWindow *swap_window;
static uint32_t window_serial = 0xFFFFFFFFu;
static int swap_broken;

/* Present on its own thread: vkQueuePresentKHR of the Adreno 618's driver took 1.48 ms of the game thread every
 * frame, a CPU-bound game's time.  The game thread hands over the image and goes on; every other use of the
 * queue or the swap chain (submits, acquires, waits for idle, swap chain changes) first waits for that present
 * (Vulkan's external synchronisation), which has long finished by then.  BIOSHOCK_R10_DEBUG bit 65536 presents
 * on the game thread. */
static pthread_mutex_t present_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t present_cond = PTHREAD_COND_INITIALIZER;
static int present_pending, present_started, present_async = -1;
static uint32_t present_index;
static VkSemaphore present_wait;
static uint32_t present_worker_us;

static void *present_main(void *arg)
{
    (void)arg;
    pthread_mutex_lock(&present_lock);
    for (;;) {
        while (!present_pending) pthread_cond_wait(&present_cond, &present_lock);
        uint32_t index = present_index;
        VkSemaphore wait = present_wait;
        pthread_mutex_unlock(&present_lock);
        VkPresentInfoKHR pi = { VK_STRUCTURE_TYPE_PRESENT_INFO_KHR, NULL, 1, &wait, 1, &swapchain, &index, NULL };
        uint64_t start = r10_now_ns();
        VkResult r = vkQueuePresentKHR(queue, &pi);
        if (r == VK_SUCCESS || r == VK_SUBOPTIMAL_KHR) r10_note_present();
        __atomic_add_fetch(&present_worker_us, (uint32_t)((r10_now_ns() - start) / 1000), __ATOMIC_RELAXED);
        pthread_mutex_lock(&present_lock);
        if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_ERROR_SURFACE_LOST_KHR) swap_broken = 1;
        else if (r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR) R10_LOG("Vulkan: present %d", r);
        present_pending = 0;
        pthread_cond_broadcast(&present_cond);
    }
    return NULL;
}

/* Waits until no present is in flight (before the queue or the swap chain is used). */
static void present_drain(void)
{
    if (!present_started) return;
    pthread_mutex_lock(&present_lock);
    if (present_pending) {
        pthread_mutex_unlock(&present_lock);
        uint64_t start = r10_now_ns();
        void_machine_lend_begin();
        pthread_mutex_lock(&present_lock);
        while (present_pending) pthread_cond_wait(&present_cond, &present_lock);
        pthread_mutex_unlock(&present_lock);
        void_machine_lend_end();
        if (r10_dev) r10_dev->stats.wait_us += (uint32_t)((r10_now_ns() - start) / 1000);
        return;
    }
    pthread_mutex_unlock(&present_lock);
}

/* 1 when the present was handed to the worker; 0 to present here. */
static int present_handoff(uint32_t index, VkSemaphore wait)
{
    if (present_async < 0) {
        present_async = !(debug_flags() & 65536);
        R10_LOG("Vulkan: present on %s", present_async ? "its own thread" : "the game thread");
    }
    if (!present_async) return 0;
    if (!present_started) {
        pthread_t thread;
        void_native_worker_offload_begin();   /* the present worker runs on a big core beside the guest core */
        int failed = pthread_create(&thread, NULL, present_main, NULL);
        void_native_worker_affinity_end();
        if (failed) { present_async = 0; return 0; }
        pthread_detach(thread);
        present_started = 1;
    }
    pthread_mutex_lock(&present_lock);
    while (present_pending) pthread_cond_wait(&present_cond, &present_lock);
    present_index = index;
    present_wait = wait;
    present_pending = 1;
    pthread_cond_broadcast(&present_cond);
    pthread_mutex_unlock(&present_lock);
    return 1;
}

static void destroy_swapchain(void)
{
    present_drain();
    if (device) vkDeviceWaitIdle(device);
    for (uint32_t i = 0; swap_images && i < swap_count; ++i) {
        struct swap_image *im = &swap_images[i];
        if (im->framebuffer) vkDestroyFramebuffer(device, im->framebuffer, NULL);
        if (im->view) vkDestroyImageView(device, im->view, NULL);
        if (im->done) vkDestroySemaphore(device, im->done, NULL);
    }
    free(swap_images);
    swap_images = NULL;
    swap_count = 0;
    if (swapchain) vkDestroySwapchainKHR(device, swapchain, NULL);
    swapchain = VK_NULL_HANDLE;
}

static void destroy_surface(void)
{
    destroy_swapchain();
    if (surface) vkDestroySurfaceKHR(instance, surface, NULL);
    surface = VK_NULL_HANDLE;
    if (swap_window) ANativeWindow_release(swap_window);
    swap_window = NULL;
}

static VkPresentModeKHR select_present_mode(void)
{
    /* Mali's Android FIFO can block vkQueuePresentKHR even when the GPU is keeping up. */
    if (gpu_props.vendorID == 0x13B5) {
        VkPresentModeKHR modes[16];
        uint32_t count = sizeof modes / sizeof *modes;
        VkResult result = vkGetPhysicalDeviceSurfacePresentModesKHR(gpu, surface, &count, modes);
        if (result == VK_SUCCESS || result == VK_INCOMPLETE)
            for (uint32_t i = 0; i < count; ++i)
                if (modes[i] == VK_PRESENT_MODE_MAILBOX_KHR) return modes[i];
    }
    return VK_PRESENT_MODE_FIFO_KHR;
}

static VkResult create_swapchain_images(VkSwapchainCreateInfoKHR *info)
{
    VkResult result = vkCreateSwapchainKHR(device, info, NULL, &swapchain);
    if (result != VK_SUCCESS) { swapchain = VK_NULL_HANDLE; return result; }
    result = vkGetSwapchainImagesKHR(device, swapchain, &swap_count, NULL);
    if (result != VK_SUCCESS || !swap_count) {
        destroy_swapchain();
        return result == VK_SUCCESS ? VK_ERROR_INITIALIZATION_FAILED : result;
    }
    VkImage *images = calloc(swap_count, sizeof *images);
    swap_images = calloc(swap_count, sizeof *swap_images);
    result = images && swap_images ? vkGetSwapchainImagesKHR(device, swapchain, &swap_count, images) : VK_ERROR_OUT_OF_HOST_MEMORY;
    swap_w = info->imageExtent.width;
    swap_h = info->imageExtent.height;
    for (uint32_t i = 0; result == VK_SUCCESS && i < swap_count; ++i) {
        struct swap_image *im = &swap_images[i];
        VkImageViewCreateInfo vi = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, NULL, 0, images[i], VK_IMAGE_VIEW_TYPE_2D, swap_format,
                                     { 0, 0, 0, 0 }, { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } };
        result = vkCreateImageView(device, &vi, NULL, &im->view);
        if (result != VK_SUCCESS) break;
        VkFramebufferCreateInfo fi = { VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO, NULL, 0, present_pass, 1, &im->view, swap_w, swap_h, 1 };
        result = vkCreateFramebuffer(device, &fi, NULL, &im->framebuffer);
        if (result != VK_SUCCESS) break;
        VkSemaphoreCreateInfo sci = { VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
        result = vkCreateSemaphore(device, &sci, NULL, &im->done);
    }
    free(images);
    if (result != VK_SUCCESS) destroy_swapchain();
    return result;
}

static int create_swapchain(void)
{
    VkSurfaceCapabilitiesKHR caps;
    if (vkGetPhysicalDeviceSurfaceCapabilitiesKHR(gpu, surface, &caps) != VK_SUCCESS) return 0;
    uint32_t n = 0;
    VkSurfaceFormatKHR formats[32];
    vkGetPhysicalDeviceSurfaceFormatsKHR(gpu, surface, &n, NULL);
    if (n > 32) n = 32;
    vkGetPhysicalDeviceSurfaceFormatsKHR(gpu, surface, &n, formats);
    swap_format = VK_FORMAT_R8G8B8A8_UNORM;
    int found = 0;
    for (uint32_t i = 0; i < n; ++i) if (formats[i].format == VK_FORMAT_R8G8B8A8_UNORM) found = 1;
    if (!found && n) swap_format = formats[0].format;
    swap_transform = caps.currentTransform;
    VkExtent2D extent = caps.currentExtent;
    if (extent.width == 0xFFFFFFFFu) { extent.width = (uint32_t)ANativeWindow_getWidth(swap_window); extent.height = (uint32_t)ANativeWindow_getHeight(swap_window); }
    if (!extent.width || !extent.height) return 0;
    /* Pre-rotation: Android reports the extent in the window's orientation; a swap chain that takes
     * over a 90/270 degree rotation has the display's native orientation, and the present pass rotates. */
    if (swap_transform & (VK_SURFACE_TRANSFORM_ROTATE_90_BIT_KHR | VK_SURFACE_TRANSFORM_ROTATE_270_BIT_KHR)) {
        uint32_t w = extent.width;
        extent.width = extent.height;
        extent.height = w;
    }
    uint32_t images = caps.minImageCount < 3 ? 3 : caps.minImageCount;
    if (caps.maxImageCount && images > caps.maxImageCount) images = caps.maxImageCount;
    VkCompositeAlphaFlagBitsKHR alpha = (caps.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR) ? VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR
                                      : (caps.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR) ? VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR
                                      : VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR;
    VkSwapchainCreateInfoKHR ci = { VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR, NULL, 0, surface, images, swap_format,
                                    VK_COLOR_SPACE_SRGB_NONLINEAR_KHR, extent, 1, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
                                    VK_SHARING_MODE_EXCLUSIVE, 0, NULL, swap_transform, alpha, select_present_mode(), VK_TRUE,
                                    VK_NULL_HANDLE };
    VkResult r = create_swapchain_images(&ci);
    if (r != VK_SUCCESS && ci.presentMode == VK_PRESENT_MODE_MAILBOX_KHR) {
        R10_LOG("Vulkan: MAILBOX unavailable (%d), trying FIFO", r);
        ci.presentMode = VK_PRESENT_MODE_FIFO_KHR;
        r = create_swapchain_images(&ci);
    }
    if (r != VK_SUCCESS) { R10_LOG("Vulkan: swap chain creation %d", r); return 0; }
    R10_LOG("Vulkan: swap chain %ux%u, %u images, transform %u, present mode %s", swap_w, swap_h, swap_count,
            (unsigned)swap_transform, ci.presentMode == VK_PRESENT_MODE_MAILBOX_KHR ? "MAILBOX" : "FIFO");
    return 1;
}

/* Follows the activity's window; 0 while there is none (the caller skips the present). */
static int follow_window(void)
{
    uint32_t serial = 0;
    void *current = void_android_window_current ? void_android_window_current(&serial, 0) : NULL;
    present_drain();
    if (serial == window_serial && swapchain && !swap_broken) return 1;
    if (serial != window_serial || !current) {
        destroy_surface();
        window_serial = serial;
        if (void_android_window_ack) void_android_window_ack(serial);
        if (!current) {
            R10_LOG("Vulkan: surface gone, the game waits for the activity");
            void_blocking_begin();
            current = void_android_window_current(&serial, 1);
            void_blocking_end();
            window_serial = serial;
        }
        swap_window = ut3_dxvk_native_window ? (ANativeWindow *)ut3_dxvk_native_window() : NULL;
        if (!swap_window) return 0;
        VkAndroidSurfaceCreateInfoKHR si = { VK_STRUCTURE_TYPE_ANDROID_SURFACE_CREATE_INFO_KHR, NULL, 0, swap_window };
        if (vkCreateAndroidSurfaceKHR(instance, &si, NULL, &surface) != VK_SUCCESS) { R10_LOG("Vulkan: surface creation failed"); return 0; }
        VkBool32 ok = VK_FALSE;
        vkGetPhysicalDeviceSurfaceSupportKHR(gpu, queue_family, surface, &ok);
        if (void_android_window_ack) void_android_window_ack(serial);
    } else destroy_swapchain();
    swap_broken = 0;
    return create_swapchain();
}

/* ------------------------------------------------------------------ init */
static int load_instance_funcs(void)
{
    int ok = 1;
#define LOAD_I(name) name = (PFN_##name)vkGetInstanceProcAddr_(instance, #name); if (!name && strcmp(#name, "vkGetPhysicalDeviceProperties2")) { R10_LOG("Vulkan: missing %s", #name); ok = 0; }
    VK_INSTANCE_FUNCS(LOAD_I)
#undef LOAD_I
    return ok;
}

static int load_device_funcs(void)
{
    int ok = 1;
#define LOAD_D(name) name = (PFN_##name)vkGetDeviceProcAddr(device, #name); if (!name) { R10_LOG("Vulkan: missing %s", #name); ok = 0; }
    VK_DEVICE_FUNCS(LOAD_D)
#undef LOAD_D
    return ok;
}

static VkShaderModule module_from(const uint32_t *code, size_t bytes)
{
    VkShaderModuleCreateInfo ci = { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, NULL, 0, bytes, code };
    VkShaderModule m = VK_NULL_HANDLE;
    vkCreateShaderModule(device, &ci, NULL, &m);
    return m;
}

static int create_present_pipeline(void)
{
    VkAttachmentDescription a = { 0, VK_FORMAT_R8G8B8A8_UNORM, VK_SAMPLE_COUNT_1_BIT, VK_ATTACHMENT_LOAD_OP_CLEAR, VK_ATTACHMENT_STORE_OP_STORE,
                                  VK_ATTACHMENT_LOAD_OP_DONT_CARE, VK_ATTACHMENT_STORE_OP_DONT_CARE, VK_IMAGE_LAYOUT_UNDEFINED,
                                  VK_IMAGE_LAYOUT_PRESENT_SRC_KHR };
    a.format = swap_format ? swap_format : VK_FORMAT_R8G8B8A8_UNORM;
    VkAttachmentReference ref = { 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
    VkSubpassDescription sub = { 0, VK_PIPELINE_BIND_POINT_GRAPHICS, 0, NULL, 1, &ref, NULL, NULL, 0, NULL };
    VkSubpassDependency dep = { VK_SUBPASS_EXTERNAL, 0, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                                0, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, 0 };
    VkRenderPassCreateInfo rp = { VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO, NULL, 0, 1, &a, 1, &sub, 1, &dep };
    if (vkCreateRenderPass(device, &rp, NULL, &present_pass) != VK_SUCCESS) return 0;
    VkDescriptorSetLayoutBinding b = { 0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL };
    VkDescriptorSetLayoutCreateInfo sl = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, NULL, 0, 1, &b };
    vkCreateDescriptorSetLayout(device, &sl, NULL, &present_set_layout);
    VkPushConstantRange pr = { VK_SHADER_STAGE_VERTEX_BIT, 0, 32 };
    VkPipelineLayoutCreateInfo pl = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, NULL, 0, 1, &present_set_layout, 1, &pr };
    vkCreatePipelineLayout(device, &pl, NULL, &present_layout);
    VkShaderModule vs = module_from(r10_present_vert, sizeof r10_present_vert), fs = module_from(r10_present_frag, sizeof r10_present_frag);
    VkPipelineShaderStageCreateInfo stages[2] = {
        { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, NULL, 0, VK_SHADER_STAGE_VERTEX_BIT, vs, "main", NULL },
        { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, NULL, 0, VK_SHADER_STAGE_FRAGMENT_BIT, fs, "main", NULL } };
    VkPipelineVertexInputStateCreateInfo vi = { VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
    VkPipelineInputAssemblyStateCreateInfo ia = { VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO, NULL, 0, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP, VK_FALSE };
    VkPipelineViewportStateCreateInfo vp = { VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, NULL, 0, 1, NULL, 1, NULL };
    VkPipelineRasterizationStateCreateInfo rs = { VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
    rs.polygonMode = VK_POLYGON_MODE_FILL; rs.cullMode = VK_CULL_MODE_NONE; rs.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo ms = { VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineColorBlendAttachmentState cba = { 0 };
    cba.colorWriteMask = 0xF;
    VkPipelineColorBlendStateCreateInfo cb = { VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO, NULL, 0, VK_FALSE, VK_LOGIC_OP_COPY, 1, &cba };
    VkDynamicState dyn[2] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
    VkPipelineDynamicStateCreateInfo ds = { VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO, NULL, 0, 2, dyn };
    VkGraphicsPipelineCreateInfo pi = { VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, NULL, 0, 2, stages, &vi, &ia, NULL, &vp, &rs, &ms, NULL, &cb, &ds,
                                        present_layout, present_pass, 0, VK_NULL_HANDLE, -1 };
    VkResult r = vkCreateGraphicsPipelines(device, pipeline_cache, 1, &pi, NULL, &present_pipeline);
    vkDestroyShaderModule(device, vs, NULL);
    vkDestroyShaderModule(device, fs, NULL);
    VkSamplerCreateInfo si = { VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
    si.magFilter = si.minFilter = VK_FILTER_LINEAR;
    si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    vkCreateSampler(device, &si, NULL, &present_sampler);
    return r == VK_SUCCESS;
}

static void load_pipeline_cache(void)
{
    const char *dir = void_data_dir();
    const char *driver = getenv("BIOSHOCK_VULKAN_DRIVER");
    snprintf(pipeline_cache_path, sizeof pipeline_cache_path, "%s/r10-pipeline-cache-%s.bin", dir ? dir : ".", driver && *driver ? driver : "system");
    void *data = NULL;
    size_t size = 0;
    FILE *f = fopen(pipeline_cache_path, "rb");
    if (f) {
        fseek(f, 0, SEEK_END);
        long n = ftell(f);
        fseek(f, 0, SEEK_SET);
        if (n > 32 && n < (64l << 20)) {
            data = malloc((size_t)n);
            if (fread(data, 1, (size_t)n, f) == (size_t)n) size = (size_t)n;
        }
        fclose(f);
    }
    /* A cache of another driver or GPU is ignored by the driver itself (header check). */
    VkPipelineCacheCreateInfo ci = { VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO, NULL, 0, size, data };
    if (vkCreatePipelineCache(device, &ci, NULL, &pipeline_cache) != VK_SUCCESS) {
        ci.initialDataSize = 0; ci.pInitialData = NULL;
        vkCreatePipelineCache(device, &ci, NULL, &pipeline_cache);
    }
    R10_LOG("Vulkan: pipeline cache %s (%zu bytes)", pipeline_cache_path, size);
    free(data);
}

static void save_pipeline_cache(void);
extern void void_native_worker_affinity_begin(void);   /* threads.c */
extern void void_native_worker_affinity_end(void);
static uint32_t pipelines_since_save;   /* pipelines created since the cache was last written */
static int save_running;                /* a background save is writing the cache */

static void *save_worker(void *arg)
{
    (void)arg;
    uint64_t start = r10_now_ns();
    save_pipeline_cache();
    static unsigned logged;
    if (logged++ < 8) R10_LOG("Vulkan: pipeline cache saved in the background in %.1f ms", (r10_now_ns() - start) / 1e6);
    __atomic_store_n(&save_running, 0, __ATOMIC_RELEASE);
    return NULL;
}

/* The periodic save runs beside the game: vkGetPipelineCacheData is internally synchronized with the pipeline
 * creations of the game thread (no VK_PIPELINE_CACHE_CREATE_EXTERNALLY_SYNCHRONIZED_BIT), and the serialization
 * and file writes took 80-280 ms of the game thread every minute (POCO X3 NFC).  Only new pipelines start one. */
static void save_pipeline_cache_async(void)
{
    if (!pipeline_cache || !pipeline_cache_path[0] || !__atomic_load_n(&pipelines_since_save, __ATOMIC_RELAXED)) return;
    if (__atomic_exchange_n(&save_running, 1, __ATOMIC_ACQ_REL)) return;
    __atomic_store_n(&pipelines_since_save, 0, __ATOMIC_RELAXED);
    pthread_t t;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    void_native_worker_affinity_begin();   /* a native worker: Android's mask, not the guest's core */
    int error = pthread_create(&t, &attr, save_worker, NULL);
    void_native_worker_affinity_end();
    pthread_attr_destroy(&attr);
    if (error) { __atomic_store_n(&save_running, 0, __ATOMIC_RELEASE); save_pipeline_cache(); }
}

/* Shutdown: a background save finishes first (the device and the cache outlive it). */
static void save_pipeline_cache_wait(void)
{
    for (int i = 0; i < 2000 && __atomic_load_n(&save_running, __ATOMIC_ACQUIRE); ++i) usleep(1000);
}

static void save_pipeline_cache(void)
{
    if (!pipeline_cache || !pipeline_cache_path[0]) return;
    size_t size = 0;
    if (vkGetPipelineCacheData(device, pipeline_cache, &size, NULL) != VK_SUCCESS || !size) return;
    void *data = malloc(size);
    if (vkGetPipelineCacheData(device, pipeline_cache, &size, data) == VK_SUCCESS) {
        char tmp[1220];
        snprintf(tmp, sizeof tmp, "%s.tmp", pipeline_cache_path);
        FILE *f = fopen(tmp, "wb");
        if (f && fwrite(data, 1, size, f) == size && !fclose(f)) rename(tmp, pipeline_cache_path);
        else if (f) fclose(f);
    }
    free(data);
}

/* Sampled and linearly filtered as it is.  Per format, not per feature: textureCompressionBC promises every one
 * of BC1-7, and a driver without BC6H/BC7 (PowerVR D-Series) still samples BC1-3, which its format queries
 * report - the specification's way to use single formats. */
static int texture_format_ok(VkFormat f)
{
    static VkFormat checked[32];
    static int result[32];
    static unsigned count;
    for (unsigned i = 0; i < count; ++i) if (checked[i] == f) return result[i];
    VkFormatProperties fp;
    vkGetPhysicalDeviceFormatProperties(gpu, f, &fp);
    int ok = port_vulkan_samples(fp);
    if (count < 32) { checked[count] = f; result[count] = ok; ++count; }
    return ok;
}

/* The driver's own threads (Mali's event handler, completion threads) are born inside the instance and device
 * creation on the calling guest thread, pinned to the guest machine's prime core: they inherited cpu 7 on the
 * Dimensity 8300 and queued behind the game thread (and it behind them).  They get the big cores beside it. */
static int vk_init_inner(r10_device *d);
static void warm_pipelines(void);
static int vk_init(r10_device *d)
{
    void_native_worker_offload_begin();
    int ok = vk_init_inner(d);
    void_native_worker_affinity_end();
    return ok;
}

static int vk_init_inner(r10_device *d)
{
    (void)d;
    char error[512] = "";
    vkGetInstanceProcAddr_ = r10_vk_loader(error, sizeof error);
    if (!vkGetInstanceProcAddr_) { R10_LOG("Vulkan: %s", error); return 0; }
    vkCreateInstance_ = (PFN_vkCreateInstance)vkGetInstanceProcAddr_(NULL, "vkCreateInstance");
    PFN_vkEnumerateInstanceVersion instance_version = (PFN_vkEnumerateInstanceVersion)vkGetInstanceProcAddr_(NULL, "vkEnumerateInstanceVersion");
    uint32_t loader_api = VK_API_VERSION_1_0;
    if (instance_version) instance_version(&loader_api);
    const char *exts[] = { "VK_KHR_surface", "VK_KHR_android_surface", "VK_KHR_get_physical_device_properties2" };
    VkApplicationInfo app = { VK_STRUCTURE_TYPE_APPLICATION_INFO, NULL, "BioShock", 1, "BioShock", 1,
                              loader_api >= VK_API_VERSION_1_1 ? VK_API_VERSION_1_1 : VK_API_VERSION_1_0 };
    VkInstanceCreateInfo ii = { VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, NULL, 0, &app, 0, NULL, 3, exts };
    VkResult r = vkCreateInstance_ ? vkCreateInstance_(&ii, NULL, &instance) : VK_ERROR_INITIALIZATION_FAILED;
    if (r != VK_SUCCESS) { ii.enabledExtensionCount = 2; r = vkCreateInstance_ ? vkCreateInstance_(&ii, NULL, &instance) : r; }
    if (r != VK_SUCCESS) { R10_LOG("Vulkan: vkCreateInstance %d", r); return 0; }
    if (!load_instance_funcs()) return 0;
    gpu = port_vulkan_physical_device(instance, vkEnumeratePhysicalDevices, vkGetPhysicalDeviceProperties, &gpu_props);
    if (!gpu) { R10_LOG("Vulkan: no GPU"); return 0; }
    if (!r10_vk_driver_active(vkGetInstanceProcAddr_, instance, gpu, &gpu_props, app.apiVersion)) return 0;
    vkGetPhysicalDeviceFeatures(gpu, &gpu_features);
    vkGetPhysicalDeviceMemoryProperties(gpu, &mem_props);
    int core11 = app.apiVersion >= VK_API_VERSION_1_1 && gpu_props.apiVersion >= VK_API_VERSION_1_1;
    uint32_t timestamp_bits = 0;
    queue_family = port_vulkan_graphics_queue(gpu, vkGetPhysicalDeviceQueueFamilyProperties, &timestamp_bits);
    if (queue_family == UINT32_MAX) { R10_LOG("Vulkan: no graphics queue"); return 0; }
    VkPhysicalDeviceFeatures enable = { 0 };
    /* Direct3D reads past the end of a bound constant or vertex buffer as zero, and the game binds
     * constant buffers smaller than its shaders declare: without robust access that is undefined (no
     * picture on Adreno's driver, a GPU fault on Turnip).  DXVK requires the feature for the same reason. */
    if (!gpu_features.robustBufferAccess) { R10_LOG("Vulkan: robustBufferAccess is required"); return 0; }
    enable.robustBufferAccess = VK_TRUE;
    { const char *e = getenv("BIOSHOCK_R10_DEBUG"); if (e && (atoi(e) & 128)) enable.robustBufferAccess = VK_FALSE; }   /* measurements */
    enable.shaderClipDistance = gpu_features.shaderClipDistance;
    enable.samplerAnisotropy = gpu_features.samplerAnisotropy;
    enable.textureCompressionBC = gpu_features.textureCompressionBC;
    /* The converted texture pack (r10_pack.c) is ASTC 4x4 or ETC2; single formats are also checked as below. */
    enable.textureCompressionASTC_LDR = gpu_features.textureCompressionASTC_LDR;
    enable.textureCompressionETC2 = gpu_features.textureCompressionETC2;
    enable.depthClamp = gpu_features.depthClamp;
    enable.depthBiasClamp = gpu_features.depthBiasClamp;
    enable.independentBlend = gpu_features.independentBlend;
    enable.fillModeNonSolid = gpu_features.fillModeNonSolid;
    enable.fragmentStoresAndAtomics = gpu_features.fragmentStoresAndAtomics;
    float priority = 1.0f;
    VkDeviceQueueCreateInfo qi = { VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, NULL, 0, queue_family, 1, &priority };
    const char *dexts[2] = { "VK_KHR_swapchain", NULL };
    uint32_t dext_count = 1;
    /* FP16 arithmetic (the FSR1 EASU filter in half precision, double rate on mobile GPUs). */
    VkPhysicalDeviceShaderFloat16Int8FeaturesKHR f16 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_FLOAT16_INT8_FEATURES_KHR };
    {
        uint32_t n = 0;
        vkEnumerateDeviceExtensionProperties(gpu, NULL, &n, NULL);
        VkExtensionProperties *ext = n ? (VkExtensionProperties *)calloc(n, sizeof *ext) : NULL;
        int has_f16 = 0, has_maintenance1 = 0;
        if (ext && vkEnumerateDeviceExtensionProperties(gpu, NULL, &n, ext) >= 0)
            for (uint32_t i = 0; i < n; ++i) {
                has_f16 |= !strcmp(ext[i].extensionName, VK_KHR_SHADER_FLOAT16_INT8_EXTENSION_NAME);
                has_maintenance1 |= !strcmp(ext[i].extensionName, VK_KHR_MAINTENANCE_1_EXTENSION_NAME);
            }
        free(ext);
        if (!core11) {
            if (!has_maintenance1) { R10_LOG("Vulkan: 1.0 requires VK_KHR_maintenance1"); return 0; }
            dexts[dext_count++] = VK_KHR_MAINTENANCE_1_EXTENSION_NAME;
        }
        PFN_vkGetPhysicalDeviceFeatures2 features2 = (PFN_vkGetPhysicalDeviceFeatures2)vkGetInstanceProcAddr_(instance, "vkGetPhysicalDeviceFeatures2");
        if (!features2) features2 = (PFN_vkGetPhysicalDeviceFeatures2)vkGetInstanceProcAddr_(instance, "vkGetPhysicalDeviceFeatures2KHR");
        /* Requested for measurements only: Adreno 618's driver runs the fp16 EASU at half the speed of fp32. */
        if (core11 && has_f16 && features2 && gpu_features.shaderInt16 && (debug_flags() & 8192)) {
            VkPhysicalDeviceFeatures2 all = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &f16 };
            features2(gpu, &all);
            if (f16.shaderFloat16) {
                f16.shaderInt8 = VK_FALSE;
                f16.pNext = NULL;
                enable.shaderInt16 = VK_TRUE;
                dexts[dext_count++] = "VK_KHR_shader_float16_int8";
                half_shaders = 1;
            }
        }
    }
    VkDeviceCreateInfo di = { VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, half_shaders ? &f16 : NULL, 0, 1, &qi, 0, NULL, dext_count, dexts, &enable };
    r = vkCreateDevice(gpu, &di, NULL, &device);
    if (r != VK_SUCCESS) { R10_LOG("Vulkan: vkCreateDevice %d", r); return 0; }
    if (!load_device_funcs()) return 0;
    vkGetDeviceQueue(device, queue_family, 0, &queue);
    if (timestamp_bits && gpu_props.limits.timestampPeriod > 0.0f) {
        VkQueryPoolCreateInfo qi2 = { VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO, NULL, 0, VK_QUERY_TYPE_TIMESTAMP, 2 * FRAMES, 0 };
        if (vkCreateQueryPool(device, &qi2, NULL, &gpu_timer_pool) != VK_SUCCESS) gpu_timer_pool = VK_NULL_HANDLE;
        gpu_timer_ns_per_tick = gpu_props.limits.timestampPeriod;
    }
    VkFormatProperties fp;
    /* BC per format (vk_samples): "BC" in the line below counts BC1-3 sampled as they are, whatever the
     * textureCompressionBC feature says (it promises all of BC1-7; PowerVR samples BC1-3 without it). */
    bc_ok = 1;
    static const VkFormat bc13[6] = { VK_FORMAT_BC1_RGBA_UNORM_BLOCK, VK_FORMAT_BC1_RGBA_SRGB_BLOCK, VK_FORMAT_BC2_UNORM_BLOCK,
                                      VK_FORMAT_BC2_SRGB_BLOCK, VK_FORMAT_BC3_UNORM_BLOCK, VK_FORMAT_BC3_SRGB_BLOCK };
    for (int i = 0; i < 6; ++i) bc_ok &= texture_format_ok(bc13[i]);
    vkGetPhysicalDeviceFormatProperties(gpu, VK_FORMAT_BC3_SRGB_BLOCK, &fp);
    uint32_t bc_features = fp.optimalTilingFeatures;
    vkGetPhysicalDeviceFormatProperties(gpu, VK_FORMAT_D24_UNORM_S8_UINT, &fp);
    d24s8_ok = (fp.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT) != 0;
    r10_vulkan_backend.bc_supported = bc_ok;
    r10_vulkan_backend.pack_formats =
        (texture_format_ok(VK_FORMAT_ASTC_4x4_UNORM_BLOCK) && texture_format_ok(VK_FORMAT_ASTC_4x4_SRGB_BLOCK) ? R10_PACK_ASTC : 0) |
        (texture_format_ok(VK_FORMAT_ETC2_R8G8B8A8_UNORM_BLOCK) && texture_format_ok(VK_FORMAT_ETC2_R8G8B8A8_SRGB_BLOCK) ? R10_PACK_ETC2 : 0);
    R10_LOG("Vulkan: textureCompressionBC %u (BC3 sRGB features %08X), ASTC LDR %u, ETC2 %u; converted pack formats %d%s",
            gpu_features.textureCompressionBC, bc_features, gpu_features.textureCompressionASTC_LDR, gpu_features.textureCompressionETC2,
            r10_vulkan_backend.pack_formats, (debug_flags() & 131072) ? "; BC decoded on the CPU (BIOSHOCK_R10_DEBUG 131072)" : "");
    char version[96];
    snprintf(version, sizeof version, "Vulkan %u.%u.%u, driver %08X", VK_API_VERSION_MAJOR(gpu_props.apiVersion),
             VK_API_VERSION_MINOR(gpu_props.apiVersion), VK_API_VERSION_PATCH(gpu_props.apiVersion), gpu_props.driverVersion);
    R10_LOG("Vulkan: %s, %s; BC %d, D24S8 %d, robust %d, fp16 %d, clip distance %d, anisotropy %d, depth clamp %d, UBO alignment %llu",
            gpu_props.deviceName, version, bc_ok, d24s8_ok, gpu_features.robustBufferAccess, half_shaders, gpu_features.shaderClipDistance, gpu_features.samplerAnisotropy,
            gpu_features.depthClamp, (unsigned long long)gpu_props.limits.minUniformBufferOffsetAlignment);
    if (void_android_gpu_strings) void_android_gpu_strings(gpu_props.deviceName, version);
    for (int i = 0; i < FRAMES; ++i) {
        VkCommandPoolCreateInfo pi = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, NULL, VK_COMMAND_POOL_CREATE_TRANSIENT_BIT, queue_family };
        vkCreateCommandPool(device, &pi, NULL, &frames[i].pool);
        VkSemaphoreCreateInfo si = { VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
        vkCreateSemaphore(device, &si, NULL, &frames[i].acquire);
        vkCreateSemaphore(device, &si, NULL, &frames[i].done);
    }
    load_pipeline_cache();
    begin_frame_commands();
    if (!create_dummies()) { R10_LOG("Vulkan: dummy resources failed"); return 0; }
    swap_format = VK_FORMAT_R8G8B8A8_UNORM;
    if (!create_present_pipeline()) { R10_LOG("Vulkan: present pipeline failed"); return 0; }
    warm_pipelines();
    return 1;
}

static void vk_shutdown(r10_device *d) { (void)d; save_pipeline_cache_wait(); save_pipeline_cache(); }

/* ------------------------------------------------------------------ resources */
static int vk_samples(const r10_format *f);

/* What the image stores: the format itself, the converted pack's blocks (r10_pack.c) or decoded RGBA8. */
static VkFormat store_format(const r10_resource *r, int *decoded, int *converted)
{
    const r10_format *f = r->fmt;
    *decoded = *converted = 0;
    if (f->kind == R10_FMT_BC && r->converted) {
        *converted = 1;
        if (r10_pack_family() == R10_PACK_ETC2) return f->srgb ? VK_FORMAT_ETC2_R8G8B8A8_SRGB_BLOCK : VK_FORMAT_ETC2_R8G8B8A8_UNORM_BLOCK;
        return f->srgb ? VK_FORMAT_ASTC_4x4_SRGB_BLOCK : VK_FORMAT_ASTC_4x4_UNORM_BLOCK;
    }
    if (f->kind == R10_FMT_BC && !vk_samples(f)) { *decoded = 1; return f->srgb ? VK_FORMAT_R8G8B8A8_SRGB : VK_FORMAT_R8G8B8A8_UNORM; }
    if ((VkFormat)f->vk == VK_FORMAT_D24_UNORM_S8_UINT && !d24s8_ok) return VK_FORMAT_D32_SFLOAT_S8_UINT;
    return (VkFormat)f->vk;
}

/* Render target formats need COLOR_ATTACHMENT and BLEND (the game blends into its HDR target), which
 * Vulkan does not require for every format; one without them is stored in a wider format of the same
 * channels, which views, clears and draws then use as well. */
static VkFormat color_target_format(VkFormat f)
{
    static VkFormat checked[16], result[16];
    static unsigned count;
    for (unsigned i = 0; i < count; ++i) if (checked[i] == f) return result[i];
    VkFormatProperties fp;
    vkGetPhysicalDeviceFormatProperties(gpu, f, &fp);
    const VkFormatFeatureFlags need = VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT |
                                      VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT;
    VkFormat use = f;
    if (f == VK_FORMAT_B10G11R11_UFLOAT_PACK32 && (debug_flags() & 1024)) fp.optimalTilingFeatures = 0;   /* measurements */
    if ((fp.optimalTilingFeatures & need) != need) {
        switch (f) {
        case VK_FORMAT_B10G11R11_UFLOAT_PACK32: case VK_FORMAT_R16G16_SFLOAT: case VK_FORMAT_R16_SFLOAT:
        case VK_FORMAT_R32_SFLOAT: case VK_FORMAT_R32G32_SFLOAT: use = VK_FORMAT_R16G16B16A16_SFLOAT; break;
        case VK_FORMAT_A2B10G10R10_UNORM_PACK32: case VK_FORMAT_R16G16B16A16_UNORM: use = VK_FORMAT_R16G16B16A16_SFLOAT; break;
        default: use = VK_FORMAT_R8G8B8A8_UNORM; break;
        }
        R10_LOG("Vulkan: format %d cannot be a blended render target (features %08X); stored as %d", (int)f,
                (unsigned)fp.optimalTilingFeatures, (int)use);
    }
    if (count < 16) { checked[count] = f; result[count] = use; ++count; }
    return use;
}

static VkFormat unorm_of(VkFormat f)
{
    switch (f) {
    case VK_FORMAT_R8G8B8A8_SRGB: return VK_FORMAT_R8G8B8A8_UNORM;
    case VK_FORMAT_B8G8R8A8_SRGB: return VK_FORMAT_B8G8R8A8_UNORM;
    case VK_FORMAT_BC1_RGBA_SRGB_BLOCK: return VK_FORMAT_BC1_RGBA_UNORM_BLOCK;
    case VK_FORMAT_BC2_SRGB_BLOCK: return VK_FORMAT_BC2_UNORM_BLOCK;
    case VK_FORMAT_BC3_SRGB_BLOCK: return VK_FORMAT_BC3_UNORM_BLOCK;
    case VK_FORMAT_ASTC_4x4_SRGB_BLOCK: return VK_FORMAT_ASTC_4x4_UNORM_BLOCK;
    case VK_FORMAT_ETC2_R8G8B8A8_SRGB_BLOCK: return VK_FORMAT_ETC2_R8G8B8A8_UNORM_BLOCK;
    default: return f;
    }
}

/* BC formats sampled as they are; sRGB ones need their UNORM twin too (the view of "HDR off"). */
static int vk_samples(const r10_format *f)
{
    if (f->kind != R10_FMT_BC) return 1;
    if (debug_flags() & 131072) return 0;   /* measurements: BC decoded on the CPU as before 2026-11-21 */
    VkFormat own = (VkFormat)f->vk, unorm = unorm_of(own);
    return texture_format_ok(own) && (unorm == own || texture_format_ok(unorm));
}

static void upload_image(VkCommandBuffer c, r10_resource *r, uint32_t sub, uint32_t x, uint32_t y, uint32_t z, uint32_t w, uint32_t h,
                         uint32_t depth, const void *data, uint32_t pitch, uint32_t slice);
static int image_create(r10_resource *r, vk_image *im);

static int create_buffer_home(r10_resource *r, vk_buffer *b)
{
    VkBufferCreateInfo bi = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, NULL, 0, r->desc.width,
        VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
        VK_SHARING_MODE_EXCLUSIVE, 0, NULL };
    if (vkCreateBuffer(device, &bi, NULL, &b->buffer) != VK_SUCCESS) return 0;
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(device, b->buffer, &req);
    if (!mem_alloc(req, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT | VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 1, MK_BUFFER, &b->mem) &&
        !mem_alloc(req, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, 1, MK_BUFFER, &b->mem)) {
        vkDestroyBuffer(device, b->buffer, NULL);
        b->buffer = VK_NULL_HANDLE;
        return 0;
    }
    vkBindBufferMemory(device, b->buffer, b->mem.block->memory, b->mem.offset);
    return 1;
}

static int vk_resource_create(r10_device *d, r10_resource *r, const uint32_t *init)
{
    (void)d;
    if (r->desc.dim == R10_DIM_BUFFER) {
        vk_buffer *b = (vk_buffer *)calloc(1, sizeof *b);
        r->be = b;
        if (r->desc.bind & D3D10_BIND_CONSTANT_BUFFER) {
            if (r->desc.usage != D3D10_USAGE_DYNAMIC) {
                b->data = (uint8_t *)calloc(1, r->desc.width);
                if (init && init[0]) memcpy(b->data, GPTR(init[0]), r->desc.width);
            }
            return 1;
        }
        if (r->desc.usage == D3D10_USAGE_STAGING || r->desc.usage == D3D10_USAGE_DYNAMIC) return 1;   /* homes appear at the first static upload */
        if (!create_buffer_home(r, b)) return 0;
        if (init && init[0]) memcpy(b->mem.map, GPTR(init[0]), r->desc.width);
        return 1;
    }
    vk_image *im = (vk_image *)calloc(1, sizeof *im);
    r->be = im;
    if (r->desc.usage == D3D10_USAGE_STAGING) return 1;   /* staging textures live in their shadow */
    if (!image_create(r, im)) return 0;
    if (init) {
        VkCommandBuffer c = uploads();
        for (uint32_t s = 0; s < r->subresources; ++s) {
            const uint32_t *sd = init + 3 * s;
            if (!sd[0]) continue;
            uint32_t w, h, dd;
            r10_subresource_size(r, s, &w, &h, &dd);
            upload_image(c, r, s, 0, 0, 0, w, h, dd, GPTR(sd[0]), sd[1], sd[2]);
        }
        transition(c, im, read_layout(im));
    }
    return 1;
}

/* The image of a texture for its current storage (vk_resource_create, vk_resource_store). */
static int image_create(r10_resource *r, vk_image *im)
{
    const r10_format *f = r->fmt;
    im->format = store_format(r, &im->decoded, &im->converted);
    if ((r->desc.bind & D3D10_BIND_RENDER_TARGET) && f->kind != R10_FMT_DEPTH && !im->decoded) {
        VkFormat use = color_target_format(im->format);
        im->substituted = use != im->format;
        im->format = use;
    }
    im->unorm_format = unorm_of(im->format);
    im->srgb = im->unorm_format != im->format;
    im->depth_kind = f->kind == R10_FMT_DEPTH;
    im->aspect = im->depth_kind ? (f->stencil ? VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT : VK_IMAGE_ASPECT_DEPTH_BIT) : VK_IMAGE_ASPECT_COLOR_BIT;
    im->width = r->desc.width; im->height = r->desc.height; im->depth = r->desc.depth;
    im->mips = r->desc.mips; im->layers = r->desc.layers;
    im->cube = (r->desc.misc & D3D10_RESOURCE_MISC_TEXTURECUBE) != 0;
    VkImageUsageFlags usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    if ((debug_flags() & 4096) && !(r->desc.bind & (D3D10_BIND_RENDER_TARGET | D3D10_BIND_DEPTH_STENCIL)))   /* measurements */
        usage &= ~(VkImageUsageFlags)VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    if (r->desc.bind & D3D10_BIND_RENDER_TARGET) usage |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    if (im->depth_kind) usage |= VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
    VkImageCreateFlags flags = im->cube ? VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT : 0;
    if (im->srgb && !(debug_flags() & 4096)) flags |= VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT;
    VkImageCreateInfo ci = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, NULL, flags,
                             r->desc.dim == R10_DIM_TEXTURE3D ? VK_IMAGE_TYPE_3D : r->desc.dim == R10_DIM_TEXTURE1D ? VK_IMAGE_TYPE_1D : VK_IMAGE_TYPE_2D,
                             im->format, { r->desc.width, r->desc.height, r->desc.depth }, r->desc.mips, r->desc.layers,
                             VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_TILING_OPTIMAL, usage, VK_SHARING_MODE_EXCLUSIVE, 0, NULL,
                             VK_IMAGE_LAYOUT_UNDEFINED };
    if (r->desc.dim == R10_DIM_TEXTURE1D) { ci.imageType = VK_IMAGE_TYPE_2D; }
    VkResult res = vkCreateImage(device, &ci, NULL, &im->image);
    if (res != VK_SUCCESS) { R10_LOG("Vulkan: vkCreateImage %s %ux%u: %d", f->name, r->desc.width, r->desc.height, res); return 0; }
    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(device, im->image, &req);
    int kind = im->converted ? MK_CONVERTED : im->decoded ? MK_DECODED : im->depth_kind ? MK_DEPTH
             : (r->desc.bind & D3D10_BIND_RENDER_TARGET) ? MK_TARGET : MK_TEXTURE;
    /* The game makes every target of every effect at the start, whatever its settings use: on the Mali-G615 at
     * 2944x1840 19 targets took 174 MiB and 9 depth buffers 61 MiB, while a frame of the LowQuality preset drew
     * into three of them.  A large one gets its memory at its first use (attachment, copy, read, upload); a clear
     * alone stays a pending clear and needs none.  BIOSHOCK_R10_DEBUG 1048576 allocates them at once. */
    if ((kind == MK_TARGET || kind == MK_DEPTH) && req.size >= (2u << 20) && !r->is_backbuffer && !(debug_flags() & 1048576)) {
        im->lazy = 1;
        im->lazy_kind = kind;
        im->lazy_req = req;
        im->lazy_w = r->desc.width;
        im->lazy_h = r->desc.height;
        im->lazy_name = f->name;
        im->layout = VK_IMAGE_LAYOUT_UNDEFINED;
        ++lazy_waiting;
        lazy_waiting_bytes += req.size;
        return 1;
    }
    if (!mem_alloc(req, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0, kind, &im->mem)) {
        vkDestroyImage(device, im->image, NULL);
        im->image = VK_NULL_HANDLE;
        return 0;
    }
    if ((kind == MK_TARGET || kind == MK_DEPTH) && req.size >= (1u << 20)) {
        static unsigned logged;
        if (logged++ < 48)
            R10_LOG("Vulkan: %s %ux%u %s (format %d, bind %02X): %.1f MiB; targets %.1f MiB (%u), depth %.1f MiB (%u)",
                    kind == MK_DEPTH ? "depth" : "target", r->desc.width, r->desc.height, f->name, (int)im->format, r->desc.bind,
                    (double)req.size / 1048576.0, (double)mk_bytes[MK_TARGET] / 1048576.0, mk_count[MK_TARGET],
                    (double)mk_bytes[MK_DEPTH] / 1048576.0, mk_count[MK_DEPTH]);
    }
    vkBindImageMemory(device, im->image, im->mem.block->memory, im->mem.offset);
    im->layout = VK_IMAGE_LAYOUT_UNDEFINED;
    return 1;
}

/* The memory of a lazy target, at its first use; views are made where they are bound (view_current). */
static int image_ready(vk_image *im)
{
    if (!im->lazy) return 1;
    if (!mem_alloc(im->lazy_req, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0, im->lazy_kind, &im->mem)) {
        R10_LOG("Vulkan: no memory for the %ux%u %s target at its first use", im->lazy_w, im->lazy_h, im->lazy_name);
        return 0;
    }
    vkBindImageMemory(device, im->image, im->mem.block->memory, im->mem.offset);
    im->lazy = 0;
    --lazy_waiting;
    lazy_waiting_bytes -= im->lazy_req.size;
    static unsigned logged;
    if (logged++ < 32)
        R10_LOG("Vulkan: %s %ux%u %s first used at frame %llu: %.1f MiB; %u large targets still without memory (%.1f MiB)",
                im->lazy_kind == MK_DEPTH ? "depth" : "target", im->lazy_w, im->lazy_h, im->lazy_name,
                (unsigned long long)(r10_dev ? r10_dev->frame : 0), (double)im->lazy_req.size / 1048576.0, lazy_waiting,
                (double)lazy_waiting_bytes / 1048576.0);
    return 1;
}

/* A converted texture leaves the pack (r10_pack.c): a new image for its storage, the old one is destroyed
 * when the frames recorded with it are done; views are rebuilt where they are bound (view_current). */
static int vk_resource_store(r10_device *d, r10_resource *r)
{
    (void)d;
    vk_image *im = img(r);
    if (!im || r->desc.dim == R10_DIM_BUFFER || r->desc.usage == D3D10_USAGE_STAGING) return 0;
    pending_flush_resource(r);
    if (im->image) dead_push(DEAD_IMAGE, (uint64_t)im->image, &im->mem);
    uint32_t generation = im->generation + 1;
    memset(im, 0, sizeof *im);
    im->generation = generation;
    return image_create(r, im);
}

static void vk_resource_destroy(r10_device *d, r10_resource *r)
{
    (void)d;
    pending_flush_resource(r);
    if (r->desc.dim == R10_DIM_BUFFER) {
        vk_buffer *b = buf(r);
        if (!b) return;
        if (b->buffer) dead_push(DEAD_BUFFER, (uint64_t)b->buffer, &b->mem);
        free(b->data);
        free(b);
    } else {
        vk_image *im = img(r);
        if (!im) return;
        if (im->lazy) { --lazy_waiting; lazy_waiting_bytes -= im->lazy_req.size; }
        if (im->image) dead_push(DEAD_IMAGE, (uint64_t)im->image, &im->mem);
        free(im);
    }
    r->be = NULL;
}

/* One subresource (or a box of it) from host memory through the frame ring into the image. */
static void upload_image(VkCommandBuffer c, r10_resource *r, uint32_t sub, uint32_t x, uint32_t y, uint32_t z, uint32_t w, uint32_t h,
                         uint32_t depth, const void *data, uint32_t pitch, uint32_t slice)
{
    vk_image *im = img(r);
    const r10_format *f = r->fmt;
    if (im->substituted) {
        static unsigned logged;
        if (logged++ < 4) R10_LOG("Vulkan: upload into a %s render target stored in another format is skipped", f->name);
        return;
    }
    uint32_t mip = sub % r->desc.mips, layer = sub / r->desc.mips;
    uint32_t lw, lh, ld;
    r10_subresource_size(r, sub, &lw, &lh, &ld);
    uint32_t rows, row_bytes, texel_w = w, texel_h = h;
    if (im->converted) { rows = (h + 3) / 4; row_bytes = ((w + 3) / 4) * R10_PACK_BLOCK_BYTES; }   /* the pack's blocks */
    else if (f->block > 1 && !im->decoded) { rows = (h + 3) / 4; row_bytes = ((w + 3) / 4) * f->bytes; }
    else if (im->decoded) { rows = h; row_bytes = w * 4; }
    else { rows = h; row_bytes = w * f->bytes; }
    uint32_t bytes = rows * row_bytes * depth;
    vk_slice s;
    if (!ring_alloc(bytes ? bytes : 4, 16, &s)) return;
    for (uint32_t k = 0; k < depth; ++k) {
        const uint8_t *src = (const uint8_t *)data + (size_t)k * slice;
        uint8_t *dst = s.map + (size_t)k * rows * row_bytes;
        if (im->decoded) r10_bc_decode(f->bc, f->dxgi == 81 || f->dxgi == 84, src, w, h, pitch, dst, row_bytes);
        else for (uint32_t row = 0; row < rows; ++row) memcpy(dst + (size_t)row * row_bytes, src + (size_t)row * pitch, row_bytes);
    }
    if (r10_dev) r10_dev->stats.uploads_kb += bytes >> 10;
    transition(c, im, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    if (x + texel_w > lw) texel_w = lw - x;
    if (y + texel_h > lh) texel_h = lh - y;
    VkBufferImageCopy region = { s.offset, 0, 0,
                                 { im->depth_kind ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT, mip,
                                   r->desc.dim == R10_DIM_TEXTURE3D ? 0 : layer, 1 },
                                 { (int32_t)x, (int32_t)y, (int32_t)z }, { texel_w, texel_h, depth } };
    vkCmdCopyBufferToImage(c, s.buffer, im->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
}

static void vk_resource_update(r10_device *d, r10_resource *r, uint32_t sub, uint32_t x, uint32_t y, uint32_t z, uint32_t w,
                               uint32_t h, uint32_t depth, const void *data, uint32_t row_pitch, uint32_t slice_pitch)
{
    (void)d;
    if (r->desc.dim == R10_DIM_BUFFER) {
        vk_buffer *b = buf(r);
        if (!b) return;
        if (b->data) { memcpy(b->data + x, data, w); ++b->data_version; return; }
        /* A static home is written in place unless frames in flight still read it: then it is renamed
         * and gets the whole content (the game's mapped memory holds all of it; without a shadow, released
         * by r10_shadow_sweep, the old home does: it stays mapped until the dead list frees it). */
        int fresh = 0;
        const uint8_t *old = NULL;
        if (b->buffer && b->gpu_use > completed_serial) {
            old = b->mem.map;
            dead_push(DEAD_BUFFER, (uint64_t)b->buffer, &b->mem);
            b->buffer = VK_NULL_HANDLE;
        }
        if (!b->buffer) { if (!create_buffer_home(r, b)) return; fresh = 1; }
        if (fresh && r->shadow) memcpy(b->mem.map, GPTR(r->shadow), r->desc.width);
        else {
            if (fresh && old) memcpy(b->mem.map, old, r->desc.width);
            memcpy(b->mem.map + x, data, w);
        }
        return;
    }
    if (r->desc.usage == D3D10_USAGE_STAGING || !img(r) || !img(r)->image || !w) return;
    vk_image *im = img(r);
    pending_flush_resource(r);
    /* The game loads every texture with UpdateSubresource.  One that the commands being recorded do
     * not use yet is written by the upload command buffer (it runs first), so no pass is split. */
    if (im->gpu_use < submit_serial && im->layout != VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL &&
        im->layout != VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL) {
        VkCommandBuffer c = uploads();
        upload_image(c, r, sub, x, y, z, w, h, depth, data, row_pitch, slice_pitch);
        transition(c, im, read_layout(im));
        return;
    }
    end_pass();
    upload_image(cmd, r, sub, x, y, z, w, h, depth, data, row_pitch, slice_pitch);
    transition(cmd, im, read_layout(im));
}

/* r10_shadow_sweep: a static buffer's home is host-visible and coherent (create_buffer_home) and holds all the
 * game wrote (Unmap uploads the whole buffer, updates write it in place or into a renamed home with the rest);
 * out NULL only asks whether it can be read. */
static int vk_buffer_read(r10_device *d, r10_resource *r, void *out)
{
    (void)d;
    vk_buffer *b = buf(r);
    if (!b || b->data || !b->buffer || !b->mem.map) return 0;
    if (out) memcpy(out, b->mem.map, r->desc.width);
    return 1;
}

/* Staging textures: copies into them are recorded into a buffer, Map(READ) waits for it. */
typedef struct staging_read { r10_resource *dst; uint32_t sub; VkBuffer buffer; vk_alloc mem; uint64_t serial; uint32_t row_bytes, rows; } staging_read;
static staging_read reads[16];

static int vk_resource_read(r10_device *d, r10_resource *r, uint32_t sub, void *out, uint32_t row_pitch, uint32_t slice_pitch)
{
    (void)d; (void)slice_pitch;
    for (int i = 0; i < 16; ++i) {
        staging_read *s = &reads[i];
        if (s->dst != r || s->sub != sub || !s->buffer) continue;
        if (s->serial >= submit_serial) submit_partial();
        wait_serial(s->serial);
        for (uint32_t row = 0; row < s->rows; ++row)
            memcpy((uint8_t *)out + (size_t)row * row_pitch, s->mem.map + (size_t)row * s->row_bytes, s->row_bytes < row_pitch ? s->row_bytes : row_pitch);
        dead_push(DEAD_BUFFER, (uint64_t)s->buffer, &s->mem);
        s->buffer = VK_NULL_HANDLE;
        s->dst = NULL;
        return 1;
    }
    return 1;
}

static void vk_copy_inner(r10_device *d, r10_resource *dst, uint32_t dst_sub, uint32_t x, uint32_t y, uint32_t z, r10_resource *src,
                          uint32_t src_sub, const uint32_t *box, int timed);
static void vk_copy(r10_device *d, r10_resource *dst, uint32_t dst_sub, uint32_t x, uint32_t y, uint32_t z, r10_resource *src,
                    uint32_t src_sub, const uint32_t *box)
{
    int open = op_timer_begin(box ? "copy region" : "copy", box ? box[3] - box[0] : src->desc.width, box ? box[4] - box[1] : src->desc.height);
    if (d->tracing) {
        const r10_format *sf = r10_format_get(src->desc.format), *df = r10_format_get(dst->desc.format);
        R10_LOG("TRACE copy after draw %u: %s %ux%u sub %u -> %s %ux%u sub %u at %u,%u box %s (#%llu -> #%llu, guest %08X)", d->stats.draws,
                sf ? sf->name : "?", src->desc.width, src->desc.height, src_sub, df ? df->name : "?", dst->desc.width,
                dst->desc.height, dst_sub, x, y, box ? "yes" : "whole", (unsigned long long)src->obj.id,
                (unsigned long long)dst->obj.id, g_cur_func);
        /* Who asked for it: the guest's last indirect calls (target <- calling function), newest first. */
        char calls[400];
        size_t n = 0;
        uint32_t last = __atomic_load_n(&g_icall_trace_idx, __ATOMIC_RELAXED);
        for (uint32_t k = 1; k <= 8 && n + 40 < sizeof calls; ++k) {
            uint32_t at = (last - k) & (ICALL_TRACE_SIZE - 1u);
            n += (size_t)snprintf(calls + n, sizeof calls - n, " %08X<-%08X", g_icall_trace[at], g_icall_from[at]);
        }
        R10_LOG("TRACE   copy asked by the guest's last indirect calls:%s", n ? calls : " none");
    }
    vk_copy_inner(d, dst, dst_sub, x, y, z, src, src_sub, box, open);
    op_timer_end(open);
}

/* The base/Z prepass can overwrite its first scene-depth snapshot before a shader reads it.
 * Record that copy at its original point, in its own command buffer. Later draws may change
 * the source; only a whole overwrite of the unobserved destination can omit the command buffer. */
static VkCommandBuffer depth_copy_commands(r10_device *d, const vk_image *src, const vk_image *dst, int timed)
{
    if (depth_copy.dst == dst->image) {
        depth_copy.dst = VK_NULL_HANDLE;
        depth_copy.copy = VK_NULL_HANDLE;
        if (depth_copy.timer) pass_info[depth_copy.timer - 1].op = "elided depth copy";
        if (d->tracing) R10_LOG("TRACE   earlier depth copy overwritten before use");
    }
    vk_frame *f = &frames[frame_index];
    if (f->depth_copy_tried || src->layout != VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL || dst->layout != read_layout(dst))
        return cmd;
    f->depth_copy_tried = 1;
    depth_copy.prefix = cmd;
    depth_copy.timer = timed ? pass_timer_count + 1 : 0;
    vkEndCommandBuffer(cmd);
    depth_copy.copy = new_cb(f);
    cmd = new_cb(f);
    return depth_copy.copy;
}

static void vk_copy_inner(r10_device *d, r10_resource *dst, uint32_t dst_sub, uint32_t x, uint32_t y, uint32_t z, r10_resource *src,
                          uint32_t src_sub, const uint32_t *box, int timed)
{
    work_recorded = 1;
    if (dst->desc.dim == R10_DIM_BUFFER) {
        vk_buffer *bs = buf(src), *bd = buf(dst);
        uint32_t sx = box ? box[0] : 0, w = box ? box[3] - box[0] : src->desc.width;
        if (!bs || !bd || sx > src->desc.width || w > src->desc.width - sx || x > dst->desc.width || w > dst->desc.width - x) return;
        const uint8_t *host = bs->data ? bs->data : r10_buffer_shadow(src);
        if (!host && !src->discards && bs->buffer && bs->mem.map) host = bs->mem.map;   /* a released shadow */
        if (host) {
            if (dst->shadow) memcpy(GPTR(dst->shadow + x), host + sx, w);
            vk_resource_update(d, dst, 0, x, 0, 0, w, 1, 1, host + sx, w, w);
        }
        return;
    }
    pending_flush_resource(src);
    pending_flush_resource(dst);
    uint32_t w, h, dd;
    r10_subresource_size(src, src_sub, &w, &h, &dd);
    uint32_t sx = 0, sy = 0, sz = 0;
    if (box) { sx = box[0]; sy = box[1]; sz = box[2]; w = box[3] - box[0]; h = box[4] - box[1]; dd = box[5] - box[2]; }
    if (src->desc.usage == D3D10_USAGE_STAGING) {
        if (!src->shadow) return;
        const uint8_t *base = (const uint8_t *)GPTR(src->shadow + src->sub_offset[src_sub]);
        uint32_t pitch = src->sub_pitch[src_sub];
        vk_resource_update(d, dst, dst_sub, x, y, z, w, h, dd,
                           base + (size_t)(sy / src->fmt->block) * pitch + (size_t)(sx / src->fmt->block) * src->fmt->bytes,
                           pitch, src->sub_slice[src_sub]);
        return;
    }
    vk_image *is = img(src);
    if (!is || !is->image) return;
    end_pass();
    uint32_t smip = src_sub % src->desc.mips, slayer = src_sub / src->desc.mips;
    if (dst->desc.usage == D3D10_USAGE_STAGING) {
        /* GPU -> CPU: into a host buffer that Map(READ) copies to the shadow. */
        if (is->decoded || is->converted || is->depth_kind) return;
        uint32_t row_bytes = r10_format_row_pitch(src->fmt, w), rows = r10_format_rows(src->fmt, h);
        staging_read *s = NULL;
        for (int i = 0; i < 16 && !s; ++i) if (!reads[i].buffer || (reads[i].dst == dst && reads[i].sub == dst_sub)) s = &reads[i];
        if (!s) s = &reads[0];
        if (s->buffer) { dead_push(DEAD_BUFFER, (uint64_t)s->buffer, &s->mem); s->buffer = VK_NULL_HANDLE; }
        VkBufferCreateInfo bi = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, NULL, 0, (VkDeviceSize)row_bytes * rows, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                  VK_SHARING_MODE_EXCLUSIVE, 0, NULL };
        vkCreateBuffer(device, &bi, NULL, &s->buffer);
        VkMemoryRequirements req;
        vkGetBufferMemoryRequirements(device, s->buffer, &req);
        if (!mem_alloc(req, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT, 1, MK_STAGING, &s->mem) &&
            !mem_alloc(req, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, 1, MK_STAGING, &s->mem)) {
            vkDestroyBuffer(device, s->buffer, NULL); s->buffer = VK_NULL_HANDLE; return;
        }
        vkBindBufferMemory(device, s->buffer, s->mem.block->memory, s->mem.offset);
        transition(cmd, is, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        VkBufferImageCopy region = { 0, 0, 0, { VK_IMAGE_ASPECT_COLOR_BIT, smip, slayer, 1 }, { (int32_t)sx, (int32_t)sy, (int32_t)sz }, { w, h, 1 } };
        vkCmdCopyImageToBuffer(cmd, is->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, s->buffer, 1, &region);
        VkMemoryBarrier mb = { VK_STRUCTURE_TYPE_MEMORY_BARRIER, NULL, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_HOST_READ_BIT };
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &mb, 0, NULL, 0, NULL);
        s->dst = dst; s->sub = dst_sub; s->serial = submit_serial; s->row_bytes = row_bytes; s->rows = rows;
        is->gpu_use = submit_serial;
        return;
    }
    vk_image *id = img(dst);
    if (!id || !id->image) return;
    uint32_t dmip = dst_sub % dst->desc.mips, dlayer = dst_sub / dst->desc.mips;
    if (is == id) { R10_LOG("Vulkan: copy within one image skipped"); return; }
    VkImageAspectFlags aspect = is->aspect & id->aspect;
    /* R24_UNORM_X8_TYPELESS has no stencil anyone can read: the game copies its depth buffer into such a texture
     * up to three times a frame (2944x1840 on the Mali-G615), and the stencil plane need not follow. */
    if (dst->desc.format == 46 && (aspect & VK_IMAGE_ASPECT_DEPTH_BIT) && !(debug_flags() & 2097152)) aspect = VK_IMAGE_ASPECT_DEPTH_BIT;
    VkCommandBuffer c = cmd;
    if (g_cur_func == 0x109AE0D0u && aspect == VK_IMAGE_ASPECT_DEPTH_BIT && !(debug_flags() & 33554432) &&
        src->desc.format == 45 && dst->desc.format == 46 && dst->desc.bind == D3D10_BIND_SHADER_RESOURCE &&
        src->desc.dim == R10_DIM_TEXTURE2D && dst->desc.dim == R10_DIM_TEXTURE2D &&
        src->subresources == 1 && dst->subresources == 1 && src->desc.samples == 1 && dst->desc.samples == 1 &&
        !box && !dst_sub && !src_sub && !x && !y && !z && w == id->width && h == id->height &&
        !is->lazy && !id->lazy && is->format == id->format)
        c = depth_copy_commands(d, is, id, timed);
    VkImageLayout src_layout = c == cmd ? read_layout(is) : is->layout;
    transition(c, is, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    transition(c, id, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    if (is->format == id->format || (is->decoded == id->decoded && src->fmt->bytes == dst->fmt->bytes && src->fmt->block == dst->fmt->block)) {
        VkImageCopy region = { { aspect, smip, src->desc.dim == R10_DIM_TEXTURE3D ? 0 : slayer, 1 }, { (int32_t)sx, (int32_t)sy, (int32_t)sz },
                               { aspect, dmip, dst->desc.dim == R10_DIM_TEXTURE3D ? 0 : dlayer, 1 }, { (int32_t)x, (int32_t)y, (int32_t)z }, { w, h, dd } };
        vkCmdCopyImage(c, is->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, id->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
    } else {
        VkImageBlit blit = { { aspect, smip, slayer, 1 }, { { (int32_t)sx, (int32_t)sy, 0 }, { (int32_t)(sx + w), (int32_t)(sy + h), 1 } },
                             { aspect, dmip, dlayer, 1 }, { { (int32_t)x, (int32_t)y, 0 }, { (int32_t)(x + w), (int32_t)(y + h), 1 } } };
        vkCmdBlitImage(c, is->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, id->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_NEAREST);
    }
    is->gpu_use = id->gpu_use = submit_serial;
    transition(c, is, src_layout);
    transition(c, id, read_layout(id));
    if (c != cmd) {
        vkEndCommandBuffer(c);
        depth_copy.dst = id->image;
    }
}

static void vk_resolve(r10_device *d, r10_resource *dst, uint32_t dst_sub, r10_resource *src, uint32_t src_sub)
{
    work_recorded = 1;
    vk_copy(d, dst, dst_sub, 0, 0, 0, src, src_sub, NULL);
}

static void vk_generate_mips_inner(r10_device *d, r10_view *v);
static void vk_generate_mips(r10_device *d, r10_view *v)
{
    int open = op_timer_begin("mips", v->res->desc.width, v->res->desc.height);
    vk_generate_mips_inner(d, v);
    op_timer_end(open);
}
static void vk_generate_mips_inner(r10_device *d, r10_view *v)
{
    work_recorded = 1;
    (void)d;
    vk_image *im = img(v->res);
    if (!im || !im->image || im->mips < 2 || im->decoded || v->res->fmt->kind == R10_FMT_BC) return;
    if (im->lazy && !image_ready(im)) return;
    pending_flush_resource(v->res);
    end_pass();
    for (uint32_t m = 1; m < im->mips; ++m) {
        VkImageMemoryBarrier b[2] = {
            { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, NULL, layout_access(im->layout), VK_ACCESS_TRANSFER_READ_BIT, im->layout,
              VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, im->image, { im->aspect, m - 1, 1, 0, im->layers } },
            { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, NULL, layout_access(im->layout), VK_ACCESS_TRANSFER_WRITE_BIT, im->layout,
              VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, im->image, { im->aspect, m, 1, 0, im->layers } } };
        if (m > 1) b[0].oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, b[0].srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 2, b);
        int32_t sw = (int32_t)(im->width >> (m - 1)), sh = (int32_t)(im->height >> (m - 1)), dw = (int32_t)(im->width >> m), dh = (int32_t)(im->height >> m);
        VkImageBlit blit = { { im->aspect, m - 1, 0, im->layers }, { { 0, 0, 0 }, { sw ? sw : 1, sh ? sh : 1, 1 } },
                             { im->aspect, m, 0, im->layers }, { { 0, 0, 0 }, { dw ? dw : 1, dh ? dh : 1, 1 } } };
        vkCmdBlitImage(cmd, im->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, im->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_LINEAR);
    }
    /* Levels are now SRC (0..n-2) and DST (n-1): one barrier brings all to the read layout. */
    VkImageMemoryBarrier all[2] = {
        { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, NULL, VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_SHADER_READ_BIT, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
          read_layout(im), VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, im->image, { im->aspect, 0, im->mips - 1, 0, im->layers } },
        { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, NULL, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
          read_layout(im), VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, im->image, { im->aspect, im->mips - 1, 1, 0, im->layers } } };
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, NULL, 0, NULL, 2, all);
    im->layout = read_layout(im);
    im->gpu_use = submit_serial;
}

/* ------------------------------------------------------------------ views */
static VkImageViewType srv_view_type(const r10_view *v, const vk_image *im)
{
    switch (v->dimension) {
    case D3D10_SRV_DIMENSION_TEXTURE1D: case D3D10_SRV_DIMENSION_TEXTURE2D: case D3D10_SRV_DIMENSION_TEXTURE2DMS: return VK_IMAGE_VIEW_TYPE_2D;
    case D3D10_SRV_DIMENSION_TEXTURE1DARRAY: case D3D10_SRV_DIMENSION_TEXTURE2DARRAY: case D3D10_SRV_DIMENSION_TEXTURE2DMSARRAY: return VK_IMAGE_VIEW_TYPE_2D_ARRAY;
    case D3D10_SRV_DIMENSION_TEXTURE3D: return VK_IMAGE_VIEW_TYPE_3D;
    case D3D10_SRV_DIMENSION_TEXTURECUBE: return VK_IMAGE_VIEW_TYPE_CUBE;
    default: (void)im; return VK_IMAGE_VIEW_TYPE_2D;
    }
}

static VkComponentMapping swizzle_of(const r10_format *f)
{
    VkComponentMapping m = { VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY };
    if (f->gl_r_to_a) { m.r = m.g = m.b = VK_COMPONENT_SWIZZLE_ZERO; m.a = VK_COMPONENT_SWIZZLE_R; }
    if (f->gl_alpha_one) m.a = VK_COMPONENT_SWIZZLE_ONE;
    return m;
}

static int view_build(r10_view *v, vk_view *bv, vk_image *im);

static int vk_view_create(r10_device *d, r10_view *v)
{
    (void)d;
    vk_image *im = img(v->res);
    vk_view *bv = (vk_view *)calloc(1, sizeof *bv);
    v->be = bv;
    if (!im || !im->image) return 1;   /* staging or buffer views have nothing to view */
    if (im->lazy) {
        bv->generation = im->generation;
        bv->lazy_view = 1;
        uint32_t mip = v->first_mip;
        bv->width = im->width >> mip ? im->width >> mip : 1;
        bv->height = im->height >> mip ? im->height >> mip : 1;
        bv->type = v->obj.kind == R10_KIND_SRV ? srv_view_type(v, im) : VK_IMAGE_VIEW_TYPE_2D;
        return 1;
    }
    return view_build(v, bv, im);
}

/* A view of a texture whose storage was recreated (vk_resource_store: it left the converted pack) is made
 * again for the new image where it is bound; the old image views go when their frames are done. */
static vk_view *view_current(r10_view *v)
{
    vk_view *bv = v ? (vk_view *)v->be : NULL;
    vk_image *im = v ? img(v->res) : NULL;
    if (!bv || !im) return bv;
    if (bv->generation == im->generation && !bv->lazy_view) return bv;
    if (im->lazy && !image_ready(im)) return bv;
    if (bv->generation != im->generation) {
        if (bv->view) { forget_framebuffers(bv->view); dead_push(DEAD_VIEW, (uint64_t)bv->view, NULL); }
        if (bv->view_unorm) dead_push(DEAD_VIEW, (uint64_t)bv->view_unorm, NULL);
        bv->view = bv->view_unorm = VK_NULL_HANDLE;
    }
    bv->generation = im->generation;
    bv->lazy_view = 0;
    if (im->image && !bv->view) view_build(v, bv, im);
    return bv;
}

static int view_build(r10_view *v, vk_view *bv, vk_image *im)
{
    bv->generation = im->generation;
    uint32_t mip = v->first_mip;
    bv->width = im->width >> mip ? im->width >> mip : 1;
    bv->height = im->height >> mip ? im->height >> mip : 1;
    VkImageViewCreateInfo vi = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, NULL, 0, im->image, VK_IMAGE_VIEW_TYPE_2D, im->format,
                                 swizzle_of(v->fmt), { im->aspect, v->first_mip, v->mips, v->first_layer, v->layers } };
    if (v->obj.kind == R10_KIND_SRV) {
        bv->type = srv_view_type(v, im);
        vi.viewType = bv->type;
        if (im->depth_kind) vi.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
        if (bv->type == VK_IMAGE_VIEW_TYPE_CUBE) vi.subresourceRange.layerCount = 6;
        if (bv->type == VK_IMAGE_VIEW_TYPE_3D) { vi.subresourceRange.baseArrayLayer = 0; vi.subresourceRange.layerCount = 1; }
        /* The view's own sRGB-ness: an sRGB view of an sRGB image decodes, a UNORM view does not. */
        if (im->srgb && !v->fmt->srgb && !(debug_flags() & 4096)) vi.format = im->unorm_format;
    } else {
        vi.components.r = vi.components.g = vi.components.b = vi.components.a = VK_COMPONENT_SWIZZLE_IDENTITY;
        vi.subresourceRange.levelCount = 1;
        vi.viewType = v->layers > 1 ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_2D;
        if (v->layers == 1) vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
        if (v->res->desc.dim == R10_DIM_TEXTURE3D) { vi.viewType = VK_IMAGE_VIEW_TYPE_2D; vi.subresourceRange.baseArrayLayer = 0; vi.subresourceRange.layerCount = 1; }
        bv->type = vi.viewType;
    }
    if (vkCreateImageView(device, &vi, NULL, &bv->view) != VK_SUCCESS) { R10_LOG("Vulkan: image view of %s failed", v->fmt->name); return 0; }
    if (v->obj.kind == R10_KIND_SRV && im->srgb && v->fmt->srgb && !(debug_flags() & 4096)) {
        vi.format = im->unorm_format;
        vkCreateImageView(device, &vi, NULL, &bv->view_unorm);
    }
    return 1;
}

static void vk_view_destroy(r10_device *d, r10_view *v)
{
    (void)d;
    vk_view *bv = (vk_view *)v->be;
    if (!bv) return;
    /* A target that never had memory has nothing to clear before it goes. */
    if (bv->pending_clear && !(img(v->res) && img(v->res)->lazy)) pending_clear_flush(v);
    pending_forget(v);
    if (is_attachment(v)) end_pass();
    if (bv->view) { forget_framebuffers(bv->view); dead_push(DEAD_VIEW, (uint64_t)bv->view, NULL); }
    if (bv->view_unorm) dead_push(DEAD_VIEW, (uint64_t)bv->view_unorm, NULL);
    free(bv);
    v->be = NULL;
}

/* ------------------------------------------------------------------ samplers */
static VkSamplerAddressMode vk_address(uint32_t a)
{
    switch (a) {
    case D3D10_TEXTURE_ADDRESS_MIRROR: return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
    case D3D10_TEXTURE_ADDRESS_CLAMP: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    case D3D10_TEXTURE_ADDRESS_BORDER: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    case D3D10_TEXTURE_ADDRESS_MIRROR_ONCE: return VK_SAMPLER_ADDRESS_MODE_MIRROR_CLAMP_TO_EDGE;
    default: return VK_SAMPLER_ADDRESS_MODE_REPEAT;
    }
}
static VkCompareOp vk_compare(uint32_t f)
{
    static const VkCompareOp t[] = { VK_COMPARE_OP_ALWAYS, VK_COMPARE_OP_NEVER, VK_COMPARE_OP_LESS, VK_COMPARE_OP_EQUAL, VK_COMPARE_OP_LESS_OR_EQUAL,
                                     VK_COMPARE_OP_GREATER, VK_COMPARE_OP_NOT_EQUAL, VK_COMPARE_OP_GREATER_OR_EQUAL, VK_COMPARE_OP_ALWAYS };
    return f < 9 ? t[f] : VK_COMPARE_OP_ALWAYS;
}

static int vk_sampler_create(r10_device *d, r10_state_object *s)
{
    (void)d;
    const r10_sampler_desc *sd = &s->d.sampler;
    uint32_t max_anisotropy, f = r10_sampler_filter(sd, &max_anisotropy);
    int anisotropic = (f & 0x40) != 0;
    VkSamplerCreateInfo si = { VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
    si.magFilter = (f & 0x04) || anisotropic ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
    si.minFilter = (f & 0x10) || anisotropic ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
    si.mipmapMode = (f & 0x01) || anisotropic ? VK_SAMPLER_MIPMAP_MODE_LINEAR : VK_SAMPLER_MIPMAP_MODE_NEAREST;
    si.addressModeU = vk_address(sd->address_u);
    si.addressModeV = vk_address(sd->address_v);
    si.addressModeW = vk_address(sd->address_w);
    si.mipLodBias = sd->mip_lod_bias;
    si.anisotropyEnable = anisotropic && gpu_features.samplerAnisotropy;
    float a = (float)max_anisotropy;
    si.maxAnisotropy = a > gpu_props.limits.maxSamplerAnisotropy ? gpu_props.limits.maxSamplerAnisotropy : a;
    si.compareEnable = (f & 0x80) != 0;
    si.compareOp = vk_compare(sd->comparison);
    si.minLod = sd->min_lod;
    si.maxLod = sd->max_lod > 1000.0f ? VK_LOD_CLAMP_NONE : sd->max_lod;
    si.borderColor = sd->border[3] == 0.0f ? VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK
                   : sd->border[0] == 0.0f ? VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK : VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE;
    vk_sampler *vs = (vk_sampler *)calloc(1, sizeof *vs);
    if (vkCreateSampler(device, &si, NULL, &vs->sampler) != VK_SUCCESS) { free(vs); return 0; }
    s->be = vs;
    return 1;
}

static void vk_sampler_destroy(r10_device *d, r10_state_object *s)
{
    (void)d;
    vk_sampler *vs = (vk_sampler *)s->be;
    if (vs) { dead_push(DEAD_SAMPLER, (uint64_t)vs->sampler, NULL); free(vs); }
    s->be = NULL;
}

/* ------------------------------------------------------------------ shaders and programs */
/* The shader module of a game shader, made at its first pipeline.  A level load creates every shader its
 * materials might use (4089 in the Arcadia save) and the driver keeps each module's SPIR-V on the host heap, while
 * a scene draws a few hundred: on the Mali-G615 the host heap went from 54 MiB at the menu to 111-123 MiB in play.
 * BIOSHOCK_R10_DEBUG 524288 makes every module when the game creates the shader (as before, for A/B). */
static VkShaderModule shader_module(r10_shader *s)
{
    vk_shader *vs = s ? (vk_shader *)s->be : NULL;
    if (!vs || vs->module || vs->module_failed) return vs ? vs->module : VK_NULL_HANDLE;
    uint32_t words = 0;
    uint32_t *code = r10_shaderlib_spirv(s->lib, !gpu_features.shaderClipDistance || (debug_flags() & 32), &words);
    if (code) {
        if ((debug_flags() & 64) && s->type == 0 && __builtin_popcount(s->lib->input_mask) >= 4)   /* scene shaders, not the copies */
            vs->module = module_from(r10_debug_frag, sizeof r10_debug_frag);
        else vs->module = module_from(code, (size_t)words * 4);
        free(code);
    }
    if (vs->module) ++shader_modules;
    else {
        vs->module_failed = 1;
        R10_LOG("Vulkan: shader module %016llx could not be made", (unsigned long long)s->hash);
    }
    return vs->module;
}

static int vk_shader_create(r10_device *d, r10_shader *s)
{
    (void)d;
    if (s->type > 1) return 0;
    vk_shader *vs = (vk_shader *)calloc(1, sizeof *vs);
    s->be = vs;
    if (debug_flags() & 524288) { if (!shader_module(s)) { s->be = NULL; free(vs); return 0; } }
    if (s->lib->icb_size) {
        VkBufferCreateInfo bi = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, NULL, 0, s->lib->icb_size, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                                  VK_SHARING_MODE_EXCLUSIVE, 0, NULL };
        vkCreateBuffer(device, &bi, NULL, &vs->icb);
        VkMemoryRequirements req;
        vkGetBufferMemoryRequirements(device, vs->icb, &req);
        if (mem_alloc(req, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, 1, MK_OTHER, &vs->icb_mem)) {
            memcpy(vs->icb_mem.map, r10_lib_at(s->lib->icb), s->lib->icb_size);
            vkBindBufferMemory(device, vs->icb, vs->icb_mem.block->memory, vs->icb_mem.offset);
        }
    }
    return 1;
}

/* A program is the pair of shaders: its descriptor set layout, pipeline layout and pipelines. */
typedef struct vk_binding { uint32_t binding, type, stage, slot, view_type; } vk_binding;
typedef struct vk_pipeline { uint8_t key[320]; uint32_t key_size; uint32_t hash; VkPipeline pipeline; struct vk_pipeline *next; } vk_pipeline;
typedef struct vk_program {
    r10_shader *vs, *ps;
    VkDescriptorSetLayout set_layout;
    VkPipelineLayout layout;
    vk_binding bindings[96]; uint32_t binding_count;
    uint32_t dynamic_count;
    vk_pipeline *pipelines[64];
    vk_pipeline *last;           /* the pipeline of the program's previous draw */
    struct vk_program *next;
    /* the last descriptor set and what it held */
    VkDescriptorSet set; uint64_t set_frame; uint64_t set_hash;
} vk_program;
#define PROGRAM_BUCKETS 4096
static vk_program *programs[PROGRAM_BUCKETS];

static void program_destroy(vk_program *p)
{
    for (int i = 0; i < 64; ++i)
        while (p->pipelines[i]) { vk_pipeline *e = p->pipelines[i]; p->pipelines[i] = e->next; dead_push(DEAD_PIPELINE, (uint64_t)e->pipeline, NULL); free(e); }
    /* Layouts may still be used by recorded commands: they are tiny, keep them. */
    free(p);
}

static void vk_shader_destroy(r10_device *d, r10_shader *s)
{
    (void)d;
    vk_shader *vs = (vk_shader *)s->be;
    for (unsigned b = 0; b < PROGRAM_BUCKETS; ++b)
        for (vk_program **p = &programs[b]; *p;) {
            if ((*p)->vs == s || (*p)->ps == s) { vk_program *e = *p; *p = e->next; program_destroy(e); }
            else p = &(*p)->next;
        }
    if (vs) {
        if (vs->module) { vkDestroyShaderModule(device, vs->module, NULL); --shader_modules; }
        if (vs->icb) dead_push(DEAD_BUFFER, (uint64_t)vs->icb, &vs->icb_mem);
        free(vs);
    }
    s->be = NULL;
}

static void program_layout(vk_program *p)
{
    r10_shader *vs = p->vs, *ps = p->ps;
    VkDescriptorSetLayoutBinding lb[96];
    uint32_t n = 0;
    r10_shader *stages[2] = { vs, ps };
    uint32_t max_dynamic = gpu_props.limits.maxDescriptorSetUniformBuffersDynamic;
    for (int s = 0; s < 2; ++s) {
        if (!stages[s] || !stages[s]->lib) continue;
        const r10_lib_binding *b = (const r10_lib_binding *)r10_lib_at(stages[s]->lib->bindings);
        uint32_t stage_offset = stages[s]->type * 160u;
        for (uint32_t i = 0; i < stages[s]->lib->binding_count && n < 96; ++i) {
            uint32_t type = b[i].descriptor_type;
            if (type == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER && p->dynamic_count < max_dynamic) { type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC; ++p->dynamic_count; }
            lb[n].binding = b[i].binding;
            lb[n].descriptorType = (VkDescriptorType)type;
            lb[n].descriptorCount = 1;
            lb[n].stageFlags = stages[s]->type == 1 ? VK_SHADER_STAGE_VERTEX_BIT : VK_SHADER_STAGE_FRAGMENT_BIT;
            lb[n].pImmutableSamplers = NULL;
            uint32_t local = b[i].binding - stage_offset;
            p->bindings[n].binding = b[i].binding;
            p->bindings[n].type = type;
            p->bindings[n].stage = stages[s]->type;
            p->bindings[n].slot = local < 16 ? local : local < 32 ? local - 16 : local - 32;
            p->bindings[n].view_type = b[i].view_type;
            ++n;
        }
    }
    p->binding_count = n;
    /* Dynamic offsets apply in increasing binding number, and vk_draw fills them in the order of
     * p->bindings: the vertex stage's buffers (160+) must not come first. */
    for (uint32_t i = 1; i < n; ++i)
        for (uint32_t j = i; j > 0 && p->bindings[j].binding < p->bindings[j - 1].binding; --j) {
            vk_binding t = p->bindings[j]; p->bindings[j] = p->bindings[j - 1]; p->bindings[j - 1] = t;
        }
    VkDescriptorSetLayoutCreateInfo sl = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, NULL, 0, n, lb };
    vkCreateDescriptorSetLayout(device, &sl, NULL, &p->set_layout);
    VkPipelineLayoutCreateInfo pl = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, NULL, 0, 1, &p->set_layout, 0, NULL };
    vkCreatePipelineLayout(device, &pl, NULL, &p->layout);
}

static vk_program *program_for(r10_shader *vs, r10_shader *ps)
{
    unsigned bucket = (unsigned)((((uintptr_t)vs >> 4) * 31u) ^ ((uintptr_t)ps >> 4)) % PROGRAM_BUCKETS;
    for (vk_program *p = programs[bucket]; p; p = p->next) if (p->vs == vs && p->ps == ps) return p;
    vk_program *p = (vk_program *)calloc(1, sizeof *p);
    p->vs = vs; p->ps = ps;
    program_layout(p);
    p->next = programs[bucket];
    programs[bucket] = p;
    return p;
}

/* ------------------------------------------------------------------ pipelines */
static const r10_blend_desc default_blend = { 0, { 0 }, D3D10_BLEND_ONE, D3D10_BLEND_ZERO, D3D10_BLEND_OP_ADD, D3D10_BLEND_ONE,
                                              D3D10_BLEND_ZERO, D3D10_BLEND_OP_ADD, { 15, 15, 15, 15, 15, 15, 15, 15 } };
static const r10_depth_desc default_depth = { 1, 1, D3D10_COMPARISON_LESS, 0, 0xFF, 0xFF, { 0, 0 },
                                              { 1, 1, 1, D3D10_COMPARISON_ALWAYS }, { 1, 1, 1, D3D10_COMPARISON_ALWAYS } };
static const r10_raster_desc default_raster = { D3D10_FILL_SOLID, D3D10_CULL_BACK, 0, 0, 0, 0, 1, 0, 0, 0 };

typedef struct pipe_key {
    uint64_t layout_id;
    uint32_t topology;
    uint32_t strides[R10_VB_SLOTS];
    r10_blend_desc blend;
    r10_depth_desc depth;
    r10_raster_desc raster;
    pass_key pass;
    uint32_t sample_mask;
} pipe_key;

typedef struct warm_pipeline {
    uint64_t vs, ps;
    pipe_key state;
    uint32_t element_count, stencil;
    r10_element elements[32];
} warm_pipeline;

static void trace_warm_pipeline(r10_device *d, const vk_program *program, const pipe_key *key)
{
    if (!(debug_flags() & R10_DEBUG_RECORD_WARMUP) || !void_data_dir()) return;
    static FILE *file;
    if (!file) {
        char path[1200];
        snprintf(path, sizeof path, "%s/r10-pipelines.raw", void_data_dir());
        file = fopen(path, "wb");
    }
    if (!file) return;
    warm_pipeline recipe = {0};
    recipe.vs = program->vs->hash;
    recipe.ps = program->ps ? program->ps->hash : 0;
    recipe.state = *key;
    recipe.state.layout_id = 0;
    recipe.stencil = cur.dsv && cur.dsv->fmt->stencil;
    if (d->st.layout) {
        recipe.element_count = d->st.layout->count;
        memcpy(recipe.elements, d->st.layout->el, recipe.element_count * sizeof recipe.elements[0]);
    }
    fwrite(&d->frame, sizeof d->frame, 1, file);
    fwrite(&recipe, sizeof recipe, 1, file);
    fflush(file);
}

static VkBlendFactor vk_blend(uint32_t b, int alpha)
{
    switch (b) {
    case D3D10_BLEND_ZERO: return VK_BLEND_FACTOR_ZERO;
    case D3D10_BLEND_ONE: return VK_BLEND_FACTOR_ONE;
    case D3D10_BLEND_SRC_COLOR: return alpha ? VK_BLEND_FACTOR_SRC_ALPHA : VK_BLEND_FACTOR_SRC_COLOR;
    case D3D10_BLEND_INV_SRC_COLOR: return alpha ? VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA : VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
    case D3D10_BLEND_SRC_ALPHA: return VK_BLEND_FACTOR_SRC_ALPHA;
    case D3D10_BLEND_INV_SRC_ALPHA: return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    case D3D10_BLEND_DEST_ALPHA: return VK_BLEND_FACTOR_DST_ALPHA;
    case D3D10_BLEND_INV_DEST_ALPHA: return VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
    case D3D10_BLEND_DEST_COLOR: return alpha ? VK_BLEND_FACTOR_DST_ALPHA : VK_BLEND_FACTOR_DST_COLOR;
    case D3D10_BLEND_INV_DEST_COLOR: return alpha ? VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA : VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR;
    case D3D10_BLEND_SRC_ALPHA_SAT: return VK_BLEND_FACTOR_SRC_ALPHA_SATURATE;
    case D3D10_BLEND_BLEND_FACTOR: return alpha ? VK_BLEND_FACTOR_CONSTANT_ALPHA : VK_BLEND_FACTOR_CONSTANT_COLOR;
    case D3D10_BLEND_INV_BLEND_FACTOR: return alpha ? VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_ALPHA : VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR;
    case D3D10_BLEND_SRC1_COLOR: return VK_BLEND_FACTOR_SRC1_COLOR;
    case D3D10_BLEND_INV_SRC1_COLOR: return VK_BLEND_FACTOR_ONE_MINUS_SRC1_COLOR;
    case D3D10_BLEND_SRC1_ALPHA: return VK_BLEND_FACTOR_SRC1_ALPHA;
    case D3D10_BLEND_INV_SRC1_ALPHA: return VK_BLEND_FACTOR_ONE_MINUS_SRC1_ALPHA;
    default: return VK_BLEND_FACTOR_ONE;
    }
}
static VkBlendOp vk_blend_op(uint32_t op)
{
    switch (op) {
    case D3D10_BLEND_OP_SUBTRACT: return VK_BLEND_OP_SUBTRACT;
    case D3D10_BLEND_OP_REV_SUBTRACT: return VK_BLEND_OP_REVERSE_SUBTRACT;
    case D3D10_BLEND_OP_MIN: return VK_BLEND_OP_MIN;
    case D3D10_BLEND_OP_MAX: return VK_BLEND_OP_MAX;
    default: return VK_BLEND_OP_ADD;
    }
}
static VkStencilOp vk_stencil_op(uint32_t op)
{
    static const VkStencilOp t[] = { VK_STENCIL_OP_KEEP, VK_STENCIL_OP_KEEP, VK_STENCIL_OP_ZERO, VK_STENCIL_OP_REPLACE, VK_STENCIL_OP_INCREMENT_AND_CLAMP,
                                     VK_STENCIL_OP_DECREMENT_AND_CLAMP, VK_STENCIL_OP_INVERT, VK_STENCIL_OP_INCREMENT_AND_WRAP, VK_STENCIL_OP_DECREMENT_AND_WRAP };
    return op < 9 ? t[op] : VK_STENCIL_OP_KEEP;
}

static VkPipeline compile_pipeline(vk_program *p, const pipe_key *key, const r10_layout *layout,
                                   VkRenderPass pass, int stencil)
{
    pipe_key k = *key;
    if (!pass || !p->layout) return VK_NULL_HANDLE;
    VkShaderModule vs_module = shader_module(p->vs), fs_module = p->ps ? shader_module(p->ps) : VK_NULL_HANDLE;
    if (!vs_module || (p->ps && !fs_module)) return VK_NULL_HANDLE;
    vk_shader *fs = p->ps ? (vk_shader *)p->ps->be : NULL;
    VkPipelineShaderStageCreateInfo stages[2] = {
        { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, NULL, 0, VK_SHADER_STAGE_VERTEX_BIT, vs_module, "main", NULL },
        { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, NULL, 0, VK_SHADER_STAGE_FRAGMENT_BIT, fs_module, "main", NULL } };
    VkVertexInputBindingDescription vb[R10_VB_SLOTS];
    VkVertexInputAttributeDescription va[32];
    uint32_t nb = 0, na = 0;
    if (layout) {
        for (uint32_t slot = 0; slot < R10_VB_SLOTS; ++slot) {
            if (!(layout->slot_mask & (1u << slot))) continue;
            int instanced = 0;
            for (uint32_t i = 0; i < layout->count; ++i) if (layout->el[i].slot == slot && layout->el[i].instanced) instanced = 1;
            vb[nb].binding = slot; vb[nb].stride = k.strides[slot];
            vb[nb].inputRate = instanced ? VK_VERTEX_INPUT_RATE_INSTANCE : VK_VERTEX_INPUT_RATE_VERTEX;
            ++nb;
        }
        for (uint32_t i = 0; i < layout->count && na < 32; ++i) {
            const r10_element *e = &layout->el[i];
            const r10_format *f = r10_format_get(e->format);
            va[na].location = e->location; va[na].binding = e->slot;
            va[na].format = f ? (VkFormat)f->vk : VK_FORMAT_R32G32B32A32_SFLOAT;
            va[na].offset = e->offset;
            ++na;
        }
    }
    VkPipelineVertexInputStateCreateInfo vi = { VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO, NULL, 0, nb, vb, na, va };
    VkPrimitiveTopology topo = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    switch (k.topology) {
    case D3D10_PRIMITIVE_TOPOLOGY_POINTLIST: topo = VK_PRIMITIVE_TOPOLOGY_POINT_LIST; break;
    case D3D10_PRIMITIVE_TOPOLOGY_LINELIST: topo = VK_PRIMITIVE_TOPOLOGY_LINE_LIST; break;
    case D3D10_PRIMITIVE_TOPOLOGY_LINESTRIP: topo = VK_PRIMITIVE_TOPOLOGY_LINE_STRIP; break;
    case D3D10_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP: topo = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP; break;
    default: break;
    }
    VkPipelineInputAssemblyStateCreateInfo ia = { VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO, NULL, 0, topo,
                                                  topo == VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP || topo == VK_PRIMITIVE_TOPOLOGY_LINE_STRIP };
    VkPipelineViewportStateCreateInfo vp = { VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, NULL, 0, 1, NULL, 1, NULL };
    VkPipelineRasterizationStateCreateInfo rs = { VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
    rs.depthClampEnable = !k.raster.depth_clip && gpu_features.depthClamp;
    rs.polygonMode = k.raster.fill == D3D10_FILL_WIREFRAME && gpu_features.fillModeNonSolid ? VK_POLYGON_MODE_LINE : VK_POLYGON_MODE_FILL;
    rs.cullMode = k.raster.cull == D3D10_CULL_FRONT ? VK_CULL_MODE_FRONT_BIT : k.raster.cull == D3D10_CULL_BACK ? VK_CULL_MODE_BACK_BIT : VK_CULL_MODE_NONE;
    rs.frontFace = k.raster.front_ccw ? VK_FRONT_FACE_COUNTER_CLOCKWISE : VK_FRONT_FACE_CLOCKWISE;
    rs.depthBiasEnable = k.raster.depth_bias || k.raster.slope_scaled_depth_bias != 0.0f;
    rs.depthBiasConstantFactor = (float)k.raster.depth_bias;
    rs.depthBiasClamp = gpu_features.depthBiasClamp ? k.raster.depth_bias_clamp : 0.0f;
    rs.depthBiasSlopeFactor = k.raster.slope_scaled_depth_bias;
    rs.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo ms = { VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    ms.pSampleMask = &k.sample_mask;
    VkStencilOpState front = { vk_stencil_op(k.depth.front.fail), vk_stencil_op(k.depth.front.pass), vk_stencil_op(k.depth.front.depth_fail),
                               vk_compare(k.depth.front.func), k.depth.stencil_read_mask, k.depth.stencil_write_mask, 0 };
    VkStencilOpState back = { vk_stencil_op(k.depth.back.fail), vk_stencil_op(k.depth.back.pass), vk_stencil_op(k.depth.back.depth_fail),
                              vk_compare(k.depth.back.func), k.depth.stencil_read_mask, k.depth.stencil_write_mask, 0 };
    VkPipelineDepthStencilStateCreateInfo dss = { VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO, NULL, 0,
        (VkBool32)(k.depth.depth_enable != 0), (VkBool32)(k.depth.depth_enable && k.depth.depth_write_mask), vk_compare(k.depth.depth_func),
        VK_FALSE, (VkBool32)(k.depth.stencil_enable && stencil), front, back, 0.0f, 1.0f };
    /* BIOSHOCK_R10_DEBUG (QA profiles' perf-flags.env): 1 no depth test, 2 no culling. */
    static int debug = -1;
    if (debug < 0) { const char *e = getenv("BIOSHOCK_R10_DEBUG"); debug = e ? atoi(e) : 0; if (debug) R10_LOG("Vulkan: debug flags %d", debug); }
    if (debug & 1) dss.depthCompareOp = VK_COMPARE_OP_ALWAYS;
    if (debug & 2) rs.cullMode = VK_CULL_MODE_NONE;
    static const uint32_t all_samples = 0xFFFFFFFFu;
    if (debug & 16) ms.pSampleMask = &all_samples;
    VkPipelineColorBlendAttachmentState att[8];
    for (uint32_t i = 0; i < 8; ++i) {
        const r10_blend_desc *b = &k.blend;
        att[i].blendEnable = b->enable[i] != 0;
        att[i].srcColorBlendFactor = vk_blend(b->src, 0);
        att[i].dstColorBlendFactor = vk_blend(b->dst, 0);
        att[i].colorBlendOp = vk_blend_op(b->op);
        att[i].srcAlphaBlendFactor = vk_blend(b->src_alpha, 1);
        att[i].dstAlphaBlendFactor = vk_blend(b->dst_alpha, 1);
        att[i].alphaBlendOp = vk_blend_op(b->op_alpha);
        att[i].colorWriteMask = b->mask[i] & 0xF;
        if (!gpu_features.independentBlend && i) att[i] = att[0];
    }
    VkPipelineColorBlendStateCreateInfo cb = { VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO, NULL, 0, VK_FALSE, VK_LOGIC_OP_COPY,
                                               k.pass.color_count, att, { 0, 0, 0, 0 } };
    VkDynamicState dyn[4] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR, VK_DYNAMIC_STATE_BLEND_CONSTANTS, VK_DYNAMIC_STATE_STENCIL_REFERENCE };
    VkPipelineDynamicStateCreateInfo ds = { VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO, NULL, 0, 4, dyn };
    VkGraphicsPipelineCreateInfo pi = { VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, NULL, 0, fs ? 2u : 1u, stages, &vi, &ia, NULL, &vp, &rs, &ms,
                                        &dss, &cb, &ds, p->layout, pass, 0, VK_NULL_HANDLE, -1 };
    VkPipeline pipeline = VK_NULL_HANDLE;
    uint64_t start = r10_now_ns();
    void_machine_lend_begin();
    VkResult r = vkCreateGraphicsPipelines(device, pipeline_cache, 1, &pi, NULL, &pipeline);
    void_machine_lend_end();
    void_perf_add(VOID_PERF_PROGRAM_LINK, r10_now_ns() - start, 0);
    __atomic_add_fetch(&pipelines_since_save, 1, __ATOMIC_RELAXED);
    if (r != VK_SUCCESS && r != VK_INCOMPLETE) {
        static unsigned logged;
        if (logged++ < 16) R10_LOG("Vulkan: pipeline of %016llx + %016llx failed: %d", (unsigned long long)p->vs->hash,
                                   (unsigned long long)(p->ps ? p->ps->hash : 0), r);
        pipeline = VK_NULL_HANDLE;
    }
    return pipeline;
}

static VkPipeline pipeline_for(r10_device *d, vk_program *p)
{
    r10_state *st = &d->st;
    pipe_key k;
    memset(&k, 0, sizeof k);
    k.layout_id = st->layout ? st->layout->obj.id : 0;
    k.topology = st->topology;
    if (st->layout) for (int i = 0; i < R10_VB_SLOTS; ++i) if (st->layout->slot_mask & (1u << i)) k.strides[i] = st->vb[i].stride;
    k.blend = st->blend ? st->blend->d.blend : default_blend;
    k.depth = st->depth ? st->depth->d.depth : default_depth;
    k.raster = st->raster ? st->raster->d.raster : default_raster;
    k.pass = targets_key(cur.rtv, cur.rtv_count, cur.dsv, 0);
    k.sample_mask = st->sample_mask;
    if (!cur.dsv) { k.depth.depth_enable = 0; k.depth.depth_write_mask = 0; k.depth.stencil_enable = 0; }
    /* Most draws repeat their program's previous state: compare with it before hashing.  The key was hashed one
     * byte per step before (296 multiplies per draw, the hottest loop of vk_draw on the Adreno 618's CPU). */
    if (p->last && !memcmp(p->last->key, &k, sizeof k)) return p->last->pipeline;
    uint64_t h64 = 0x9E3779B97F4A7C15ull;
    const uint8_t *kb = (const uint8_t *)&k;
    size_t i = 0;
    for (; i + 8 <= sizeof k; i += 8) { uint64_t w; memcpy(&w, kb + i, 8); h64 = (h64 ^ w) * 0xFF51AFD7ED558CCDull; h64 ^= h64 >> 32; }
    for (; i < sizeof k; ++i) { h64 = (h64 ^ kb[i]) * 0x100000001B3ull; }
    uint32_t hash = (uint32_t)(h64 ^ (h64 >> 29));
    for (vk_pipeline *e = p->pipelines[hash & 63]; e; e = e->next)
        if (e->hash == hash && !memcmp(e->key, &k, sizeof k)) { p->last = e; return e->pipeline; }
    VkPipeline pipeline = compile_pipeline(p, &k, st->layout, cur.pass, cur.dsv && cur.dsv->fmt->stencil);
    if (pipeline) trace_warm_pipeline(d, p, &k);
    ++d->stats.pipelines;
    vk_pipeline *e = (vk_pipeline *)calloc(1, sizeof *e);
    memcpy(e->key, &k, sizeof k);
    e->key_size = sizeof k;
    e->hash = hash;
    e->pipeline = pipeline;
    e->next = p->pipelines[hash & 63];
    p->pipelines[hash & 63] = e;
    p->last = e;
    return pipeline;
}
_Static_assert(sizeof(pipe_key) <= 320, "pipeline key fits its slot");
_Static_assert(sizeof(warm_pipeline) == 1088, "warmup asset record layout");

static int warm_pipeline_recipe(warm_pipeline *recipe)
{
    const r10_lib_shader *vs_lib = r10_shaderlib_by_hash(recipe->vs, 1);
    const r10_lib_shader *ps_lib = r10_shaderlib_by_hash(recipe->ps, 0);
    pass_key *pass = &recipe->state.pass;
    const VkPhysicalDeviceLimits *limits = &gpu_props.limits;
    if (!vs_lib || (recipe->ps && !ps_lib) || recipe->element_count > 32 ||
        recipe->element_count > limits->maxVertexInputAttributes ||
        pass->color_count > 8 || pass->color_count > limits->maxColorAttachments) return 0;
    r10_layout layout = {0};
    layout.count = recipe->element_count;
    for (uint32_t i = 0; i < layout.count; ++i) {
        r10_element e = recipe->elements[i];
        if (e.slot >= R10_VB_SLOTS || e.slot >= limits->maxVertexInputBindings ||
            e.location >= limits->maxVertexInputAttributes ||
            e.offset > limits->maxVertexInputAttributeOffset ||
            recipe->state.strides[e.slot] > limits->maxVertexInputBindingStride ||
            !r10_format_get(e.format)) return 0;
        layout.slot_mask |= 1u << e.slot;
        layout.el[i] = e;
    }
    for (uint32_t i = 0; i < pass->color_count; ++i)
        if (pass->color[i]) pass->color[i] = color_target_format((VkFormat)pass->color[i]);
    if (pass->depth == VK_FORMAT_D24_UNORM_S8_UINT && !d24s8_ok) pass->depth = VK_FORMAT_D32_SFLOAT_S8_UINT;
    vk_shader vs_backend = {0}, ps_backend = {0};
    r10_shader vs = {.type = 1, .lib = vs_lib, .hash = recipe->vs, .be = &vs_backend};
    r10_shader ps = {.type = 0, .lib = ps_lib, .hash = recipe->ps, .be = &ps_backend};
    vk_program p = {.vs = &vs, .ps = ps_lib ? &ps : NULL};
    program_layout(&p);
    VkPipeline pipeline = compile_pipeline(&p, &recipe->state, layout.count ? &layout : NULL, render_pass(pass), recipe->stencil);
    if (pipeline) vkDestroyPipeline(device, pipeline, NULL);
    if (p.layout) vkDestroyPipelineLayout(device, p.layout, NULL);
    if (p.set_layout) vkDestroyDescriptorSetLayout(device, p.set_layout, NULL);
    if (vs_backend.module) { vkDestroyShaderModule(device, vs_backend.module, NULL); --shader_modules; }
    if (ps_backend.module) { vkDestroyShaderModule(device, ps_backend.module, NULL); --shader_modules; }
    return pipeline != VK_NULL_HANDLE;
}

static void warm_pipelines(void)
{
    r10_warmup seed;
    if ((debug_flags() & (R10_DEBUG_RECORD_WARMUP | R10_DEBUG_NO_WARMUP)) ||
        !r10_shaderlib_warmup(&seed) || seed.pipeline_stride != sizeof(warm_pipeline)) return;
    uint64_t start = r10_now_ns();
    uint32_t ready = 0;
    for (uint32_t i = 0; i < seed.pipeline_count; ++i) {
        warm_pipeline recipe;
        memcpy(&recipe, seed.pipelines + i * sizeof recipe, sizeof recipe);
        ready += (uint32_t)warm_pipeline_recipe(&recipe);
        if (r10_now_ns() - start >= 4000000000ull) break;
    }
    R10_LOG("Vulkan: warmed %u/%u pipelines during startup in %.1f ms", ready, seed.pipeline_count, (r10_now_ns() - start) / 1e6);
}


/* ------------------------------------------------------------------ draws */
static VkDescriptorSet allocate_set(VkDescriptorSetLayout layout)
{
    vk_frame *f = &frames[frame_index];
    for (;;) {
        if (f->dpool_next == f->dpool_count) {
            if (f->dpool_count == 64) { R10_LOG("Vulkan: descriptor pools exhausted"); return VK_NULL_HANDLE; }
            /* COMBINED_IMAGE_SAMPLER: the present pass's set comes from these pools as well. */
            VkDescriptorPoolSize sizes[6] = { { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 4096 }, { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 2048 },
                                              { VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 8192 }, { VK_DESCRIPTOR_TYPE_SAMPLER, 8192 },
                                              { VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER, 256 }, { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 64 } };
            VkDescriptorPoolCreateInfo ci = { VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, NULL, 0, 2048, 6, sizes };
            if (vkCreateDescriptorPool(device, &ci, NULL, &f->dpools[f->dpool_count]) != VK_SUCCESS) return VK_NULL_HANDLE;
            ++f->dpool_count;
        }
        VkDescriptorSetAllocateInfo ai = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, NULL, f->dpools[f->dpool_next], 1, &layout };
        VkDescriptorSet set;
        VkResult r = vkAllocateDescriptorSets(device, &ai, &set);
        if (r == VK_SUCCESS) return set;
        ++f->dpool_next;
    }
}

static int srgb_wanted(void) { return !void_bioshock_srgb_textures || void_bioshock_srgb_textures(); }

/* A dynamic buffer's bytes [lo, hi) in its ring slice of the current WRITE_DISCARD version. */
static int stream_buffer(r10_resource *r, uint32_t lo, uint32_t hi, vk_slice *out)
{
    vk_buffer *b = buf(r);
    const uint8_t *src = r10_buffer_shadow(r);
    if (hi > r->desc.width) hi = r->desc.width;
    if (lo >= hi) lo = hi = 0;
    uint64_t frame = r10_dev->frame;
    if (b->slice_version != r->version || b->slice_frame != frame || !b->slice.buffer) {
        /* A slice lives for its frame: a version still in use later is copied again into this frame's ring. */
        int same = b->slice_version == r->version && b->slice.buffer;
        uint32_t keep_lo = b->slice_lo, keep_hi = b->slice_hi;
        if (!ring_alloc(r->desc.width, 256, &b->slice)) return 0;
        b->slice_frame = frame;
        b->slice_lo = b->slice_hi = 0;
        if (same && src && keep_hi > keep_lo) { memcpy(b->slice.map + keep_lo, src + keep_lo, keep_hi - keep_lo); b->slice_lo = keep_lo; b->slice_hi = keep_hi; }
        b->slice_version = r->version;
    }
    if (src && hi > lo) {
        if (b->slice_hi <= b->slice_lo) { memcpy(b->slice.map + lo, src + lo, hi - lo); b->slice_lo = lo; b->slice_hi = hi; }
        else {
            if (lo < b->slice_lo) { memcpy(b->slice.map + lo, src + lo, b->slice_lo - lo); b->slice_lo = lo; }
            if (hi > b->slice_hi) { memcpy(b->slice.map + b->slice_hi, src + b->slice_hi, hi - b->slice_hi); b->slice_hi = hi; }
        }
        r10_dev->stats.uploads_kb += (hi - lo) >> 10;
    }
    *out = b->slice;
    return 1;
}

static int cb_slice(r10_resource *cb, vk_slice *out)
{
    vk_buffer *b = buf(cb);
    const uint8_t *data = b->data ? b->data : r10_buffer_shadow(cb);
    if (!data) return 0;
    uint64_t version = b->data ? b->data_version : cb->version;
    if (b->cb_version != version || b->cb_frame != r10_dev->frame || !b->cb_slice.buffer) {
        if (!ring_alloc(cb->desc.width, (uint32_t)gpu_props.limits.minUniformBufferOffsetAlignment, &b->cb_slice)) return 0;
        memcpy(b->cb_slice.map, data, cb->desc.width);
        b->cb_version = version;
        b->cb_frame = r10_dev->frame;
    }
    *out = b->cb_slice;
    return 1;
}

static void prepare_srv(r10_view *v)
{
    if (!v) return;
    vk_image *im = img(v->res);
    if (!im || !im->image) return;
    keep_depth_copy(im->image);
    pending_flush_resource(v->res);
    if (im->layout != read_layout(im)) {
        if (image_in_pass(im)) {
            static unsigned logged;
            if (logged++ < 8) R10_LOG("Vulkan: a shader reads a target of the current pass (%s)", v->fmt->name);
            return;
        }
        end_pass();
        transition(cmd, im, read_layout(im));
    }
    im->gpu_use = submit_serial;
}

/* The pipeline vk_draw last bound in command buffer bound_cmd (other passes reset it). */
static VkPipeline bound;
static VkCommandBuffer bound_cmd;
static void probe_draw(r10_device *d);
static void probe_report(void);
static VkBuffer probe_vb, probe_ib;
static VkDeviceSize probe_vb_at, probe_ib_at;
static uint32_t probe_vb_stride, probe_ib_size;

/* A draw that cannot be recorded: counted, and named in traced frames. */
#define DRAW_SKIP(why) do { static unsigned skips; if (d->tracing || skips++ < 8) R10_LOG("Vulkan: draw %u skipped: %s", d->stats.draws, why); } while (0)

static void vk_draw_inner(r10_device *d, const r10_draw *dr);
/* The CPU time of draws and descriptor sets is measured only with BIOSHOCK_R10_DEBUG 4194304: two clock reads per
 * draw were 3 % of the game thread's samples on the Dimensity 8300 (the statistics line shows 0 ms otherwise). */
static void vk_draw(r10_device *d, const r10_draw *dr)
{
    if (!(debug_flags() & 4194304)) { vk_draw_inner(d, dr); return; }
    uint64_t start = r10_now_ns();
    vk_draw_inner(d, dr);
    d->stats.draw_ns += (uint32_t)(r10_now_ns() - start);
}
static void vk_draw_inner(r10_device *d, const r10_draw *dr)
{
    work_recorded = 1;
    r10_state *st = &d->st;
    r10_shader *vs = st->vs.shader, *ps = st->ps.shader;
    if (!vs->be || (ps && !ps->be)) { DRAW_SKIP("no shader module"); return; }
    vk_program *p = program_for(vs, ps);
    /* A pass of other targets ends first; shader resources must be readable before the pass starts. */
    int same = cur.active && cur.dsv == st->dsv && cur.rtv_count == st->rtv_count && !memcmp(cur.rtv, st->rtv, st->rtv_count * sizeof *st->rtv);
    if (!same) end_pass();
    for (uint32_t i = 0; i < p->binding_count; ++i) {
        if (p->bindings[i].type != VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE) continue;
        r10_stage_state *stage = p->bindings[i].stage ? &st->vs : &st->ps;
        if (p->bindings[i].slot < R10_SRV_SLOTS) prepare_srv(stage->srv[p->bindings[i].slot]);
    }
    if (!cur.active) {
        if (!begin_pass(st->rtv, st->rtv_count, st->dsv)) { DRAW_SKIP("render pass"); return; }
        st->dirty |= R10_DIRTY_VIEWPORT | R10_DIRTY_SCISSOR | R10_DIRTY_BLEND_FACTOR | R10_DIRTY_STENCIL_REF | R10_DIRTY_SHADERS;
    }
    if (cur.limited) {
        /* the draw's reach: its viewport, cut by an enabled scissor */
        const r10_raster_desc *r = st->raster ? &st->raster->d.raster : &default_raster;
        int64_t x1 = st->viewports ? (int64_t)st->viewport[0].x + st->viewport[0].w : (int64_t)r10_area.full_w;
        int64_t y1 = st->viewports ? (int64_t)st->viewport[0].y + st->viewport[0].h : (int64_t)r10_area.full_h;
        if (r->scissor && st->scissors) {
            if (st->scissor[0].r < x1) x1 = st->scissor[0].r;
            if (st->scissor[0].b < y1) y1 = st->scissor[0].b;
        }
        if (x1 > (int64_t)cur.width || y1 > (int64_t)cur.height) {
            if (area_restarts++ < 8 || d->tracing)
                R10_LOG("Vulkan: draw %u reaches %lldx%lld outside the scene area %ux%u: the pass restarts on the whole target",
                        d->stats.draws, (long long)x1, (long long)y1, cur.width, cur.height);
            area_full_next = 1;
            if (!begin_pass(st->rtv, st->rtv_count, st->dsv)) { DRAW_SKIP("render pass"); return; }
            st->dirty |= R10_DIRTY_VIEWPORT | R10_DIRTY_SCISSOR | R10_DIRTY_BLEND_FACTOR | R10_DIRTY_STENCIL_REF | R10_DIRTY_SHADERS;
        }
    }
    VkPipeline pipeline = pipeline_for(d, p);
    if (!pipeline) { DRAW_SKIP("pipeline"); return; }
    if (bound_cmd != cmd) { bound = VK_NULL_HANDLE; bound_cmd = cmd; st->dirty |= R10_DIRTY_VIEWPORT | R10_DIRTY_SCISSOR | R10_DIRTY_BLEND_FACTOR | R10_DIRTY_STENCIL_REF; }
    if (pipeline != bound) { vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline); bound = pipeline; }
    if (st->dirty & R10_DIRTY_VIEWPORT) {
        const r10_viewport *v = &st->viewport[0];
        VkViewport vp = { (float)v->x, (float)v->y + (float)v->h, (float)v->w, -(float)v->h, v->minz, v->maxz };
        if (!st->viewports) { vp.x = 0; vp.y = (float)cur.height; vp.width = (float)cur.width; vp.height = -(float)cur.height; vp.minDepth = 0; vp.maxDepth = 1; }
        vkCmdSetViewport(cmd, 0, 1, &vp);
    }
    if (st->dirty & (R10_DIRTY_SCISSOR | R10_DIRTY_RASTER | R10_DIRTY_VIEWPORT)) {
        const r10_raster_desc *r = st->raster ? &st->raster->d.raster : &default_raster;
        VkRect2D sc = { { 0, 0 }, { cur.width, cur.height } };
        if (r->scissor && st->scissors) {
            const r10_rect *s = &st->scissor[0];
            int32_t l = s->l < 0 ? 0 : s->l, t = s->t < 0 ? 0 : s->t, rr = s->r, b = s->b;
            if (rr > (int32_t)cur.width) rr = (int32_t)cur.width;
            if (b > (int32_t)cur.height) b = (int32_t)cur.height;
            sc.offset.x = l; sc.offset.y = t;
            sc.extent.width = rr > l ? (uint32_t)(rr - l) : 0; sc.extent.height = b > t ? (uint32_t)(b - t) : 0;
        }
        vkCmdSetScissor(cmd, 0, 1, &sc);
    }
    if (st->dirty & R10_DIRTY_BLEND_FACTOR) vkCmdSetBlendConstants(cmd, st->blend_factor);
    if (st->dirty & R10_DIRTY_STENCIL_REF) vkCmdSetStencilReference(cmd, VK_STENCIL_FACE_FRONT_AND_BACK, st->stencil_ref);
    /* Descriptors */
    if (p->binding_count) {
        VkWriteDescriptorSet writes[96];
        VkDescriptorBufferInfo binfo[96];
        VkDescriptorImageInfo iinfo[96];
        uint32_t offsets[96], noffsets = 0, nw = 0;
        uint64_t h = 1469598103934665603ull;
        int srgb = srgb_wanted();
        for (uint32_t i = 0; i < p->binding_count; ++i) {
            const vk_binding *bd = &p->bindings[i];
            r10_stage_state *stage = bd->stage ? &st->vs : &st->ps;
            r10_shader *shader = bd->stage ? vs : ps;
            VkWriteDescriptorSet w = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, NULL, VK_NULL_HANDLE, bd->binding, 0, 1, (VkDescriptorType)bd->type, NULL, NULL, NULL };
            if (bd->type == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC || bd->type == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER) {
                vk_slice s = { dummy_ubo, 0, NULL };
                VkDeviceSize range = 65536;
                if (bd->slot == 14 && shader->lib->icb_size) { s.buffer = ((vk_shader *)shader->be)->icb; range = shader->lib->icb_size; }
                else if (bd->slot < R10_CB_SLOTS && stage->cb[bd->slot] && cb_slice(stage->cb[bd->slot], &s)) range = stage->cb[bd->slot]->desc.width;
                if (range > gpu_props.limits.maxUniformBufferRange) range = gpu_props.limits.maxUniformBufferRange;
                if (bd->type == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC) { binfo[i] = (VkDescriptorBufferInfo){ s.buffer, 0, range }; offsets[noffsets++] = s.offset; }
                else binfo[i] = (VkDescriptorBufferInfo){ s.buffer, s.offset, range };
                w.pBufferInfo = &binfo[i];
                h ^= (uint64_t)s.buffer + (bd->type == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER ? s.offset : 0) + range * 31; h *= 1099511628211ull;
            } else if (bd->type == VK_DESCRIPTOR_TYPE_SAMPLER) {
                r10_state_object *smp = bd->slot < R10_SAMPLER_SLOTS ? stage->sampler[bd->slot] : NULL;
                iinfo[i] = (VkDescriptorImageInfo){ smp && smp->be ? ((vk_sampler *)smp->be)->sampler : dummy_sampler, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED };
                w.pImageInfo = &iinfo[i];
                h ^= (uint64_t)iinfo[i].sampler; h *= 1099511628211ull;
            } else if (bd->type == VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE) {
                r10_view *v = bd->slot < R10_SRV_SLOTS ? stage->srv[bd->slot] : NULL;
                vk_view *bv = view_current(v);
                VkImageView view = bv && bv->view ? (!srgb && bv->view_unorm ? bv->view_unorm : bv->view) : VK_NULL_HANDLE;
                VkImageLayout layout = v && img(v->res) ? read_layout(img(v->res)) : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
                if (view && bv->type != (VkImageViewType)bd->view_type) {
                    static unsigned logged;
                    if (logged++ < 8) R10_LOG("Vulkan: view type %d bound where the shader declares %u", bv->type, bd->view_type);
                    view = VK_NULL_HANDLE;
                }
                if (view && img(v->res)->layout != layout) {   /* a target of this pass */
                    static unsigned logged;
                    if (logged++ < 8) R10_LOG("Vulkan: %s texture %ux%u in layout %d is not readable here", v->fmt->name, v->res->desc.width,
                                              v->res->desc.height, img(v->res)->layout);
                    view = VK_NULL_HANDLE;
                }
                if (!view) { view = dummy_view[bd->view_type < 8 ? bd->view_type : 1] ? dummy_view[bd->view_type < 8 ? bd->view_type : 1] : dummy_view[1]; layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL; }
                iinfo[i] = (VkDescriptorImageInfo){ VK_NULL_HANDLE, view, layout };
                w.pImageInfo = &iinfo[i];
                h ^= (uint64_t)view; h *= 1099511628211ull;
            } else {
                continue;   /* texel buffers are not used by the game */
            }
            writes[nw++] = w;
        }
        if (p->set_frame != d->frame + 1 || p->set_hash != h || !p->set) {
            uint64_t set_start = (debug_flags() & 4194304) ? r10_now_ns() : 0;
            VkDescriptorSet set = allocate_set(p->set_layout);
            if (!set) { DRAW_SKIP("descriptor set"); return; }
            for (uint32_t i = 0; i < nw; ++i) writes[i].dstSet = set;
            vkUpdateDescriptorSets(device, nw, writes, 0, NULL);
            if (set_start) d->stats.set_ns += (uint32_t)(r10_now_ns() - set_start);
            ++d->stats.sets; d->stats.set_writes += nw;
            p->set = set;
            p->set_hash = h;
            p->set_frame = d->frame + 1;
        }
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, p->layout, 0, 1, &p->set, noffsets, offsets);
    }
    /* Vertex buffers */
    r10_layout *l = st->layout;
    if (l) {
        for (uint32_t slot = 0; slot < R10_VB_SLOTS; ++slot) {
            if (!(l->slot_mask & (1u << slot))) continue;
            r10_resource *vb = st->vb[slot].buf;
            uint32_t stride = st->vb[slot].stride, offset = st->vb[slot].offset;
            VkBuffer handle = dummy_ubo;
            VkDeviceSize at = 0;
            if (vb) {
                vk_buffer *b = buf(vb);
                if (b->buffer && !vb->discards) { handle = b->buffer; at = offset; b->gpu_use = submit_serial; }
                else {
                    int instanced = 0;
                    for (uint32_t i = 0; i < l->count; ++i) if (l->el[i].slot == slot && l->el[i].instanced) instanced = 1;
                    uint32_t lo, hi;
                    if (instanced) { lo = offset; hi = offset + (dr->first_instance + dr->instances) * stride; }
                    else if (dr->vertex_hi == 0xFFFFFFFFu) { lo = offset; hi = vb->desc.width; }
                    else { lo = offset + dr->vertex_lo * stride; hi = offset + dr->vertex_hi * stride; }
                    vk_slice s;
                    if (!stream_buffer(vb, lo, hi, &s)) { DRAW_SKIP("vertex stream"); return; }
                    handle = s.buffer;
                    at = s.offset + offset;
                }
            }
            vkCmdBindVertexBuffers(cmd, slot, 1, &handle, &at);
            if (slot == 0) { probe_vb = handle; probe_vb_at = at; probe_vb_stride = stride; }
        }
    }
    if (dr->indexed) {
        r10_resource *ib = st->ib;
        if (!ib) { DRAW_SKIP("no index buffer"); return; }
        vk_buffer *b = buf(ib);
        int wide = st->ib_format == 42;
        uint32_t isize = wide ? 4 : 2;
        VkBuffer handle;
        VkDeviceSize at;
        if (b->buffer && !ib->discards) { handle = b->buffer; at = st->ib_offset; b->gpu_use = submit_serial; }
        else {
            uint32_t lo = st->ib_offset + dr->first * isize;
            vk_slice s;
            if (!stream_buffer(ib, lo, lo + dr->count * isize, &s)) { DRAW_SKIP("index stream"); return; }
            handle = s.buffer;
            at = s.offset + st->ib_offset;
        }
        vkCmdBindIndexBuffer(cmd, handle, at, wide ? VK_INDEX_TYPE_UINT32 : VK_INDEX_TYPE_UINT16);
        probe_ib = handle; probe_ib_at = at; probe_ib_size = isize;
        vkCmdDrawIndexed(cmd, dr->count, dr->instances, dr->first, dr->base_vertex, dr->first_instance);
    } else {
        vkCmdDraw(cmd, dr->count, dr->instances, dr->first, dr->first_instance);
    }
    st->dirty = 0;
    if (d->tracing && (debug_flags() & 512)) probe_draw(d);   /* probes split passes: opt-in */
}

/* ------------------------------------------------------------------ trace probes */
/* In a traced frame every draw is followed by copies of five texels (centre and quarter points) of
 * render target 0 and of the depth buffer into a host buffer; after the frame completes they are logged
 * as "PROBE draw <n> <rt|ds> <format> <hex texels>" (tools decode the formats). */
#define PROBE_POINTS 32   /* an 8x4 grid of texel centres */
typedef struct probe_rec { uint32_t draw, format, bytes, offset; char what; uint8_t expect[16]; } probe_rec;
static VkBuffer probe_buffer;
static vk_alloc probe_mem;
static probe_rec probe_recs[1024];
static uint32_t probe_count, probe_used;

static int probe_buffer_ok(void);
static uint32_t texel_bytes(VkFormat f)
{
    switch (f) {
    case VK_FORMAT_R8_UNORM: return 1;
    case VK_FORMAT_R16G16B16A16_SFLOAT: case VK_FORMAT_R32G32_SFLOAT: case VK_FORMAT_R16G16B16A16_UNORM: return 8;
    case VK_FORMAT_R32G32B32A32_SFLOAT: return 16;
    case VK_FORMAT_R16_SFLOAT: case VK_FORMAT_R16_UNORM: case VK_FORMAT_R8G8_UNORM: return 2;
    default: return 4;
    }
}

static int probe_buffer_ok(void)
{
    if (!probe_buffer) {
        VkBufferCreateInfo bi = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, NULL, 0, 1 << 20, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                  VK_SHARING_MODE_EXCLUSIVE, 0, NULL };
        if (vkCreateBuffer(device, &bi, NULL, &probe_buffer) != VK_SUCCESS) return 0;
        VkMemoryRequirements req;
        vkGetBufferMemoryRequirements(device, probe_buffer, &req);
        if (!mem_alloc(req, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, 1, MK_OTHER, &probe_mem)) return 0;
        vkBindBufferMemory(device, probe_buffer, probe_mem.block->memory, probe_mem.offset);
    }
    return probe_count < 1024 && probe_used + PROBE_POINTS * 16 <= (1u << 20);
}

static void probe_image(vk_image *im, uint32_t draw, char what, uint32_t mip, uint32_t layer)
{
    if (!probe_buffer) {
        VkBufferCreateInfo bi = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, NULL, 0, 1 << 20, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                  VK_SHARING_MODE_EXCLUSIVE, 0, NULL };
        if (vkCreateBuffer(device, &bi, NULL, &probe_buffer) != VK_SUCCESS) return;
        VkMemoryRequirements req;
        vkGetBufferMemoryRequirements(device, probe_buffer, &req);
        if (!mem_alloc(req, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, 1, MK_OTHER, &probe_mem)) return;
        vkBindBufferMemory(device, probe_buffer, probe_mem.block->memory, probe_mem.offset);
    }
    if (probe_count >= 1024 || probe_used + PROBE_POINTS * 16 > (1u << 20)) return;
    uint32_t w = im->width >> mip ? im->width >> mip : 1, h = im->height >> mip ? im->height >> mip : 1;
    uint32_t bytes = im->depth_kind ? 4 : texel_bytes(im->format);
    uint32_t px[PROBE_POINTS][2];
    for (int i = 0; i < PROBE_POINTS; ++i) { px[i][0] = (2 * (i % 8) + 1) * w / 16; px[i][1] = (2 * (i / 8) + 1) * h / 8; }
    end_pass();
    VkImageLayout old = im->layout;
    transition(cmd, im, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    probe_rec *p = &probe_recs[probe_count++];
    p->draw = draw; p->format = (uint32_t)im->format; p->bytes = bytes; p->offset = probe_used; p->what = what;
    for (int i = 0; i < PROBE_POINTS; ++i) {
        VkBufferImageCopy c = { probe_used, 0, 0, { im->depth_kind ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT, mip, layer, 1 },
                                { (int32_t)px[i][0], (int32_t)px[i][1], 0 }, { 1, 1, 1 } };
        vkCmdCopyImageToBuffer(cmd, im->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, probe_buffer, 1, &c);
        probe_used += 16;
    }
    transition(cmd, im, old == VK_IMAGE_LAYOUT_UNDEFINED ? read_layout(im) : old);
}

static void probe_bytes(VkBuffer from, VkDeviceSize at, uint32_t draw, char what, const uint8_t *expect)
{
    if (!probe_buffer_ok() || !from) return;
    end_pass();
    probe_rec *p = &probe_recs[probe_count++];
    p->draw = draw; p->format = 0; p->bytes = 16; p->offset = probe_used; p->what = what;
    memcpy(p->expect, expect, 16);
    VkBufferCopy c = { at, probe_used, 16 };
    vkCmdCopyBuffer(cmd, from, probe_buffer, 1, &c);
    probe_used += PROBE_POINTS * 16;
}

static void probe_draw(r10_device *d)
{
    r10_state *st = &d->st;
    if (probe_ib && st->ib && r10_buffer_shadow(st->ib)) {
        const uint8_t *ishadow = r10_buffer_shadow(st->ib);
        uint32_t first_byte = (uint32_t)st->ib_offset + d->probe_first * probe_ib_size;
        uint8_t expect[16] = { 0 };
        if (first_byte + 16 <= st->ib->desc.width) memcpy(expect, ishadow + first_byte, 16);
        probe_bytes(probe_ib, probe_ib_at + d->probe_first * probe_ib_size, d->stats.draws, 'i', expect);
        uint32_t index = probe_ib_size == 4 ? *(const uint32_t *)expect : *(const uint16_t *)expect;
        r10_resource *vb = st->vb[0].buf;
        if (probe_vb && vb && r10_buffer_shadow(vb)) {
            uint64_t vbyte = (uint64_t)st->vb[0].offset + (uint64_t)((int64_t)index + d->probe_base) * probe_vb_stride;
            uint8_t vexp[16] = { 0 };
            if (vbyte + 16 <= vb->desc.width) memcpy(vexp, r10_buffer_shadow(vb) + vbyte, 16);
            probe_bytes(probe_vb, probe_vb_at + (VkDeviceSize)((int64_t)index + d->probe_base) * probe_vb_stride, d->stats.draws, 'v', vexp);
        }
    }
    probe_ib = probe_vb = VK_NULL_HANDLE;
    /* Shader resources: once per image and frame, those copyable texel by texel. */
    static VkImage seen[256];
    static uint32_t seen_count;
    static uint64_t seen_frame = ~0ull;
    if (seen_frame != d->frame) { seen_frame = d->frame; seen_count = 0; }
    for (uint32_t i = 0; i < 4; ++i) {
        r10_view *v = st->ps.srv[i];
        vk_image *im = v ? img(v->res) : NULL;
        if (!im || !im->image || (v->res->fmt->kind == R10_FMT_BC && !im->decoded) || im->depth_kind) continue;
        int dup = 0;
        for (uint32_t k = 0; k < seen_count; ++k) if (seen[k] == im->image) dup = 1;
        if (dup || seen_count >= 256) continue;
        seen[seen_count++] = im->image;
        probe_image(im, d->stats.draws, (char)('0' + i), v->first_mip, v->first_layer);
    }
    if (st->rtv_count && st->rtv[0] && img(st->rtv[0]->res) && img(st->rtv[0]->res)->image)
        probe_image(img(st->rtv[0]->res), d->stats.draws, 'c', st->rtv[0]->first_mip, st->rtv[0]->first_layer);
    if (st->dsv && img(st->dsv->res) && img(st->dsv->res)->image)
        probe_image(img(st->dsv->res), d->stats.draws, 'd', st->dsv->first_mip, st->dsv->first_layer);
}

static void probe_report(void)
{
    if (pass_timer_count) {
        present_drain();
        vkQueueWaitIdle(queue);
        uint64_t t[2 * PASS_TIMERS];
        if (vkGetQueryPoolResults(device, pass_timer_pool, 0, 2 * pass_timer_count, sizeof t, t, sizeof t[0], VK_QUERY_RESULT_64_BIT) == VK_SUCCESS) {
            for (uint32_t i = 0; i < pass_timer_count; ++i) {
                if (pass_info[i].op) {
                    R10_LOG("PASS %u: %s %ux%u: %.3f ms", i, pass_info[i].op, pass_info[i].w, pass_info[i].h,
                            (double)(t[2 * i + 1] - t[2 * i]) * gpu_timer_ns_per_tick / 1e6);
                } else {
                    R10_LOG("PASS %u: color %u depth %u %ux%u load %u/%u draws %u-%u: %.3f ms", i, pass_info[i].color, pass_info[i].depth,
                            pass_info[i].w, pass_info[i].h, pass_info[i].load_color, pass_info[i].load_depth, pass_info[i].first_draw,
                            pass_info[i].last_draw, (double)(t[2 * i + 1] - t[2 * i]) * gpu_timer_ns_per_tick / 1e6);
                }
            }
        }
        pass_timer_count = 0;
    }
    if (!probe_count) return;
    present_drain();
    vkQueueWaitIdle(queue);
    const uint8_t *m = (const uint8_t *)probe_mem.map;
    for (uint32_t i = 0; i < probe_count; ++i) {
        const probe_rec *p = &probe_recs[i];
        char line[1600];
        if (p->what == 'i' || p->what == 'v') {
            char got[40], want[40];
            for (int b = 0; b < 16; ++b) { snprintf(got + 2 * b, 3, "%02x", m[p->offset + b]); snprintf(want + 2 * b, 3, "%02x", p->expect[b]); }
            R10_LOG("PROBE draw %u %s gpu %s cpu %s%s", p->draw, p->what == 'i' ? "ib" : "vb", got, want, strcmp(got, want) ? "  MISMATCH" : "");
            continue;
        }
        char what[4] = { 't', p->what, 0, 0 };
        size_t n = (size_t)snprintf(line, sizeof line, "PROBE draw %u %s fmt %u:", p->draw, p->what == 'c' ? "rt" : p->what == 'd' ? "ds" : what,
                                    p->format);
        for (int k = 0; k < PROBE_POINTS && n < sizeof line - 40; ++k) {
            n += (size_t)snprintf(line + n, sizeof line - n, " ");
            for (uint32_t b = 0; b < p->bytes && b < 16; ++b) n += (size_t)snprintf(line + n, sizeof line - n, "%02x", m[p->offset + 16 * k + b]);
        }
        R10_LOG("%s", line);
    }
    probe_count = 0;
    probe_used = 0;
}

/* ------------------------------------------------------------------ clears */
static void vk_clear_rtv(r10_device *d, r10_view *v, const float color[4])
{
    work_recorded = 1;
    (void)d;
    vk_view *bv = (vk_view *)v->be;
    if (!bv || (!bv->view && !bv->lazy_view)) return;
    VkClearValue value;
    memcpy(value.color.float32, color, 16);
    {
        static int debug = -1;
        if (debug < 0) { const char *e = getenv("BIOSHOCK_R10_DEBUG"); debug = e ? atoi(e) : 0; }
        if ((debug & 8) && !v->res->is_backbuffer) { value.color.float32[0] = 0.5f; value.color.float32[1] = 0.25f; value.color.float32[2] = 0.1f; }
    }
    if (v->fmt->is_int) for (int i = 0; i < 4; ++i) value.color.uint32[i] = (uint32_t)color[i];
    if (is_attachment(v)) {
        uint32_t index = 0;
        for (uint32_t i = 0; i < cur.rtv_count; ++i) { if (cur.rtv[i] == v) break; if (cur.rtv[i]) ++index; }
        VkClearAttachment a = { VK_IMAGE_ASPECT_COLOR_BIT, index, value };
        VkClearRect rect = { { { 0, 0 }, { bv->width < cur.width ? bv->width : cur.width, bv->height < cur.height ? bv->height : cur.height } }, 0, 1 };
        vkCmdClearAttachments(cmd, 1, &a, 1, &rect);
        return;
    }
    bv->pending_clear = 1;
    bv->clear = value;
    bv->clear_aspects = VK_IMAGE_ASPECT_COLOR_BIT;
    pending_add(v);
}

static void vk_clear_dsv(r10_device *d, r10_view *v, uint32_t flags, float depth, uint8_t stencil)
{
    work_recorded = 1;
    (void)d;
    vk_view *bv = (vk_view *)v->be;
    if (!bv || (!bv->view && !bv->lazy_view)) return;
    VkImageAspectFlags aspects = ((flags & D3D10_CLEAR_DEPTH) ? VK_IMAGE_ASPECT_DEPTH_BIT : 0) |
                                 ((flags & D3D10_CLEAR_STENCIL) && v->fmt->stencil ? VK_IMAGE_ASPECT_STENCIL_BIT : 0);
    if (!aspects) return;
    VkClearValue value;
    value.depthStencil.depth = depth;
    value.depthStencil.stencil = stencil;
    if (is_attachment(v)) {
        VkClearAttachment a = { aspects, 0, value };
        VkClearRect rect = { { { 0, 0 }, { bv->width < cur.width ? bv->width : cur.width, bv->height < cur.height ? bv->height : cur.height } }, 0, 1 };
        vkCmdClearAttachments(cmd, 1, &a, 1, &rect);
        return;
    }
    if (bv->pending_clear && bv->clear_aspects != aspects) {
        /* Merge with an earlier clear of the other aspect. */
        if (!(aspects & VK_IMAGE_ASPECT_DEPTH_BIT)) value.depthStencil.depth = bv->clear.depthStencil.depth;
        if (!(aspects & VK_IMAGE_ASPECT_STENCIL_BIT)) value.depthStencil.stencil = bv->clear.depthStencil.stencil;
        aspects |= bv->clear_aspects;
    }
    bv->pending_clear = 1;
    bv->clear = value;
    bv->clear_aspects = aspects;
    pending_add(v);
}

/* ------------------------------------------------------------------ queries */
/* Occlusion queries: one pool per frame slot, reset when the slot is reused; a Direct3D Begin/End may
 * span render passes, so a query is one Vulkan query per pass of its interval, summed. */
typedef struct vk_query {
    uint32_t slots[16]; uint32_t slot_count, frame;
    uint64_t serial;
    int running, ready;          /* ready: 1 result known, 2 no samples were recorded (zero once submitted) */
    struct vk_query *next_pending;
    r10_query *owner;
} vk_query;
static vk_query *pending_queries[FRAMES];

static void harvest(uint32_t frame)
{
    while (pending_queries[frame]) {
        vk_query *vq = pending_queries[frame];
        uint64_t total = 0;
        for (uint32_t i = 0; i < vq->slot_count; ++i) {
            uint64_t v = 0;
            if (vkGetQueryPoolResults(device, occlusion_pools[frame], vq->slots[i], 1, sizeof v, &v, sizeof v, VK_QUERY_RESULT_64_BIT) == VK_SUCCESS) total += v;
        }
        vq->owner->samples = total;
        vq->ready = 1;
        vq->slot_count = 0;
        pending_queries[frame] = vq->next_pending;
        vq->next_pending = NULL;
    }
}

static void unlink_pending(vk_query *vq)
{
    for (int f = 0; f < FRAMES; ++f)
        for (vk_query **p = &pending_queries[f]; *p; p = &(*p)->next_pending)
            if (*p == vq) { *p = vq->next_pending; vq->next_pending = NULL; return; }
}

static int vk_query_create(r10_device *d, r10_query *q)
{
    (void)d;
    vk_query *vq = (vk_query *)calloc(1, sizeof(vk_query));
    vq->owner = q;
    q->be = vq;
    if ((q->type == D3D10_QUERY_OCCLUSION || q->type == D3D10_QUERY_OCCLUSION_PREDICATE) && !occlusion_pools[0]) {
        for (int f = 0; f < FRAMES; ++f) {
            VkQueryPoolCreateInfo ci = { VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO, NULL, 0, VK_QUERY_TYPE_OCCLUSION, QUERY_POOL_SIZE, 0 };
            vkCreateQueryPool(device, &ci, NULL, &occlusion_pools[f]);
            occlusion_used[f] = QUERY_POOL_SIZE;   /* the other slots' pools are reset when their frame comes */
        }
        /* The current slot's pool is reset by the upload commands, which run before the frame's. */
        vkCmdResetQueryPool(uploads(), occlusion_pools[frame_index], 0, QUERY_POOL_SIZE);
        occlusion_used[frame_index] = 0;
    }
    return 1;
}

static void vk_query_destroy(r10_device *d, r10_query *q)
{
    (void)d;
    vk_query *vq = (vk_query *)q->be;
    if (active_occlusion == q) { if (cur.active) occlusion_pass_end(); active_occlusion = NULL; }
    if (vq) unlink_pending(vq);
    free(vq);
}

static void occlusion_pass_begin(void)
{
    if (!active_occlusion || !occlusion_pools[0]) return;
    vk_query *vq = (vk_query *)active_occlusion->be;
    if (vq->slot_count == 16 || occlusion_used[frame_index] >= QUERY_POOL_SIZE) return;
    uint32_t s = occlusion_used[frame_index]++;
    vkCmdBeginQuery(cmd, occlusion_pools[frame_index], s, 0);
    vq->slots[vq->slot_count++] = s;
    vq->running = 1;
    cur.occlusion = active_occlusion;
}

static void occlusion_pass_end(void)
{
    if (!cur.occlusion) return;
    vk_query *vq = (vk_query *)cur.occlusion->be;
    if (vq->running) { vkCmdEndQuery(cmd, occlusion_pools[frame_index], vq->slots[vq->slot_count - 1]); vq->running = 0; }
    cur.occlusion = NULL;
}

static void vk_query_begin(r10_device *d, r10_query *q)
{
    (void)d;
    if (q->type != D3D10_QUERY_OCCLUSION && q->type != D3D10_QUERY_OCCLUSION_PREDICATE) return;
    vk_query *vq = (vk_query *)q->be;
    if (debug_flags() & 2048) { vq->slot_count = 0; vq->frame = frame_index; return; }   /* measurements: no GPU queries */
    if (active_occlusion && cur.active) occlusion_pass_end();
    unlink_pending(vq);
    vq->slot_count = 0;
    vq->running = 0;
    vq->ready = 0;
    vq->frame = frame_index;
    active_occlusion = q;
    if (cur.active) occlusion_pass_begin();
}

static void vk_query_end(r10_device *d, r10_query *q)
{
    (void)d;
    vk_query *vq = (vk_query *)q->be;
    vq->serial = q->type == D3D10_QUERY_EVENT && !work_recorded ? submit_serial - 1 : submit_serial;
    vq->ready = 0;
    if (q->type == D3D10_QUERY_OCCLUSION || q->type == D3D10_QUERY_OCCLUSION_PREDICATE) {
        if (active_occlusion == q) { if (cur.active) occlusion_pass_end(); active_occlusion = NULL; }
        if (vq->frame != frame_index) vq->slot_count = 0;   /* begun in another frame: its slots belong to that pool */
        if (debug_flags() & 2048) { q->samples = 1000; vq->ready = 1; return; }
        if (!vq->slot_count) { q->samples = 0; vq->ready = 2; return; }
        vq->next_pending = pending_queries[frame_index];
        pending_queries[frame_index] = vq;
    }
}

/* Whether the GPU finished submission `serial` (polls the fences, never waits). */
static int serial_done(uint64_t serial)
{
    if (serial <= completed_serial) return 1;
    if (done_map) {
        uint64_t done = done_marker_read();
        if (done > completed_serial && done < submit_serial) completed_serial = done;
        return serial <= completed_serial;
    }
    for (int i = 0; i < FRAMES; ++i) {
        vk_frame *f = &frames[i];
        if (!f->fence_count) continue;
        /* the fences of a frame signal in submission order: the last one completes the frame */
        uint64_t status_start = r10_now_ns();
        VkResult r = vkGetFenceStatus(device, f->fences[f->fence_count - 1]);
        if (r10_dev) r10_dev->stats.status_us += (uint32_t)((r10_now_ns() - status_start) / 1000);
        if (r == VK_SUCCESS && f->serial > completed_serial) completed_serial = f->serial;
    }
    return serial <= completed_serial;
}

static int vk_query_poll(r10_device *d, r10_query *q, int flush)
{
    (void)d;
    vk_query *vq = (vk_query *)q->be;
    if (q->type == D3D10_QUERY_TIMESTAMP) { q->samples = r10_now_ns(); return 1; }
    if (vq->ready == 1) return 1;
    if (vq->serial >= submit_serial) {
        if (!flush) return 0;
        submit_partial();
    }
    if (!serial_done(vq->serial)) return 0;
    if (vq->ready == 2) return 1;
    if (q->type == D3D10_QUERY_OCCLUSION || q->type == D3D10_QUERY_OCCLUSION_PREDICATE) {
        uint64_t total = 0;
        for (uint32_t i = 0; i < vq->slot_count; ++i) {
            uint64_t v = 0;
            if (vkGetQueryPoolResults(device, occlusion_pools[vq->frame], vq->slots[i], 1, sizeof v, &v, sizeof v, VK_QUERY_RESULT_64_BIT) == VK_SUCCESS) total += v;
        }
        unlink_pending(vq);
        vq->slot_count = 0;
        q->samples = total;
    }
    vq->ready = 1;
    return 1;
}

static void vk_flush(r10_device *d) { (void)d; }

/* ------------------------------------------------------------------ present */
static VkDescriptorSet present_set;

static uint32_t vk_present_inner(r10_device *d, r10_resource *bb, uint32_t interval);
static uint32_t vk_present(r10_device *d, r10_resource *bb, uint32_t interval)
{
    uint64_t start = r10_now_ns();
    uint32_t r = vk_present_inner(d, bb, interval);
    d->stats.present_ns += (uint32_t)(r10_now_ns() - start);
    return r;
}
static uint32_t vk_present_inner(r10_device *d, r10_resource *bb, uint32_t interval)
{
    (void)interval;
    pending_flush_resource(NULL);
    end_pass();
    vk_image *im = img(bb);
    transition(cmd, im, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    vk_frame *f = &frames[frame_index];
    uint32_t index = 0;
    int have_image = follow_window();
    VkResult r = VK_ERROR_OUT_OF_DATE_KHR;
    if (have_image) {
        uint64_t acquire_start = r10_now_ns();
        void_machine_lend_begin();
        r = vkAcquireNextImageKHR(device, swapchain, 2000000000ull, f->acquire, VK_NULL_HANDLE, &index);
        void_machine_lend_end();
        d->stats.wait_us += (uint32_t)((r10_now_ns() - acquire_start) / 1000);
        d->stats.acquire_us += (uint32_t)((r10_now_ns() - acquire_start) / 1000);
        if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_ERROR_SURFACE_LOST_KHR) { swap_broken = 1; have_image = 0; }
        else if (r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR) { R10_LOG("Vulkan: acquire %d", r); have_image = 0; }
    }
    if (have_image) {
        VkSemaphore done = swap_images[index].done;
        /* The present pass: letterboxed and rotated for the pre-transform. */
        VkDescriptorSet set = allocate_set(present_set_layout);
        VkDescriptorImageInfo ii = { present_sampler, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
        /* A view of the back buffer (created once per back buffer image). */
        static VkImage bb_image;
        static VkImageView bb_view;
        if (bb_image != im->image) {
            if (bb_view) dead_push(DEAD_VIEW, (uint64_t)bb_view, NULL);
            VkImageViewCreateInfo vi = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, NULL, 0, im->image, VK_IMAGE_VIEW_TYPE_2D, im->format,
                                         { 0, 0, 0, 0 }, { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } };
            vkCreateImageView(device, &vi, NULL, &bb_view);
            bb_image = im->image;
        }
        ii.imageView = bb_view;
        VkWriteDescriptorSet w = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, NULL, set, 0, 0, 1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &ii, NULL, NULL };
        vkUpdateDescriptorSets(device, 1, &w, 0, NULL);
        present_set = set;
        int rotated = swap_transform == VK_SURFACE_TRANSFORM_ROTATE_90_BIT_KHR || swap_transform == VK_SURFACE_TRANSFORM_ROTATE_270_BIT_KHR;
        uint32_t lw = rotated ? swap_h : swap_w, lh = rotated ? swap_w : swap_h;
        int32_t x, y, w2, h2;
        r10_fit(bb->desc.width, bb->desc.height, lw, lh, &x, &y, &w2, &h2);
        float pc[8];
        float sx = (float)w2 / (float)lw, sy = (float)h2 / (float)lh;
        float ox = ((float)x + (float)w2 * 0.5f) / (float)lw * 2.0f - 1.0f, oy = ((float)y + (float)h2 * 0.5f) / (float)lh * 2.0f - 1.0f;
        switch (swap_transform) {
        case VK_SURFACE_TRANSFORM_ROTATE_90_BIT_KHR: pc[0] = 0; pc[1] = -1; pc[2] = 1; pc[3] = 0; break;
        case VK_SURFACE_TRANSFORM_ROTATE_180_BIT_KHR: pc[0] = -1; pc[1] = 0; pc[2] = 0; pc[3] = -1; break;
        case VK_SURFACE_TRANSFORM_ROTATE_270_BIT_KHR: pc[0] = 0; pc[1] = 1; pc[2] = -1; pc[3] = 0; break;
        default: pc[0] = 1; pc[1] = 0; pc[2] = 0; pc[3] = 1; break;
        }
        pc[4] = sx; pc[5] = sy; pc[6] = ox; pc[7] = oy;
        VkClearValue black = { { { 0, 0, 0, 1 } } };
        VkRenderPassBeginInfo bi = { VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO, NULL, present_pass, swap_images[index].framebuffer,
                                     { { 0, 0 }, { swap_w, swap_h } }, 1, &black };
        int open = op_timer_begin("present pass", swap_w, swap_h);
        vkCmdBeginRenderPass(cmd, &bi, VK_SUBPASS_CONTENTS_INLINE);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, present_pipeline);
        VkViewport vp = { 0, 0, (float)swap_w, (float)swap_h, 0, 1 };
        VkRect2D sc = { { 0, 0 }, { swap_w, swap_h } };
        vkCmdSetViewport(cmd, 0, 1, &vp);
        vkCmdSetScissor(cmd, 0, 1, &sc);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, present_layout, 0, 1, &set, 0, NULL);
        vkCmdPushConstants(cmd, present_layout, VK_SHADER_STAGE_VERTEX_BIT, 0, 32, pc);
        vkCmdDraw(cmd, 4, 1, 0, 0);
        vkCmdEndRenderPass(cmd);
        op_timer_end(open);
        im->gpu_use = submit_serial;
        if (gpu_timer_pool && f->timed == 1) {
            vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, gpu_timer_pool, 2 * frame_index + 1);
            f->timed = 2;
        }
        submit(f->acquire, done);
        /* The limiter below gives the machine to every guest thread (r10_pace_frame sleeps inside
         * void_blocking_begin), and the game's texture streaming thread issues CopySubresourceRegion at any
         * time: what it records goes to a new command buffer of this frame slot, never into the one just
         * submitted (recording into a pending command buffer crashed the Adreno 650's driver, RMX3371). */
        begin_frame_commands();
        d->st.dirty = R10_DIRTY_ALL;
        probe_report();
        r10_pace_frame();
        if (!present_handoff(index, done)) {
            VkPresentInfoKHR pi = { VK_STRUCTURE_TYPE_PRESENT_INFO_KHR, NULL, 1, &done, 1, &swapchain, &index, NULL };
            uint64_t present_start = r10_now_ns();
            r = vkQueuePresentKHR(queue, &pi);
            if (r == VK_SUCCESS || r == VK_SUBOPTIMAL_KHR) r10_note_present();
            d->stats.wait_us += (uint32_t)((r10_now_ns() - present_start) / 1000);
            d->stats.queue_present_us += (uint32_t)((r10_now_ns() - present_start) / 1000);
            if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_ERROR_SURFACE_LOST_KHR) swap_broken = 1;
            else if (r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR) R10_LOG("Vulkan: present %d", r);
        } else d->stats.queue_present_us += __atomic_exchange_n(&present_worker_us, 0, __ATOMIC_RELAXED);
    } else {
        submit(VK_NULL_HANDLE, VK_NULL_HANDLE);
        begin_frame_commands();
        d->st.dirty = R10_DIRTY_ALL;
        r10_pace_frame();
    }
    /* Work other threads recorded meanwhile is submitted before the next frame's; an empty command buffer is
     * reset with its frame slot. */
    if (work_recorded || upload_used) submit(VK_NULL_HANDLE, VK_NULL_HANDLE);
    next_frame();
    d->st.dirty = R10_DIRTY_ALL;
    static uint64_t saved;
    if (d->frame - saved >= 900 || (saved == 0 && d->frame == 600)) {
        /* New pipelines since the last save go to the file on a worker (save_pipeline_cache_async). */
        save_pipeline_cache_async();
        saved = d->frame;
    }
    return have_image ? 0 : DXGI_STATUS_OCCLUDED_;
}

static uint8_t *vk_read_rgba(r10_device *d, r10_resource *r, uint32_t sub, uint32_t *w, uint32_t *h)
{
    (void)d; (void)r; (void)sub; (void)w; (void)h;
    return NULL;
}

static void vk_layout_destroy(r10_device *d, r10_layout *l) { (void)d; (void)l; }
static void vk_state_destroy(r10_device *d, r10_state_object *s) { (void)d; (void)s; }

/* ------------------------------------------------------------------ AMD FSR1 */
/* The render scale draws the scene's tonemap into the top-left sw x sh of the back buffer; EASU upscales it
 * into an image of the back buffer's size, RCAS sharpens that back into the back buffer before the HUD. */
static VkDescriptorSetLayout fsr_set_layout;
static VkPipelineLayout fsr_layout;
static VkPipeline fsr_easu_pipe, fsr_rcas_pipe;
static VkRenderPass fsr_pass;
static VkSampler fsr_sampler;
static VkFormat fsr_format;
static int fsr_failed;
static vk_image fsr_temp;
static VkImageView fsr_temp_view, fsr_bb_view;
static VkFramebuffer fsr_temp_fb, fsr_bb_fb;
static VkImage fsr_bb_image;

static VkPipeline fsr_pipeline(VkShaderModule vs, VkShaderModule fs)
{
    VkPipelineShaderStageCreateInfo stages[2] = {
        { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, NULL, 0, VK_SHADER_STAGE_VERTEX_BIT, vs, "main", NULL },
        { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, NULL, 0, VK_SHADER_STAGE_FRAGMENT_BIT, fs, "main", NULL } };
    VkPipelineVertexInputStateCreateInfo vi = { VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
    VkPipelineInputAssemblyStateCreateInfo ia = { VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO, NULL, 0,
                                                  VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, VK_FALSE };
    VkPipelineViewportStateCreateInfo vp = { VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, NULL, 0, 1, NULL, 1, NULL };
    VkPipelineRasterizationStateCreateInfo rs = { VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
    rs.polygonMode = VK_POLYGON_MODE_FILL; rs.cullMode = VK_CULL_MODE_NONE; rs.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo ms = { VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineDepthStencilStateCreateInfo ds = { VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO };
    VkPipelineColorBlendAttachmentState att = { 0 };
    att.colorWriteMask = 0xF;
    VkPipelineColorBlendStateCreateInfo cb = { VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO, NULL, 0, VK_FALSE,
                                               VK_LOGIC_OP_COPY, 1, &att, { 0, 0, 0, 0 } };
    VkDynamicState dyn[2] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
    VkPipelineDynamicStateCreateInfo dy = { VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO, NULL, 0, 2, dyn };
    VkGraphicsPipelineCreateInfo pi = { VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, NULL, 0, 2, stages, &vi, &ia, NULL, &vp,
                                        &rs, &ms, &ds, &cb, &dy, fsr_layout, fsr_pass, 0, VK_NULL_HANDLE, -1 };
    VkPipeline pipe = VK_NULL_HANDLE;
    if (vkCreateGraphicsPipelines(device, pipeline_cache, 1, &pi, NULL, &pipe) != VK_SUCCESS) return VK_NULL_HANDLE;
    return pipe;
}

static int fsr_prepare(VkFormat format)
{
    if (fsr_easu_pipe && fsr_format == format) return 1;
    if (fsr_failed) return 0;
    if (!fsr_set_layout) {
        VkDescriptorSetLayoutBinding b = { 0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL };
        VkDescriptorSetLayoutCreateInfo sl = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, NULL, 0, 1, &b };
        VkPushConstantRange range = { VK_SHADER_STAGE_FRAGMENT_BIT, 0, 64 };
        VkPipelineLayoutCreateInfo pl = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, NULL, 0, 1, &fsr_set_layout, 1, &range };
        VkSamplerCreateInfo si = { VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
        si.magFilter = si.minFilter = VK_FILTER_LINEAR;
        si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        si.maxLod = 0.25f;
        if (vkCreateDescriptorSetLayout(device, &sl, NULL, &fsr_set_layout) != VK_SUCCESS ||
            vkCreatePipelineLayout(device, &pl, NULL, &fsr_layout) != VK_SUCCESS ||
            vkCreateSampler(device, &si, NULL, &fsr_sampler) != VK_SUCCESS) {
            fsr_failed = 1;
            return 0;
        }
    }
    pass_key k;
    memset(&k, 0, sizeof k);
    k.color[0] = (uint32_t)format;
    k.color_count = 1;
    k.color_load[0] = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    fsr_pass = render_pass(&k);
    VkShaderModule vs = module_from(r10_fsr_vert, sizeof r10_fsr_vert);
    VkShaderModule easu = half_shaders && (debug_flags() & 8192) ? module_from(r10_fsr_easu_half_frag, sizeof r10_fsr_easu_half_frag)
                                                                 : module_from(r10_fsr_easu_frag, sizeof r10_fsr_easu_frag);
    VkShaderModule rcas = module_from(r10_fsr_rcas_frag, sizeof r10_fsr_rcas_frag);
    if (fsr_easu_pipe) { present_drain(); vkDeviceWaitIdle(device); vkDestroyPipeline(device, fsr_easu_pipe, NULL); vkDestroyPipeline(device, fsr_rcas_pipe, NULL); }
    fsr_easu_pipe = vs && easu ? fsr_pipeline(vs, easu) : VK_NULL_HANDLE;
    fsr_rcas_pipe = vs && rcas ? fsr_pipeline(vs, rcas) : VK_NULL_HANDLE;
    if (vs) vkDestroyShaderModule(device, vs, NULL);
    if (easu) vkDestroyShaderModule(device, easu, NULL);
    if (rcas) vkDestroyShaderModule(device, rcas, NULL);
    if (!fsr_easu_pipe || !fsr_rcas_pipe || !fsr_pass) {
        fsr_failed = 1;
        R10_LOG("Vulkan: FSR1 unavailable; the render scale stretches the scene bilinearly");
        return 0;
    }
    fsr_format = format;
    R10_LOG("Vulkan: FSR1 EASU (%s)/RCAS ready (format %d)", half_shaders && (debug_flags() & 8192) ? "fp16" : "fp32", (int)format);
    return 1;
}

static VkImageView fsr_view(VkImage image, VkFormat format)
{
    VkImageViewCreateInfo vi = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, NULL, 0, image, VK_IMAGE_VIEW_TYPE_2D, format,
                                 { VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY,
                                   VK_COMPONENT_SWIZZLE_IDENTITY }, { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } };
    VkImageView view = VK_NULL_HANDLE;
    vkCreateImageView(device, &vi, NULL, &view);
    return view;
}

/* Images of the back buffer's size: the temporary EASU target and the framebuffers of both passes. */
static int fsr_targets(vk_image *bb)
{
    if (fsr_bb_image == bb->image && fsr_temp.image && fsr_temp.width == bb->width && fsr_temp.height == bb->height &&
        fsr_temp.format == bb->format)
        return 1;
    present_drain();
    vkDeviceWaitIdle(device);   /* a resize: nothing of the old targets may be in flight */
    if (fsr_temp_fb) vkDestroyFramebuffer(device, fsr_temp_fb, NULL);
    if (fsr_bb_fb) vkDestroyFramebuffer(device, fsr_bb_fb, NULL);
    if (fsr_temp_view) vkDestroyImageView(device, fsr_temp_view, NULL);
    if (fsr_bb_view) vkDestroyImageView(device, fsr_bb_view, NULL);
    if (fsr_temp.image) { vkDestroyImage(device, fsr_temp.image, NULL); mem_free(&fsr_temp.mem); }
    memset(&fsr_temp, 0, sizeof fsr_temp);
    fsr_temp_fb = fsr_bb_fb = VK_NULL_HANDLE;
    fsr_temp_view = fsr_bb_view = VK_NULL_HANDLE;
    fsr_bb_image = VK_NULL_HANDLE;
    VkImageCreateInfo ci = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, NULL, 0, VK_IMAGE_TYPE_2D, bb->format, { bb->width, bb->height, 1 },
                             1, 1, VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_TILING_OPTIMAL,
                             VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, VK_SHARING_MODE_EXCLUSIVE, 0, NULL,
                             VK_IMAGE_LAYOUT_UNDEFINED };
    if (vkCreateImage(device, &ci, NULL, &fsr_temp.image) != VK_SUCCESS) { fsr_temp.image = VK_NULL_HANDLE; return 0; }
    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(device, fsr_temp.image, &req);
    if (!mem_alloc(req, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0, MK_TARGET, &fsr_temp.mem)) {
        vkDestroyImage(device, fsr_temp.image, NULL);
        fsr_temp.image = VK_NULL_HANDLE;
        return 0;
    }
    vkBindImageMemory(device, fsr_temp.image, fsr_temp.mem.block->memory, fsr_temp.mem.offset);
    fsr_temp.format = bb->format;
    fsr_temp.unorm_format = bb->format;
    fsr_temp.aspect = VK_IMAGE_ASPECT_COLOR_BIT;
    fsr_temp.layout = VK_IMAGE_LAYOUT_UNDEFINED;
    fsr_temp.width = bb->width; fsr_temp.height = bb->height; fsr_temp.depth = 1; fsr_temp.mips = 1; fsr_temp.layers = 1;
    fsr_temp_view = fsr_view(fsr_temp.image, bb->format);
    fsr_bb_view = fsr_view(bb->image, bb->format);
    VkFramebufferCreateInfo fi = { VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO, NULL, 0, fsr_pass, 1, &fsr_temp_view, bb->width, bb->height, 1 };
    vkCreateFramebuffer(device, &fi, NULL, &fsr_temp_fb);
    fi.pAttachments = &fsr_bb_view;
    vkCreateFramebuffer(device, &fi, NULL, &fsr_bb_fb);
    fsr_bb_image = bb->image;
    return fsr_temp_view && fsr_bb_view && fsr_temp_fb && fsr_bb_fb;
}

static int fsr_pass_run(VkFramebuffer fb, VkPipeline pipe, VkImageView source, uint32_t w, uint32_t h, const uint32_t *con)
{
    VkDescriptorSet set = allocate_set(fsr_set_layout);
    if (!set) return 0;
    VkDescriptorImageInfo ii = { fsr_sampler, source, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
    VkWriteDescriptorSet w1 = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, NULL, set, 0, 0, 1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &ii, NULL, NULL };
    vkUpdateDescriptorSets(device, 1, &w1, 0, NULL);
    VkRenderPassBeginInfo bi = { VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO, NULL, fsr_pass, fb, { { 0, 0 }, { w, h } }, 0, NULL };
    int timed = r10_dev && r10_dev->tracing && pass_timer_pool && pass_timer_count < PASS_TIMERS;
    if (timed) {
        vkCmdResetQueryPool(cmd, pass_timer_pool, 2 * pass_timer_count, 2);
        vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, pass_timer_pool, 2 * pass_timer_count);
        memset(&pass_info[pass_timer_count], 0, sizeof pass_info[0]);
        pass_info[pass_timer_count].color = 9999; pass_info[pass_timer_count].w = w; pass_info[pass_timer_count].h = h;
    }
    vkCmdBeginRenderPass(cmd, &bi, VK_SUBPASS_CONTENTS_INLINE);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
    VkViewport vp = { 0, 0, (float)w, (float)h, 0, 1 };
    VkRect2D sc = { { 0, 0 }, { w, h } };
    vkCmdSetViewport(cmd, 0, 1, &vp);
    vkCmdSetScissor(cmd, 0, 1, &sc);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, fsr_layout, 0, 1, &set, 0, NULL);
    vkCmdPushConstants(cmd, fsr_layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, 64, con);
    vkCmdDraw(cmd, 3, 1, 0, 0);
    vkCmdEndRenderPass(cmd);
    if (timed) { vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, pass_timer_pool, 2 * pass_timer_count + 1); ++pass_timer_count; }
    return 1;
}

static int vk_upscale(r10_device *d, r10_resource *bb, uint32_t sw, uint32_t sh, float sharpness)
{
    if (!bb) return fsr_failed ? 0 : fsr_prepare(VK_FORMAT_R8G8B8A8_UNORM);
    vk_image *im = img(bb);
    if (!im || !im->image || im->depth_kind || (im->lazy && !image_ready(im)) || !fsr_prepare(im->format) || !fsr_targets(im)) return 0;
    work_recorded = 1;
    pending_flush_resource(bb);
    end_pass();
    uint32_t easu[16], rcas[16] = { 0 };
    r10_fsr_constants(easu, rcas, (float)sw, (float)sh, (float)im->width, (float)im->height, (float)im->width, (float)im->height,
                      sharpness);
    transition(cmd, im, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    transition(cmd, &fsr_temp, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    if (!fsr_pass_run(fsr_temp_fb, fsr_easu_pipe, fsr_bb_view, im->width, im->height, easu)) return 0;
    fsr_temp.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    transition(cmd, &fsr_temp, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    transition(cmd, im, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    if (!fsr_pass_run(fsr_bb_fb, fsr_rcas_pipe, fsr_temp_view, im->width, im->height, rcas)) return 0;
    im->gpu_use = submit_serial;
    bound = VK_NULL_HANDLE;
    d->st.dirty = R10_DIRTY_ALL;
    return 1;
}

/* The statistics' memory line: the device memory blocks (64 MiB blocks are kept once allocated) and what the
 * allocations in them hold. */
static void vk_memory_line(char *out, size_t size)
{
    uint64_t linear = 0, optimal = 0, used = 0, dedicated = 0;
    uint32_t count = 0;
    for (vk_block *b = blocks; b; b = b->next) {
        ++count;
        if (b->linear) linear += b->size; else optimal += b->size;
        if (b->own) dedicated += b->size;
        used += b->used;
    }
    int n = snprintf(out, size, "Vulkan %llu MiB in %u blocks (%llu MiB buffers, %llu MiB images, %llu MiB own), %llu MiB used:",
                     (unsigned long long)((linear + optimal) >> 20), count, (unsigned long long)(linear >> 20),
                     (unsigned long long)(optimal >> 20), (unsigned long long)(dedicated >> 20), (unsigned long long)(used >> 20));
    for (int k = 0; k < MK_KINDS && n > 0 && (size_t)n < size; ++k)
        n += snprintf(out + n, size - (size_t)n, "%s %s %.1f MiB (%u)", k ? "," : "", mk_name[k], (double)mk_bytes[k] / 1048576.0,
                      mk_count[k]);
    if (n > 0 && (size_t)n < size)
        snprintf(out + n, size - (size_t)n, "; ring peak %.1f MiB per frame (chunks %u MiB), %u blocks released, %u shader modules, "
                 "%u targets without memory (%.1f MiB)", (double)ring_peak_bytes / 1048576.0, ring_base >> 20, blocks_released,
                 shader_modules, lazy_waiting, (double)lazy_waiting_bytes / 1048576.0);
    ring_peak_bytes = 0;
}

r10_backend r10_vulkan_backend = {
    "Vulkan", vk_init, vk_shutdown, 0,
    vk_resource_create, vk_resource_destroy, vk_resource_update, vk_resource_read, vk_copy, vk_resolve, vk_generate_mips,
    vk_view_create, vk_view_destroy, vk_sampler_create, vk_sampler_destroy, vk_shader_create, vk_shader_destroy,
    vk_layout_destroy, vk_state_destroy, vk_clear_rtv, vk_clear_dsv, vk_draw, vk_query_create, vk_query_destroy,
    vk_query_begin, vk_query_end, vk_query_poll, vk_flush, vk_present, vk_read_rgba, vk_upscale,
    vk_samples, 0, vk_resource_store, vk_memory_line, vk_buffer_read,
};

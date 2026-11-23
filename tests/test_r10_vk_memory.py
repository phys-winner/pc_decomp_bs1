"""Device memory of the native Direct3D 10 renderer's Vulkan backend (android/render10/r10_vulkan.c).

The production allocator and frame ring run here against a model of the Vulkan memory API (one memory type that is
device-local, host-visible and coherent, as on the Mali-G615): pooled allocations go to the fullest block that takes
them and empty blocks go back to the driver while one spare of their kind stays; buffer blocks are 16 MiB, image
blocks 64 MiB, and larger requests and the ring's chunks have memory of their own; the ring's chunk size starts at
4 MiB, grows to fit a frame (up to 24 MiB) and shrinks after 600 frames that need less; a frame's extra chunks are
freed when its slot is reused.  On the test tablet the old allocator kept 237 of 938 MiB as empty blocks during play
(work/ha22-perf-20261122).
"""
from pathlib import Path
import re
import subprocess
import unittest

from tests.host_environment import CLANG, environment
from tests.test_mobile_shaders import function

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / "build/tests/r10-vk-memory"

HARNESS = r'''
#define _CRT_SECURE_NO_WARNINGS
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef uint64_t VkDeviceSize; typedef uint32_t VkFlags, VkMemoryPropertyFlags, VkBufferUsageFlags; typedef int VkResult;
typedef struct VkDeviceMemory_T *VkDeviceMemory; typedef struct VkBuffer_T *VkBuffer; typedef void *VkDevice;
typedef void *VkCommandPool, *VkCommandBuffer, *VkFence, *VkSemaphore, *VkDescriptorPool;
#define VK_SUCCESS 0
#define VK_ERROR_OUT_OF_DEVICE_MEMORY (-2)
#define VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT 1u
#define VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT 2u
#define VK_MEMORY_PROPERTY_HOST_COHERENT_BIT 4u
#define VK_MEMORY_PROPERTY_HOST_CACHED_BIT 8u
#define VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO 12
#define VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO 5
#define VK_BUFFER_USAGE_TRANSFER_SRC_BIT 1u
#define VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT 0x10u
#define VK_BUFFER_USAGE_INDEX_BUFFER_BIT 0x40u
#define VK_BUFFER_USAGE_VERTEX_BUFFER_BIT 0x80u
#define VK_SHARING_MODE_EXCLUSIVE 0
#define VK_WHOLE_SIZE (~0ull)
typedef struct { VkDeviceSize size, alignment; uint32_t memoryTypeBits; } VkMemoryRequirements;
typedef struct { int sType; const void *pNext; VkDeviceSize allocationSize; uint32_t memoryTypeIndex; } VkMemoryAllocateInfo;
typedef struct { int sType; const void *pNext; VkFlags flags; VkDeviceSize size; VkBufferUsageFlags usage; int sharingMode;
                 uint32_t queueFamilyIndexCount; const uint32_t *pQueueFamilyIndices; } VkBufferCreateInfo;
typedef struct { VkMemoryPropertyFlags propertyFlags; uint32_t heapIndex; } VkMemoryType;
typedef struct { uint32_t memoryTypeCount; VkMemoryType memoryTypes[32]; } VkPhysicalDeviceMemoryProperties;
typedef struct { struct { VkDeviceSize nonCoherentAtomSize; } limits; } VkPhysicalDeviceProperties;
static VkPhysicalDeviceMemoryProperties mem_props = { 1, { { 7u, 0 } } };
static VkPhysicalDeviceProperties gpu_props = { { 64 } };
static VkDevice device;
static int debug_value;
static int debug_flags(void) { return debug_value; }
static unsigned logs;
#define R10_LOG(...) (++logs, printf("[r10] " __VA_ARGS__), printf("\n"))

/* The driver model: allocations with their sizes, the live total and the buffers. */
static unsigned allocations, frees, live_memories; static uint64_t live_bytes, peak_bytes;
static VkResult vkAllocateMemory(VkDevice d, const VkMemoryAllocateInfo *ai, const void *a, VkDeviceMemory *out)
{
    (void)d; (void)a;
    uint64_t *m = (uint64_t *)malloc(16); m[0] = ai->allocationSize; m[1] = 0;
    *out = (VkDeviceMemory)m; ++allocations; ++live_memories; live_bytes += ai->allocationSize;
    if (live_bytes > peak_bytes) peak_bytes = live_bytes;
    return VK_SUCCESS;
}
static void vkFreeMemory(VkDevice d, VkDeviceMemory m, const void *a)
{
    (void)d; (void)a; uint64_t *p = (uint64_t *)m;
    ++frees; --live_memories; live_bytes -= p[0]; free((void *)p[1]); free(p);
}
static VkResult vkMapMemory(VkDevice d, VkDeviceMemory m, VkDeviceSize o, VkDeviceSize s, VkFlags f, void **out)
{
    (void)d; (void)o; (void)s; (void)f; uint64_t *p = (uint64_t *)m;
    if (!p[1]) p[1] = (uint64_t)(uintptr_t)calloc(1, 16);   /* a token address: the ring only adds offsets */
    *out = (void *)(uintptr_t)p[1];
    return VK_SUCCESS;
}
static unsigned buffers;
static VkResult vkCreateBuffer(VkDevice d, const VkBufferCreateInfo *bi, const void *a, VkBuffer *out)
{
    (void)d; (void)a; uint64_t *b = (uint64_t *)malloc(8); b[0] = bi->size; *out = (VkBuffer)b; ++buffers; return VK_SUCCESS;
}
static void vkDestroyBuffer(VkDevice d, VkBuffer b, const void *a) { (void)d; (void)a; free(b); --buffers; }
static void vkGetBufferMemoryRequirements(VkDevice d, VkBuffer b, VkMemoryRequirements *req)
{
    (void)d; req->size = ((uint64_t *)b)[0]; req->alignment = 256; req->memoryTypeBits = 1;
}
static VkResult vkBindBufferMemory(VkDevice d, VkBuffer b, VkDeviceMemory m, VkDeviceSize o) { (void)d; (void)b; (void)m; (void)o; return VK_SUCCESS; }
typedef struct vk_dead vk_dead;

@DECLARATIONS@

@FUNCTIONS@

static VkMemoryRequirements req(uint64_t size) { VkMemoryRequirements r = { size, 256, 1 }; return r; }
static unsigned pooled_blocks(int linear)
{
    unsigned n = 0;
    for (vk_block *b = blocks; b; b = b->next) n += !b->own && b->linear == linear;
    return n;
}
static uint64_t frame_bytes(uint64_t bytes)
{
    vk_slice s;
    uint64_t done = 0;
    while (done < bytes) {
        uint32_t step = bytes - done > (1u << 20) ? 1u << 20 : (uint32_t)(bytes - done);
        assert(ring_alloc(step, 256, &s));
        done += step;
    }
    return done;
}
static void next_slot(void) { frame_index = (frame_index + 1) % FRAMES; frame_reset_ring(&frames[frame_index]); }

int main(void)
{
    /* Pooled buffers: 16 MiB blocks, the fullest block first. */
    vk_alloc a[64]; memset(a, 0, sizeof a);
    for (int i = 0; i < 6; ++i) assert(mem_alloc(req(4u << 20), 7u, 1, MK_BUFFER, &a[i]));   /* 24 MiB: two blocks */
    assert(pooled_blocks(1) == 2 && LINEAR_BLOCK_SIZE == (16u << 20));
    assert(a[0].block == a[1].block && a[1].block == a[2].block && a[2].block == a[3].block && a[4].block != a[0].block);
    mem_free(&a[0]);                                       /* the first block has 4 MiB free, the second 8 MiB */
    assert(mem_alloc(req(4u << 20), 7u, 1, MK_BUFFER, &a[6]));
    assert(a[6].block == a[1].block);                      /* the fuller one takes it, not the first in the list */
    /* Emptied blocks: one spare stays, a second empty one goes back to the driver. */
    unsigned before = frees;
    mem_free(&a[4]); mem_free(&a[5]);                      /* the second block is empty: the only spare */
    assert(frees == before && pooled_blocks(1) == 2);
    mem_free(&a[1]); mem_free(&a[2]); mem_free(&a[3]); mem_free(&a[6]);   /* the first empties too: one of them goes */
    assert(frees == before + 1 && pooled_blocks(1) == 1 && blocks_released == 1);
    /* Larger requests have memory of their own, freed with them; images pool in 64 MiB blocks. */
    vk_alloc big = { 0 }, image = { 0 };
    assert(mem_alloc(req(9u << 20), 7u, 1, MK_BUFFER, &big) && big.block->own && big.block->size == (9u << 20));
    assert(mem_alloc(req(20u << 20), 7u, 0, MK_TARGET, &image) && !image.block->own && image.block->size == BLOCK_SIZE);
    before = frees; mem_free(&big); assert(frees == before + 1);
    assert(mk_bytes[MK_TARGET] == (20u << 20) && mk_count[MK_TARGET] == 1);
    mem_free(&image); assert(mk_bytes[MK_TARGET] == 0 && mk_count[MK_TARGET] == 0);
    /* The ring: 4 MiB chunks of their own while frames take 3 MiB (Arcadia with the pack: 2.8-4.0 MiB). */
    assert(ring_base == RING_MIN && RING_MIN == (4u << 20) && RING_MAX == (24u << 20));
    for (int i = 0; i < 30; ++i) { frame_bytes(3u << 20); next_slot(); }
    for (int i = 0; i < FRAMES; ++i) assert(!frames[i].ring || (frames[i].ring->size == RING_MIN && frames[i].ring->mem.block->own));
    assert(ring_base == RING_MIN);
    /* A frame of 5 MiB (decoded BC) grows the chunks to it and a quarter, rounded up to 1 MiB. */
    frame_bytes(5u << 20);
    for (int i = 0; i < FRAMES; ++i) next_slot();   /* applied when its slot comes back */
    assert(ring_base == (7u << 20));
    /* A level load's frames of 60 MiB grow the chunks to 24 MiB; their extra chunks are freed when the slot returns. */
    unsigned live = live_memories;
    for (int i = 0; i < 6; ++i) { frame_bytes(60u << 20); next_slot(); }
    assert(ring_base == RING_MAX);
    for (int i = 0; i < FRAMES; ++i) { next_slot(); }
    for (int i = 0; i < FRAMES; ++i) assert(!frames[i].ring || !frames[i].ring->next);
    /* Back to 5 MiB frames: after 600 quiet frames the chunks shrink to their peak and a quarter (7 MiB). */
    for (int i = 0; i < 700; ++i) { frame_bytes(5u << 20); next_slot(); }
    assert(ring_base == (7u << 20));
    for (int i = 0; i < FRAMES; ++i) { frame_bytes(5u << 20); next_slot(); }
    uint64_t ring_live = 0;
    for (int i = 0; i < FRAMES; ++i) for (vk_chunk *c = frames[i].ring; c; c = c->next) ring_live += c->size;
    assert(ring_live <= (uint64_t)FRAMES * (7u << 20));
    /* 3 MiB frames again: down to RING_MIN once a whole quiet window holds only them. */
    for (int i = 0; i < 1300; ++i) { frame_bytes(3u << 20); next_slot(); }
    assert(ring_base == RING_MIN);
    assert(live_memories <= live + FRAMES);
    /* BIOSHOCK_R10_DEBUG 262144: blocks stay for the session as before. */
    debug_value = 262144;
    vk_alloc k[2] = { { 0 } };
    assert(mem_alloc(req(1u << 20), 7u, 0, MK_TEXTURE, &k[0]) && mem_alloc(req(40u << 20), 7u, 0, MK_TEXTURE, &k[1]));
    before = frees; mem_free(&k[0]); mem_free(&k[1]);
    assert(frees == before + 1);   /* only the own allocation went */
    printf("OK allocations %u frees %u peak %.1f MiB\n", allocations, frees, peak_bytes / 1048576.0);
    return 0;
}
'''


def declarations(source):
    """The allocator's types, constants and state as the production file defines them."""
    out = []
    for name in ("vk_block", "vk_frame"):
        start = source.index(f"typedef struct {name} {{")
        end = source.index(f"}} {name};", start) + len(f"}} {name};")
        out.append(source[start:end])
    for pattern in (r"typedef struct vk_range \{[^\n]*\} vk_range;", r"typedef struct vk_alloc \{[^\n]*\} vk_alloc;",
                    r"typedef struct vk_chunk \{[^\n]*\} vk_chunk;",
                    r"typedef struct vk_slice \{[^\n]*\} vk_slice;", r"static vk_block \*blocks;",
                    r"#define BLOCK_SIZE .*", r"#define LINEAR_BLOCK_SIZE .*", r"enum \{ MK_TEXTURE[^\n]*\};",
                    r"static const char \*const mk_name\[MK_KINDS\][^;]*;", r"static uint64_t mk_bytes\[MK_KINDS\];",
                    r"static uint32_t mk_count\[MK_KINDS\];", r"static uint32_t blocks_released;", r"#define FRAMES \d+",
                    r"#define RING_MIN .*", r"#define RING_MAX .*", r"static uint32_t ring_base = RING_MIN;",
                    r"static uint64_t ring_quiet_peak;", r"static uint32_t ring_quiet_frames;",
                    r"static vk_frame frames\[FRAMES\];", r"static uint32_t frame_index;",
                    r"static uint64_t ring_frame_bytes, ring_peak_bytes;[^\n]*"):
        found = re.search(pattern, source)   # no DOTALL: a ".*" stays on its line
        if not found:
            raise AssertionError(f"r10_vulkan.c no longer has {pattern}")
        out.append(found[0])
    # out: vk_block, vk_frame, vk_range, vk_alloc, vk_chunk, vk_slice, ...; each type before its users.
    order = [out[2], out[0], out[3], out[4], out[1], out[5]] + out[6:]
    return "\n".join(order)


class VulkanMemoryTest(unittest.TestCase):
    def test_pool_blocks_and_ring(self):
        source = (ROOT / "android/render10/r10_vulkan.c").read_text(encoding="utf-8")
        functions = [function(source, signature) for signature in (
            "static int memory_type(", "static void range_insert(", "static int block_take(",
            "static int mem_alloc_in(", "static int mem_alloc(VkMemoryRequirements req", "static void mem_free(vk_alloc *a)",
            "static int ring_chunk(", "static int ring_alloc(", "static void frame_reset_ring(")]
        harness = HARNESS.replace("@DECLARATIONS@", declarations(source)).replace("@FUNCTIONS@", "\n\n".join(functions))
        self.assertIn("OK allocations", self.run_harness(harness))

    def test_depth_copy_submission(self):
        source = (ROOT / "android/render10/r10_vulkan.c").read_text(encoding="utf-8")
        start = source.index("static struct {\n    VkCommandBuffer prefix, copy;")
        end = source.index("} depth_copy;") + len("} depth_copy;")
        state = source[start:end]
        code = [function(source, signature) for signature in (
            "static void keep_depth_copy(", "static uint32_t finish_commands(", "static VkCommandBuffer depth_copy_commands(")]
        harness = r'''
#include <assert.h>
#include <stdint.h>
#include <string.h>
typedef uint32_t VkCommandBuffer, VkImage;
typedef struct { VkImage image; int layout; } vk_image;
typedef struct { int depth_copy_tried; } vk_frame;
typedef struct { int tracing; } r10_device;
#define VK_NULL_HANDLE 0
#define VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL 1
#define R10_LOG(...) ((void)0)
static vk_frame frames[1];
static uint32_t frame_index, pass_timer_count;
static VkCommandBuffer cmd, upload_cmd, next_cb;
static int upload_used;
static struct { const char *op; } pass_info[2];
static void vkEndCommandBuffer(VkCommandBuffer buffer) { assert(buffer); }
static VkCommandBuffer new_cb(vk_frame *frame) { return ++next_cb; }
static int read_layout(const vk_image *dst) { return 2; }
@STATE@
@FUNCTIONS@
int main(void)
{
    r10_device d = {0};
    const vk_image src = {11, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
    const vk_image dst = {12, 2}, other = {13, 2}, uninitialized = {14, 0};
    VkCommandBuffer list[4];
    for (int outcome = 0; outcome < 3; ++outcome) {
        upload_used = 1;
        upload_cmd = 1;
        cmd = next_cb = 2;
        frames[0].depth_copy_tried = 0;
        pass_info[0].op = "copy";
        assert(depth_copy_commands(&d, &src, &uninitialized, 1) == cmd);
        assert(!depth_copy.prefix);
        VkCommandBuffer copy = depth_copy_commands(&d, &src, &dst, 1);
        assert(copy == 3 && cmd == 4);
        depth_copy.dst = dst.image;
        keep_depth_copy(src.image);  /* source writes can follow the snapshot */
        assert(depth_copy_commands(&d, &src, &other, 1) == cmd && depth_copy.copy == copy);
        if (outcome == 1) keep_depth_copy(dst.image);  /* shader read or a resource transition */
        if (outcome != 2) assert(depth_copy_commands(&d, &src, &dst, 1) == cmd);
        uint32_t n = finish_commands(list);
        assert(list[0] == upload_cmd && list[1] == 2 && list[n - 1] == cmd);
        assert(n == (outcome == 0 ? 3 : 4));
        if (outcome == 0) assert(!strcmp(pass_info[0].op, "elided depth copy"));
        else assert(list[2] == copy && !strcmp(pass_info[0].op, "copy"));
        assert(!depth_copy.prefix && !depth_copy.copy && !depth_copy.dst);
    }
    return 0;
}
'''
        self.run_harness(harness.replace("@STATE@", state).replace("@FUNCTIONS@", "\n\n".join(code)))

    def run_harness(self, harness):
        OUT.mkdir(parents=True, exist_ok=True)
        (OUT / "test.c").write_text(harness, encoding="utf-8")
        env, linker = environment("x64")
        subprocess.run([str(CLANG), "/nologo", "/O2", "/std:c11", "/clang:--target=x86_64-pc-windows-msvc", "/c",
                        str(OUT / "test.c"), "/Fo" + str(OUT / "test.obj")], cwd=ROOT, env=env, check=True)
        subprocess.run([str(linker), "/nologo", str(OUT / "test.obj"), "/OUT:" + str(OUT / "test.exe")], env=env, check=True)
        result = subprocess.run([str(OUT / "test.exe")], env=env, capture_output=True, text=True, timeout=60)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        return result.stdout


if __name__ == "__main__":
    unittest.main()

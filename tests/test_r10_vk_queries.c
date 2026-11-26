/* Host Vulkan recorder for test_r10_vk_queries.py. The generated header contains R10's query implementation. */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "query_limits.h"

enum { VK_SUCCESS, VK_NOT_READY };
enum { VK_QUERY_RESULT_64_BIT = 1, VK_QUERY_RESULT_WAIT_BIT = 2 };
enum { VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO, VK_QUERY_TYPE_OCCLUSION };
enum { D3D10_QUERY_OCCLUSION, D3D10_QUERY_OCCLUSION_PREDICATE, D3D10_QUERY_EVENT, D3D10_QUERY_TIMESTAMP };
typedef unsigned VkQueryResultFlags;
typedef int VkResult;
typedef struct { int type; const void *next; int flags, query_type, count, stats; } VkQueryPoolCreateInfo;
typedef struct { int type; void *be; uint64_t samples; } r10_query;
typedef struct { int unused; } r10_device;
#define R10_LOG(...) ((void)0)

static int device, cmd, work_recorded, upload_used;
static uint32_t frame_index, occlusion_used[FRAMES];
static uint64_t submit_serial = 1, completed_serial;
static int occlusion_pools[FRAMES], pool_count, prefix_mode, debug;
static r10_query *active_occlusion;
static struct { int active; r10_query *occlusion; } cur;
static struct query_slot { int ended, available; uint64_t value; } slots[FRAMES][QUERY_POOL_SIZE];
static unsigned host_resets, gpu_resets, last_reset_size, reads;

static void reset_pool(int pool, unsigned first, unsigned count)
{
    assert(pool > 0 && pool <= FRAMES && first + count <= QUERY_POOL_SIZE);
    memset(&slots[pool - 1][first], 0, count * sizeof(struct query_slot));
    last_reset_size = count;
}

static void host_reset(int dev, int pool, unsigned first, unsigned count)
{
    ++host_resets;
    reset_pool(pool, first, count);
}
static void (*host_reset_queries)(int, int, unsigned, unsigned);

static void vkCmdResetQueryPool(int cb, int pool, unsigned first, unsigned count)
{
    ++gpu_resets;
    reset_pool(pool, first, count);
}

static int vkCreateQueryPool(int dev, const VkQueryPoolCreateInfo *info, void *allocator, int *pool)
{
    assert(pool_count < FRAMES && info->count == QUERY_POOL_SIZE);
    *pool = ++pool_count;
    return VK_SUCCESS;
}

static void vkCmdBeginQuery(int cb, int pool, unsigned s, unsigned flags)
{
    assert(!slots[pool - 1][s].available && !slots[pool - 1][s].ended);
}

static void vkCmdEndQuery(int cb, int pool, unsigned s)
{
    assert(!slots[pool - 1][s].ended);
    slots[pool - 1][s].ended = 1;
}

static int vkGetQueryPoolResults(int dev, int pool, unsigned first, unsigned count, size_t bytes,
                                 uint64_t *results, size_t stride, VkQueryResultFlags flags)
{
    ++reads;
    assert(pool > 0 && pool <= FRAMES && first + count <= QUERY_POOL_SIZE);
    assert(stride == sizeof(uint64_t) && bytes >= count * stride);
    int status = VK_SUCCESS;
    for (unsigned i = 0; i < count; ++i) {
        struct query_slot *s = &slots[pool - 1][first + i];
        if (s->available) {
            assert(s->ended);
            results[i] = s->value;
        } else {
            /* The only blocking reads R10 requests are for work whose fence has signalled. */
            assert(!(flags & VK_QUERY_RESULT_WAIT_BIT));
            status = VK_NOT_READY;
        }
    }
    return status;
}

static void vk_failed(int error, const char *why) { fprintf(stderr, "%s (%d)\n", why, error); abort(); }
static int uploads(void) { return 0; }
static int debug_flags(void) { return debug; }
static uint64_t r10_now_ns(void) { return 5000; }
static void occlusion_pass_end(void);
static void submit_partial(void) { occlusion_pass_end(); ++submit_serial; }
static int serial_done(uint64_t serial) { return serial <= completed_serial; }
static void wait_serial(uint64_t serial) { completed_serial = serial; }

#include "r10_query_code.h"

static void publish(unsigned f, unsigned slot, uint64_t value)
{
    assert(slots[f][slot].ended);
    slots[f][slot].value = value;
    slots[f][slot].available = 1;
}

static void recycle(unsigned f)
{
    frame_index = f;
    harvest(f);
    reset_fenced_pool();
}

/* The backend has a single active occlusion query. Both supported End orders must
 * preserve each query's recorded pass segments when another Begin preempts it. */
static void check_preemption(r10_query *outer, r10_query *inner, int outer_ends_first)
{
    unsigned first = occlusion_used[frame_index];
    vk_query_begin(NULL, outer);
    vk_query_begin(NULL, inner);
    if (outer_ends_first) {
        vk_query_end(NULL, outer);
        vk_query_end(NULL, inner);
    } else {
        vk_query_end(NULL, inner);
        vk_query_end(NULL, outer);
    }
    submit_partial();
    publish(frame_index, first, 27);
    publish(frame_index, first + 1, 0);
    if (!host_reset_queries) {
        assert(!vk_query_poll(NULL, outer, 0));
        completed_serial = submit_serial - 1;
    }
    assert(vk_query_poll(NULL, inner, 0) && inner->samples == 0);
    assert(vk_query_poll(NULL, outer, 0) && outer->samples == 27);
}

static void check_frame_wrap(r10_query *q, r10_query *preempted)
{
    vk_query_begin(NULL, q);
    occlusion_pass_end(); publish(0, 0, 3);
    frame_index = 1;
    occlusion_pass_begin(); occlusion_pass_end(); publish(1, 0, 5);
    frame_index = 2;
    occlusion_pass_begin(); occlusion_pass_end(); publish(2, 0, 7);
    recycle(0);   /* same physical slot, still inside the old D3D Begin/End */
    occlusion_pass_begin(); vk_query_end(NULL, q); submit_partial();
    assert(!vk_query_poll(NULL, q, 0));
    publish(0, 0, 11);
    assert(vk_query_poll(NULL, q, 0) && q->samples == 26);

    vk_query_begin(NULL, preempted); vk_query_begin(NULL, q); vk_query_end(NULL, q);
    publish(0, 1, 51); publish(0, 2, 2); submit_partial(); recycle(0);
    assert(vk_query_poll(NULL, q, 0) && q->samples == 2);
    frame_index = 1;
    vk_query_end(NULL, preempted);
    assert(!vk_query_poll(NULL, preempted, 0));
    submit_partial(); completed_serial = submit_serial - 1;
    assert(vk_query_poll(NULL, preempted, 0) && preempted->samples == 51);
}

static void check_host_availability(r10_query *q)
{
    unsigned first = occlusion_used[frame_index];
    vk_query_begin(NULL, q);
    for (unsigned i = 1; i < 20; ++i) { occlusion_pass_end(); occlusion_pass_begin(); }
    vk_query_end(NULL, q);
    unsigned before = reads;
    assert(!vk_query_poll(NULL, q, 0) && before == reads);  /* commands still being recorded */
    submit_partial();
    for (unsigned i = 0; i < 19; ++i) publish(frame_index, first + i, 10);
    q->samples = 12345;
    for (int i = 0; i < 2; ++i) {
        assert(!vk_query_poll(NULL, q, 0) && q->samples == 12345);
        assert(pending_queries[frame_index] == q->be);
    }
    publish(frame_index, first + 19, 77);
    assert(vk_query_poll(NULL, q, 0) && q->samples == 267 && !pending_queries[frame_index]);

    /* Reissuing or freeing a query must release each of its pending range references. */
    first = occlusion_used[frame_index];
    r10_query replacement = { D3D10_QUERY_OCCLUSION_PREDICATE };
    assert(vk_query_create(NULL, &replacement));
    vk_query_begin(NULL, &replacement); vk_query_end(NULL, &replacement); submit_partial();
    vk_query_begin(NULL, &replacement); vk_query_end(NULL, &replacement);
    publish(frame_index, first, 99);
    assert(!vk_query_poll(NULL, &replacement, 1));  /* flushed, but only older issue is ready */
    publish(frame_index, first + 1, 0);
    assert(vk_query_poll(NULL, &replacement, 0) && replacement.samples == 0);
    vk_query_begin(NULL, &replacement); vk_query_end(NULL, &replacement);
    vk_query_destroy(NULL, &replacement);
    publish(frame_index, first + 2, 55);
    before = reads;
    recycle(frame_index);
    assert(reads == before);
}

int main(int argc, char **argv)
{
    assert(argc == 2);
    host_reset_queries = strstr(argv[1], "host") ? host_reset : NULL;
    prefix_mode = strstr(argv[1], "prefix") != NULL;
    panvk_options = (prefix_mode ? PANVK_QUERY_BATCHES : 0) | (host_reset_queries ? PANVK_QUERY_AVAILABILITY : 0);
    memset(slots, 1, sizeof slots);  /* newly allocated query storage has undefined contents */
    r10_query q = { D3D10_QUERY_OCCLUSION }, p = { D3D10_QUERY_OCCLUSION_PREDICATE }, event = { D3D10_QUERY_EVENT };
    assert(vk_query_create(NULL, &q) && vk_query_create(NULL, &p) && vk_query_create(NULL, &event));
    assert(host_resets == (host_reset_queries ? FRAMES : 0) && gpu_resets == (host_reset_queries ? 0 : 1));
    cur.active = 1;
    check_preemption(&q, &p, 0);
    check_preemption(&q, &p, 1);
    recycle(0);
    assert(last_reset_size == (host_reset_queries || prefix_mode ? 4 : QUERY_POOL_SIZE));
    unsigned before = gpu_resets + host_resets;
    recycle(0);
    assert(gpu_resets + host_resets == before + (!host_reset_queries && !prefix_mode));

    if (host_reset_queries) {
        check_frame_wrap(&q, &p);
        check_host_availability(&q);
    }
    cur.active = 0;
    vk_query_begin(NULL, &q); vk_query_end(NULL, &q);
    assert(!vk_query_poll(NULL, &q, 0));
    submit_partial(); assert(!vk_query_poll(NULL, &q, 0));
    completed_serial = submit_serial - 1;
    assert(vk_query_poll(NULL, &q, 0) && q.samples == 0);
    work_recorded = 1; vk_query_end(NULL, &event);
    assert(!vk_query_poll(NULL, &event, 0));
    submit_partial(); assert(!vk_query_poll(NULL, &event, 0));
    completed_serial = submit_serial - 1;
    assert(vk_query_poll(NULL, &event, 0));
    debug = 2048; vk_query_begin(NULL, &q); vk_query_end(NULL, &q);
    assert(vk_query_poll(NULL, &q, 0) && q.samples == 1000);
    vk_query_destroy(NULL, &q); vk_query_destroy(NULL, &p); vk_query_destroy(NULL, &event);
    return 0;
}

#include <stdint.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef uint64_t VkPipeline, VkPipelineLayout, VkDescriptorSetLayout, VkDescriptorSet;
typedef int r10_lib_shader, r10_shader;
typedef struct { uint64_t vs, ps; struct { uint32_t topology; } state; uint8_t other[1068]; } warm_pipeline;
typedef struct audit_key { struct audit_key *next; int retired; } audit_key;
typedef struct { uint32_t unused; } vk_binding;
static audit_key *audit_keys[1024];
static void audit_retire(audit_key *a) { ++a->retired; }
static FILE *audit_file, *recipe_file;
static int pipe_audit, device, drained, idle, destroyed[16], queued[16];
#define VK_NULL_HANDLE 0
#define DEAD_PIPELINE 1
static void present_drain(void) { drained = 1; }
static void vkDeviceWaitIdle(int d) { (void)d; assert(drained); idle = 1; }
static void vkDestroyPipeline(int d, VkPipeline h, void *a)
{
    (void)d; (void)a; assert(h == 2 || idle); assert(!destroyed[h]++);
}
static void vkDestroyPipelineLayout(int d, VkPipelineLayout h, void *a)
{
    (void)d; (void)a; assert(idle && destroyed[1] && !destroyed[h]++);
}
static void vkDestroyDescriptorSetLayout(int d, VkDescriptorSetLayout h, void *a)
{
    (void)d; (void)a; assert(idle && destroyed[4] && !destroyed[h]++);
}
static void dead_push(int kind, uint64_t h, void *mem)
{
    (void)mem; assert(kind == DEAD_PIPELINE && !queued[h]++);
}

@PROGRAM@
@TABLE@

int main(void)
{
    retained = calloc(2, sizeof *retained);
    warm_pipeline key = {0}, changed;
    r10_lib_shader vs = 1, ps = 2, different = 3;
    retained_add(&key, &vs, &ps);
    retained_pipeline *first = retained_find(&key, &vs, &ps);
    assert(first && !first->pipeline);
    changed = key;
    changed.other[1067] = 1;
    assert(!retained_find(&changed, &vs, &ps) && !retained_find(&key, &vs, &different));
    retained_add(&changed, &vs, &ps);
    retained_pipeline *second = retained_find(&changed, &vs, &ps);
    assert(second && second != first);

    vk_program worker = {.layout=4, .set_layout=5};
    assert(retained_publish(first, 1, &worker) == 1 && !worker.layout && !worker.set_layout);
    assert(retained_publish(first, 2, NULL) == 1 && destroyed[2]);
    assert(retained_publish(second, 3, NULL) == 3);
    vk_program *old = calloc(1, sizeof *old);
    vk_pipeline *alias = calloc(1, sizeof *alias), *local = calloc(1, sizeof *local);
    audit_key audit = {0};
    alias->shared = 1; alias->pipeline = 1; alias->audit = &audit;
    local->pipeline = 6; alias->next = local; old->pipelines[0] = alias;
    program_destroy(old);
    assert(audit.retired && queued[6] && !queued[1] && !destroyed[1]);
    assert(retained_find(&key, &vs, &ps)->pipeline == 1);

    vk_program live = {0}; programs[1] = &live;
    live.last = live.pipelines[4] = calloc(1, sizeof *live.last);
    live.last->shared = 1; live.last->pipeline = 1;
    pipe_shutdown();
    assert(idle && !retained && !retained_count && !live.last && !live.pipelines[4]);
    assert(destroyed[1] && destroyed[3] && destroyed[4] && destroyed[5]);
    assert(!retained_find(&key, &vs, &ps));
    pipe_shutdown();
    return 0;
}

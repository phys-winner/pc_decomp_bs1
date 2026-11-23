/* The content worker polls empty/pending async queues without the 100 ms sleep
 * used by its active/suspended paths. Let their producers and FMOD run between
 * unchanged polls, after the original completion callbacks release their locks. */
#define _GNU_SOURCE
#include "host.h"
#include <errno.h>
#include <time.h>

extern int void_machine_is_owned(void);

static const uint8_t content_poll_preimage[] = {
    0x8b,0x46,0x0c,0x3b,0xc5,0x74,0x34,0x39,0x68,0x5c,0x75,0x2f,0xe8,0x66,0x57,0xf5,
    0xff,0x8b,0xf8,0x8b,0x07,0x83,0xc0,0x04,0x50,0xff,0x15,0x60,0xc1,0xfc,0x10,0x8b,
    0x0f,0x8b,0x5f,0x08,0x83,0xc1,0x04,0x51,0xff,0x15,0x5c,0xc1,0xfc,0x10,0x3b,0xdd,
    0x74,0x09,0x8b,0x4e,0x0c,0x55,0xe8,0xac,0x43,0xf6,0xff,
};
static int content_backoff = -1;
static _Thread_local struct {
    uint32_t worker, manager, queue, count, head;
    unsigned repeated;
} content_poll;

/* Called at the original idle branch 10B65469, with registers published.
 * sub_10B63F50 has returned false and holds no guest critical section. */
void void_bioshock_content_poll(uint32_t worker)
{
    if (!void_machine_is_owned()) return;
    if (content_backoff < 0) {
        const char *value = getenv("BIOSHOCK_LOADING_BACKOFF");
        content_backoff = !(value && *value == '0') &&
            void_vm_readable(0x10B65469u, sizeof content_poll_preimage) &&
            !memcmp(GPTR(0x10B65469u), content_poll_preimage, sizeof content_poll_preimage);
        void_log("[loading] idle/pending content backoff %s (50 us after 8 unchanged polls)",
                 content_backoff ? "enabled" : "disabled");
    }
    if (!content_backoff) return;
    uint32_t manager, queue, count = 0, head = 0;
    if (worker < 0x10000u || worker > UINT32_MAX - 44u || !void_vm_readable(worker, 44) ||
        MEM32(worker + 24) || MEM32(worker + 40) ||
        !void_vm_readable(0x1128F03Cu, 4)) {
        memset(&content_poll, 0, sizeof content_poll);
        return;
    }
    manager = MEM32(worker + 12);
    queue = MEM32(0x1128F03Cu);
    if ((manager && (manager > UINT32_MAX - 96u || !void_vm_readable(manager, 96) || MEM32(manager + 92))) ||
        (queue && (queue > UINT32_MAX - 16u || !void_vm_readable(queue, 16) ||
         (count = MEM32(queue + 8)) > MEM32(queue + 12) ||
         (count && (!(head = MEM32(queue + 4)) || !void_vm_readable(head, 4)))))) {
        memset(&content_poll, 0, sizeof content_poll);
        return;
    }
    if (count) head = MEM32(head);
    if (content_poll.worker != worker || content_poll.manager != manager || content_poll.queue != queue ||
        content_poll.count != count || content_poll.head != head) {
        content_poll.worker = worker; content_poll.manager = manager; content_poll.queue = queue;
        content_poll.count = count; content_poll.head = head; content_poll.repeated = 0;
    }
    if (content_poll.repeated < 8) { ++content_poll.repeated; return; }
    struct timespec delay = { 0, 50000L };
    void_blocking_begin();
    while (nanosleep(&delay, &delay) && errno == EINTR) {}
    void_blocking_end();
}

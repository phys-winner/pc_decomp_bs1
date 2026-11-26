/* user32: window classes, one desktop of borderless top-level windows, the
 * per-thread message queues, PE string resources and the input state the
 * Android layer feeds (cursor position, key states). */
#define _GNU_SOURCE
#include "host.h"
#include "input.h"
#include <strings.h>
#include <time.h>

#define U32 "user32.dll"

#define WM_NULL 0x0000
#define WM_CREATE 0x0001
#define WM_DESTROY 0x0002
#define WM_MOVE 0x0003
#define WM_SIZE 0x0005
#define WM_ACTIVATE 0x0006
#define WM_SETFOCUS 0x0007
#define WM_KILLFOCUS 0x0008
#define WM_PAINT 0x000F
#define WM_CLOSE 0x0010
#define WM_QUIT 0x0012
#define WM_ERASEBKGND 0x0014
#define WM_SHOWWINDOW 0x0018
#define WM_ACTIVATEAPP 0x001C
#define WM_SETCURSOR 0x0020
#define WM_GETMINMAXINFO 0x0024
#define WM_WINDOWPOSCHANGING 0x0046
#define WM_WINDOWPOSCHANGED 0x0047
#define WM_NCCREATE 0x0081
#define WM_NCDESTROY 0x0082
#define WM_NCCALCSIZE 0x0083
#define WM_NCACTIVATE 0x0086
#define WM_KEYDOWN 0x0100
#define WM_KEYUP 0x0101
#define WM_CHAR 0x0102
#define WM_SYSKEYDOWN 0x0104
#define WM_SYSKEYUP 0x0105
#define WM_SYSCHAR 0x0106
#define WM_SYSCOMMAND 0x0112

/* ------------------------------------------------------------- classes */
typedef struct {
    char name[128];
    uint32_t atom, wndproc, style, extra, instance, background;
    int unicode;
} wclass_t;
static wclass_t classes[64];
static int class_count;
static uint32_t next_atom = 0xC100;

typedef struct {
    uint32_t hwnd;
    wclass_t *cls;
    uint32_t wndproc, style, exstyle, instance, parent, id, userdata;
    int x, y, w, h;
    int d3d_fullscreen_w, d3d_fullscreen_h;
    int normal_x, normal_y, normal_w, normal_h;
    int min_x, min_y, max_x, max_y;
    uint32_t show_state;
    int restore_maximized;
    int visible, unicode, alive;
    uint32_t thread, menu;
    uint8_t extra[64];
    char title[256];
} window_t;
static window_t windows[32];
static int window_count;
static uint32_t main_window, focus_window;
static uint32_t capture_window;
static pthread_mutex_t user_lock = PTHREAD_MUTEX_INITIALIZER;

static wclass_t *find_class(const char *name, uint32_t atom)
{
    for (int i = 0; i < class_count; ++i) {
        if (atom && classes[i].atom == atom) return &classes[i];
        if (name && !strcasecmp(classes[i].name, name)) return &classes[i];
    }
    return NULL;
}

static window_t *find_window(uint32_t hwnd)
{
    for (int i = 0; i < window_count; ++i) if (windows[i].alive && windows[i].hwnd == hwnd) return &windows[i];
    return NULL;
}

static int prop_slot(window_t *w, uint32_t key)
{
    uint32_t *props = (uint32_t *)w->extra;
    for (int i = 0; i < 8; ++i) if (props[2 * i] == key) return i;
    return -1;
}
static uint32_t prop_set(window_t *w, uint32_t key, uint32_t value)
{
    if (!w || !key) return 0;
    int slot = prop_slot(w, key);
    if (slot < 0) {
        uint32_t *props = (uint32_t *)w->extra;
        for (int i = 0; i < 8; ++i) if (!props[2 * i]) { slot = i; break; }
    }
    if (slot < 0) return 0;
    uint32_t *props = (uint32_t *)w->extra;
    props[2 * slot] = key;
    props[2 * slot + 1] = value;
    return 1;
}
static uint32_t prop_get(window_t *w, uint32_t key)
{
    int slot = w ? prop_slot(w, key) : -1;
    return slot < 0 ? 0 : ((uint32_t *)w->extra)[2 * slot + 1];
}
static uint32_t prop_remove(window_t *w, uint32_t key)
{
    int slot = w ? prop_slot(w, key) : -1;
    if (slot < 0) return 0;
    uint32_t *props = (uint32_t *)w->extra;
    uint32_t previous = props[2 * slot + 1];
    props[2 * slot] = props[2 * slot + 1] = 0;
    return previous;
}
WINAPI_EXPORT(U32, SetPropA, 3) { return prop_set(find_window(ARG(0)), ARG(1), ARG(2)); }
WINAPI_EXPORT(U32, SetPropW, 3) { return prop_set(find_window(ARG(0)), ARG(1), ARG(2)); }
WINAPI_EXPORT(U32, GetPropA, 2) { return prop_get(find_window(ARG(0)), ARG(1)); }
WINAPI_EXPORT(U32, GetPropW, 2) { return prop_get(find_window(ARG(0)), ARG(1)); }
WINAPI_EXPORT(U32, RemovePropA, 2) { return prop_remove(find_window(ARG(0)), ARG(1)); }
WINAPI_EXPORT(U32, RemovePropW, 2) { return prop_remove(find_window(ARG(0)), ARG(1)); }

uint32_t void_window_main(void)
{
    if (!find_window(main_window)) {
        uint32_t previous = main_window;
        main_window = find_window(focus_window) ? focus_window : 0;
        for (int i = window_count; !main_window && i-- > 0;)
            if (windows[i].alive && !windows[i].parent) main_window = windows[i].hwnd;
        if (previous != main_window)
            void_log("[input] main window recovered %08X -> %08X", previous, main_window);
    }
    return main_window;
}

uint32_t void_window_thread(uint32_t hwnd)
{
    window_t *win = find_window(hwnd);
    return win ? win->thread : 0;
}

void void_window_client_size(uint32_t hwnd, int *w, int *h)
{
    window_t *win = find_window(hwnd ? hwnd : main_window);
    *w = win && win->show_state != 2 ? win->w : 0;
    *h = win && win->show_state != 2 ? win->h : 0;
}

/* The guest string at va as UTF-8: a class atom (MAKEINTATOM) yields NULL. */
static char *class_name(uint32_t va, int wide, uint32_t *atom)
{
    *atom = 0;
    if (va < 0x10000u) { *atom = va; return NULL; }
    return wide ? void_wstr_utf8(va) : void_astr_utf8(va);
}

static uint32_t register_class(uint32_t wc, int ex, int wide)
{
    /* WNDCLASSA: style, lpfnWndProc, cbClsExtra, cbWndExtra, hInstance, hIcon,
     * hCursor, hbrBackground, lpszMenuName, lpszClassName. WNDCLASSEX has cbSize first. */
    uint32_t base = ex ? wc + 4 : wc;
    uint32_t atom;
    char *name = class_name(MEM32(base + 36), wide, &atom);
    pthread_mutex_lock(&user_lock);
    if (!name || find_class(name, 0) || class_count >= 64) {
        pthread_mutex_unlock(&user_lock);
        free(name);
        void_set_last_error(ERROR_CLASS_ALREADY_EXISTS_);
        return 0;
    }
    wclass_t *c = &classes[class_count++];
    snprintf(c->name, sizeof c->name, "%s", name);
    c->style = MEM32(base);
    c->wndproc = MEM32(base + 4);
    c->extra = MEM32(base + 12);
    c->instance = MEM32(base + 16);
    c->background = MEM32(base + 28);
    c->unicode = wide;
    c->atom = next_atom++;
    pthread_mutex_unlock(&user_lock);
    VLOG("[void] RegisterClass %s wndproc=%08X atom=%04X", name, c->wndproc, c->atom);
    free(name);
    return c->atom;
}

WINAPI_EXPORT(U32, RegisterClassA, 1) { return register_class(ARG(0), 0, 0); }
WINAPI_EXPORT(U32, RegisterClassW, 1) { return register_class(ARG(0), 0, 1); }
WINAPI_EXPORT(U32, RegisterClassExA, 1) { return register_class(ARG(0), 1, 0); }
WINAPI_EXPORT(U32, RegisterClassExW, 1) { return register_class(ARG(0), 1, 1); }

static uint32_t class_info(uint32_t name_va, uint32_t out, int wide)
{
    uint32_t atom;
    char *name = class_name(name_va, wide, &atom);
    wclass_t *c = find_class(name, atom);
    free(name);
    if (!c || !out || !void_vm_writable(out, 48)) return 0;
    MEM32(out + 4) = c->style;
    MEM32(out + 8) = c->wndproc;
    MEM32(out + 12) = 0;
    MEM32(out + 16) = 0;
    MEM32(out + 20) = c->instance;
    MEM32(out + 24) = 0;
    MEM32(out + 28) = 0;
    MEM32(out + 32) = c->background;
    MEM32(out + 36) = 0;
    MEM32(out + 40) = name_va;
    MEM32(out + 44) = 0;
    return 1;
}
WINAPI_EXPORT(U32, GetClassInfoExA, 3) { return class_info(ARG(1), ARG(2), 0); }
WINAPI_EXPORT(U32, GetClassInfoExW, 3) { return class_info(ARG(1), ARG(2), 1); }

static uint32_t class_long(uint32_t name_va, int32_t index, uint32_t value, int set, int wide)
{
    uint32_t atom;
    char *name = class_name(name_va, wide, &atom);
    wclass_t *c = find_class(name, atom);
    free(name);
    if (!c) return 0;
    uint32_t *field = NULL;
    switch (index) {
    case -24: field = &c->instance; break;
    case -12: field = &c->wndproc; break;
    case -20: field = &c->background; break;
    case -4: field = &c->style; break;
    default: return 0;
    }
    uint32_t previous = *field;
    if (set) *field = value;
    return previous;
}
WINAPI_EXPORT(U32, GetClassLongA, 2) { return class_long(ARG(0), (int32_t)ARG(1), 0, 0, 0); }
WINAPI_EXPORT(U32, GetClassLongW, 2) { return class_long(ARG(0), (int32_t)ARG(1), 0, 0, 1); }
WINAPI_EXPORT(U32, SetClassLongA, 3) { return class_long(ARG(0), (int32_t)ARG(1), ARG(2), 1, 0); }
WINAPI_EXPORT(U32, SetClassLongW, 3) { return class_long(ARG(0), (int32_t)ARG(1), ARG(2), 1, 1); }

static uint32_t unregister_class(uint32_t name_va, int wide)
{
    uint32_t atom;
    char *name = class_name(name_va, wide, &atom);
    pthread_mutex_lock(&user_lock);
    wclass_t *c = find_class(name, atom);
    if (c) c->name[0] = 0, c->atom = 0;
    pthread_mutex_unlock(&user_lock);
    free(name);
    return c != NULL;
}

WINAPI_EXPORT(U32, UnregisterClassA, 2) { return unregister_class(ARG(0), 0); }
WINAPI_EXPORT(U32, UnregisterClassW, 2) { return unregister_class(ARG(0), 1); }

WINAPI_EXPORT(U32, RegisterHotKey, 4) { return 1; }
WINAPI_EXPORT(U32, UnregisterHotKey, 2) { return 1; }

/* ------------------------------------------------------------ messages */
typedef struct { uint32_t hwnd, msg, wparam, lparam, time, x, y; } qmsg_t;
typedef struct queue {
    uint32_t thread;
    qmsg_t items[1024];
    int head, count;
    int quit;
    uint32_t quit_code, active;
} queue_t;
static queue_t queues[32];
static int queue_count;

static queue_t *queue_for(uint32_t thread)
{
    for (int i = 0; i < queue_count; ++i) if (queues[i].thread == thread) return &queues[i];
    if (queue_count >= 32) return NULL;
    queues[queue_count].thread = thread;
    return &queues[queue_count++];
}

static int queue_push(uint32_t thread, uint32_t hwnd, uint32_t msg, uint32_t wparam, uint32_t lparam)
{
    int posted = 0;
    pthread_mutex_lock(&user_lock);
    queue_t *q = queue_for(thread);
    if (q && q->count < 1024) {
        qmsg_t *m = &q->items[(q->head + q->count++) % 1024];
        m->hwnd = hwnd; m->msg = msg; m->wparam = wparam; m->lparam = lparam;
        m->time = void_tick_ms();
        void_input_cursor(&m->x, &m->y);
        posted = 1;
    } else if (q) {
        void_log("[void] message queue of thread %u is full; dropped %04X", thread, msg);
    }
    pthread_mutex_unlock(&user_lock);
    void_wait_broadcast();
    return posted;
}

/* A second press of the same button within the double-click time (500 ms)
 * and rectangle (4x4, SM_CXDOUBLECLK) becomes WM_xBUTTONDBLCLK for a window
 * whose class has CS_DBLCLKS, as Windows posts it; the press after that
 * starts a new pair. The Load game list opens a save on a double click. */
static uint32_t dblclk_translate(window_t *w, uint32_t msg, uint32_t lparam)
{
    static uint32_t last_msg, last_lparam;
    static double last_time;
    if (msg != 0x0201u && msg != 0x0204u && msg != 0x0207u) return msg;
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    double now = (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
    int dx = (int16_t)(lparam & 0xFFFF) - (int16_t)(last_lparam & 0xFFFF);
    int dy = (int16_t)(lparam >> 16) - (int16_t)(last_lparam >> 16);
    int pair = w->cls && (w->cls->style & 0x0008u) && last_msg == msg && now - last_time <= 0.5 &&
               dx >= -2 && dx <= 2 && dy >= -2 && dy <= 2;
    VLOG("[user32] button %04X at %d,%d: class style %08X%s", msg, (int16_t)(lparam & 0xFFFF),
         (int16_t)(lparam >> 16), w->cls ? w->cls->style : 0, pair ? " -> double click" : "");
    last_msg = pair ? 0 : msg;
    last_lparam = lparam;
    last_time = now;
    return pair ? msg + 2u : msg;
}

void void_post_message(uint32_t hwnd, uint32_t msg, uint32_t wparam, uint32_t lparam)
{
    window_t *capture = find_window(capture_window);
    if (capture && msg >= 0x0200 && msg <= 0x020E) hwnd = capture->hwnd;
    window_t *w = find_window(hwnd ? hwnd : main_window);
    if (!w) return;
    if (msg >= 0x0200 && msg <= 0x020E && msg != 0x020A && msg != 0x020E) {
        uint32_t x = (uint32_t)(int32_t)(int16_t)lparam - (uint32_t)w->x;
        uint32_t y = (uint32_t)(int32_t)(int16_t)(lparam >> 16) - (uint32_t)w->y;
        lparam = (x & 0xFFFF) | ((y & 0xFFFF) << 16);
    }
    msg = dblclk_translate(w, msg, lparam);
#ifdef RECOMP_TRACE
    if (msg == 0x0201u || msg == 0x0202u)
        void_log("[win32-button] hwnd=%08X capture=%08X msg=%04X wparam=%08X lparam=%08X",
            w->hwnd, capture ? capture->hwnd : 0, msg, wparam, lparam);
#endif
    queue_push(w->thread, w->hwnd, msg, wparam, lparam);
}

static int matches(const qmsg_t *m, uint32_t hwnd, uint32_t lo, uint32_t hi)
{
    if (hwnd && hwnd != 0xFFFFFFFFu && m->hwnd != hwnd) return 0;
    if (hwnd == 0xFFFFFFFFu && m->hwnd) return 0;
    if (lo || hi) return m->msg >= lo && m->msg <= hi;
    return 1;
}

/* Remove the first matching message into out. Caller holds user_lock. */
static int queue_take(queue_t *q, uint32_t hwnd, uint32_t lo, uint32_t hi, int remove, qmsg_t *out)
{
    for (int i = 0; i < q->count; ++i) {
        qmsg_t *m = &q->items[(q->head + i) % 1024];
        if (!matches(m, hwnd, lo, hi)) continue;
        *out = *m;
        if (remove) {
            for (int k = i; k > 0; --k) q->items[(q->head + k) % 1024] = q->items[(q->head + k - 1) % 1024];
            q->head = (q->head + 1) % 1024;
            --q->count;
        }
        return 1;
    }
    if (q->quit && (!lo || (WM_QUIT >= lo && WM_QUIT <= hi))) {
        out->hwnd = 0; out->msg = WM_QUIT; out->wparam = q->quit_code; out->lparam = 0;
        out->time = void_tick_ms(); out->x = out->y = 0;
        if (remove) q->quit = 0;
        return 1;
    }
    return 0;
}

static void store_msg(uint32_t va, const qmsg_t *m)
{
    MEM32(va + 0) = m->hwnd; MEM32(va + 4) = m->msg; MEM32(va + 8) = m->wparam;
    MEM32(va + 12) = m->lparam; MEM32(va + 16) = m->time; MEM32(va + 20) = m->x; MEM32(va + 24) = m->y;
}

/* The Android layer's input reaches the queue through this pump. */
static void pump_host_input(void) { void_input_pump(); }

/* A console command the port requested (the QA F10 command or travel, the cheats dialog) runs on the main thread
 * when the engine pumps its messages (PeekMessage, GetMessage): that is where Windows runs a key's console command,
 * at the top of the engine's loop. GetQueueStatus and MsgWaitForMultipleObjects only pump input: they can be
 * called from inside a wait. At a loop's yield point (recomp_yield_hook) the command ran inside whatever the guest
 * was doing: inside BinkDoFrame it closed and reopened the movie under the running decoder, which then read freed
 * or reinitialized state (the PlasmiNow crashes of the QA repro, 25 November 2026). */
static void pump_messages(void)
{
    extern void void_bioshock_poll_main_navigation(void);
    pump_host_input();
    void_bioshock_poll_main_navigation();
}

WINAPI_EXPORT(U32, PeekMessageA, 5)
{
    pump_messages();
    uint32_t out = ARG(0), hwnd = ARG(1), lo = ARG(2), hi = ARG(3), flags = ARG(4);
    qmsg_t m;
    pthread_mutex_lock(&user_lock);
    queue_t *q = queue_for(void_thread_id());
    int got = q && queue_take(q, hwnd, lo, hi, (flags & 1u) != 0, &m);
    pthread_mutex_unlock(&user_lock);
    if (got) store_msg(out, &m);
    return (uint32_t)got;
}
WINAPI_EXPORT(U32, PeekMessageW, 5)
{
    pump_messages();
    uint32_t out = ARG(0), hwnd = ARG(1), lo = ARG(2), hi = ARG(3), flags = ARG(4);
    qmsg_t m;
    pthread_mutex_lock(&user_lock);
    queue_t *q = queue_for(void_thread_id());
    int got = q && queue_take(q, hwnd, lo, hi, (flags & 1u) != 0, &m);
    pthread_mutex_unlock(&user_lock);
    if (got) store_msg(out, &m);
    return (uint32_t)got;
}

static int queue_nonempty(void *arg)
{
    queue_t *q = (queue_t *)arg;
    return q->count > 0 || q->quit;
}

static uint32_t get_message(uint32_t out, uint32_t hwnd, uint32_t lo, uint32_t hi)
{
    if (!out || !void_vm_writable(out, 28)) { void_set_last_error(998); return 0xFFFFFFFFu; }
    if (hwnd && hwnd != 0xFFFFFFFFu) {
        window_t *window = find_window(hwnd);
        if (!window || window->thread != void_thread_id()) { void_set_last_error(1400); return 0xFFFFFFFFu; }
    }
    for (;;) {
        pump_messages();
        qmsg_t m;
        pthread_mutex_lock(&user_lock);
        queue_t *q = queue_for(void_thread_id());
        int got = q && queue_take(q, hwnd, lo, hi, 1, &m);
        pthread_mutex_unlock(&user_lock);
        if (!q) { void_set_last_error(ERROR_NOT_ENOUGH_MEMORY_); return 0xFFFFFFFFu; }
        if (got) { store_msg(out, &m); return m.msg != WM_QUIT; }
        void_wait_extra_cond(0, NULL, 50, queue_nonempty, q);
    }
}
WINAPI_EXPORT(U32, GetMessageA, 4) { return get_message(ARG(0), ARG(1), ARG(2), ARG(3)); }
WINAPI_EXPORT(U32, GetMessageW, 4) { return get_message(ARG(0), ARG(1), ARG(2), ARG(3)); }

WINAPI_EXPORT(U32, GetQueueStatus, 1)
{
    pump_host_input();
    pthread_mutex_lock(&user_lock);
    queue_t *q = queue_for(void_thread_id());
    uint32_t bits = 0;
    if (q) for (int i = 0; i < q->count; ++i) {
        uint32_t msg = q->items[(q->head + i) % 1024].msg;
        if (msg >= 0x100 && msg <= 0x109) bits |= 0x0001;          /* QS_KEY */
        else if (msg >= 0x200 && msg <= 0x20E) bits |= 0x0006;     /* QS_MOUSE */
        else bits |= 0x0008;                                        /* QS_POSTMESSAGE */
    }
    pthread_mutex_unlock(&user_lock);
    return (bits << 16) | bits;
}

WINAPI_EXPORT(U32, MsgWaitForMultipleObjects, 5)
{
    uint32_t count = ARG(0), list = ARG(1), timeout = ARG(3);
    uint32_t handles[64];
    for (uint32_t i = 0; i < count && i < 63; ++i) handles[i] = MEM32(list + 4 * i);
    pump_host_input();
    pthread_mutex_lock(&user_lock);
    queue_t *q = queue_for(void_thread_id());
    pthread_mutex_unlock(&user_lock);
    return void_wait_extra_cond((int)count, handles, timeout, queue_nonempty, q);
}

static uint32_t post_thread_message(uint32_t thread, uint32_t message, uint32_t wparam, uint32_t lparam)
{
    /* Posting cannot create a target thread's USER queue. It is created by
     * Peek/GetMessage or a window operation on that thread, as on Windows. */
    int exists = 0;
    pthread_mutex_lock(&user_lock);
    for (int i = 0; i < queue_count; ++i) if (queues[i].thread == thread) { exists = 1; break; }
    pthread_mutex_unlock(&user_lock);
    if (!exists) { void_set_last_error(1444); return 0; } /* ERROR_INVALID_THREAD_ID */
    if (!queue_push(thread, 0, message, wparam, lparam)) {
        void_set_last_error(1816); /* ERROR_NOT_ENOUGH_QUOTA */
        return 0;
    }
    return 1;
}
WINAPI_EXPORT(U32, PostThreadMessageA, 4) { return post_thread_message(ARG(0), ARG(1), ARG(2), ARG(3)); }
WINAPI_EXPORT(U32, PostThreadMessageW, 4) { return post_thread_message(ARG(0), ARG(1), ARG(2), ARG(3)); }

static uint32_t post_message(uint32_t hwnd, uint32_t message, uint32_t wparam, uint32_t lparam)
{
    window_t *w = find_window(hwnd);
    if (!w && hwnd) { void_set_last_error(1400u); return 0; }
    if (!queue_push(w ? w->thread : void_thread_id(), hwnd, message, wparam, lparam)) {
        void_set_last_error(1816); return 0;
    }
    return 1;
}
WINAPI_EXPORT(U32, PostMessageA, 4) { return post_message(ARG(0), ARG(1), ARG(2), ARG(3)); }
WINAPI_EXPORT(U32, PostMessageW, 4) { return post_message(ARG(0), ARG(1), ARG(2), ARG(3)); }

WINAPI_EXPORT(U32, PostQuitMessage, 1)
{
    pthread_mutex_lock(&user_lock);
    queue_t *q = queue_for(void_thread_id());
    if (q) { q->quit = 1; q->quit_code = ARG(0); }
    pthread_mutex_unlock(&user_lock);
    void_log("[void] PostQuitMessage(%u) from %08X", ARG(0), g_cur_func);
    return 0;
}

static uint32_t send_message(uint32_t hwnd, uint32_t msg, uint32_t wparam, uint32_t lparam)
{
    window_t *w = find_window(hwnd);
#ifndef RECOMP_TRACE
    if (msg == 0x0201u) {
        static unsigned calls;
        if (calls++ < 32) void_log("[menu-win32] hwnd=%08X wparam=%08X lparam=%08X wndproc=%08X",
            hwnd, wparam, lparam, w ? w->wndproc : 0);
    }
#endif
    if (wparam == 13 && (msg == WM_KEYDOWN || msg == WM_KEYUP))
        void_log("[input] window Enter msg=%04X hwnd=%08X wndproc=%08X", msg, hwnd, w ? w->wndproc : 0);
    if (!w || !w->wndproc) return 0;
    uint32_t args[4] = { hwnd, msg, wparam, lparam };
    return void_call_guest(w->wndproc, 4, args, 1);
}

uint32_t void_send_message(uint32_t hwnd, uint32_t msg, uint32_t wparam, uint32_t lparam)
{
    return send_message(hwnd ? hwnd : main_window, msg, wparam, lparam);
}

WINAPI_EXPORT(U32, SendMessageA, 4) { return send_message(ARG(0), ARG(1), ARG(2), ARG(3)); }
WINAPI_EXPORT(U32, SendMessageW, 4) { return send_message(ARG(0), ARG(1), ARG(2), ARG(3)); }

WINAPI_EXPORT(U32, DispatchMessageA, 1)
{
    uint32_t m = ARG(0);
    if (!MEM32(m)) return 0;
    return send_message(MEM32(m), MEM32(m + 4), MEM32(m + 8), MEM32(m + 12));
}
WINAPI_EXPORT(U32, DispatchMessageW, 1)
{
    uint32_t m = ARG(0);
    if (!MEM32(m)) return 0;
    return send_message(MEM32(m), MEM32(m + 4), MEM32(m + 8), MEM32(m + 12));
}

WINAPI_EXPORT(U32, TranslateMessage, 1)
{
    uint32_t m = ARG(0), msg = MEM32(m + 4), vk = MEM32(m + 8);
    if (msg != WM_KEYDOWN && msg != WM_SYSKEYDOWN) return 0;
    uint32_t ch = void_input_vk_char(vk);
    if (!ch) return 0;
    window_t *w = find_window(MEM32(m));
    queue_push(w ? w->thread : void_thread_id(), MEM32(m), msg == WM_KEYDOWN ? WM_CHAR : WM_SYSCHAR, ch, MEM32(m + 12));
    return 1;
}

WINAPI_EXPORT(U32, RegisterWindowMessageA, 1)
{
    static char names[64][128];
    static int count;
    char *name = void_astr_utf8(ARG(0));
    uint32_t id = 0;
    pthread_mutex_lock(&user_lock);
    for (int i = 0; i < count; ++i) if (!strcasecmp(names[i], name)) id = 0xC000u + (uint32_t)i;
    if (!id && count < 64) { snprintf(names[count], 128, "%s", name); id = 0xC000u + (uint32_t)count++; }
    pthread_mutex_unlock(&user_lock);
    free(name);
    return id;
}

/* ------------------------------------------------------------- windows */
static uint32_t def_window_proc(uint32_t hwnd, uint32_t msg, uint32_t wparam, uint32_t lparam)
{
    (void)wparam; (void)lparam;
    switch (msg) {
    case WM_NCCREATE: return 1;
    case WM_CLOSE: {
        uint32_t arg = hwnd;
        (void)arg;
        window_t *w = find_window(hwnd);
        if (w) {
            send_message(hwnd, WM_DESTROY, 0, 0);
            send_message(hwnd, WM_NCDESTROY, 0, 0);
            w->alive = 0;
            if (capture_window == hwnd) capture_window = 0;
            queue_t *q = queue_for(w->thread);
            if (q && q->active == hwnd) q->active = 0;
            if (focus_window == hwnd) focus_window = 0;
            if (main_window == hwnd) main_window = 0;
        }
        return 0;
    }
    case WM_ERASEBKGND: return 1;
    case WM_SETCURSOR: return 1;
    case WM_NCACTIVATE: return 1;
    case WM_SYSCOMMAND: return 0;
    default: return 0;
    }
}

WINAPI_EXPORT(U32, DefWindowProcA, 4) { return def_window_proc(ARG(0), ARG(1), ARG(2), ARG(3)); }
WINAPI_EXPORT(U32, DefWindowProcW, 4) { return def_window_proc(ARG(0), ARG(1), ARG(2), ARG(3)); }
WINAPI_EXPORT(U32, CallWindowProcA, 5)
{
    if (!ARG(0)) return def_window_proc(ARG(1), ARG(2), ARG(3), ARG(4));
    uint32_t args[4] = { ARG(1), ARG(2), ARG(3), ARG(4) };
    return void_call_guest(ARG(0), 4, args, 1);
}
WINAPI_EXPORT(U32, CallWindowProcW, 5)
{
    if (!ARG(0)) return def_window_proc(ARG(1), ARG(2), ARG(3), ARG(4));
    uint32_t args[4] = { ARG(1), ARG(2), ARG(3), ARG(4) };
    return void_call_guest(ARG(0), 4, args, 1);
}
WINAPI_EXPORT(U32, DefMDIChildProcA, 4) { return def_window_proc(ARG(0), ARG(1), ARG(2), ARG(3)); }
WINAPI_EXPORT(U32, DefMDIChildProcW, 4) { return def_window_proc(ARG(0), ARG(1), ARG(2), ARG(3)); }

static void notify_geometry(window_t *w)
{
    uint32_t pos = void_galloc(28);
    MEM32(pos) = w->hwnd; MEM32(pos + 8) = (uint32_t)w->x; MEM32(pos + 12) = (uint32_t)w->y;
    MEM32(pos + 16) = (uint32_t)w->w; MEM32(pos + 20) = (uint32_t)w->h;
    send_message(w->hwnd, WM_WINDOWPOSCHANGED, 0, pos);
    void_gfree(pos);
    send_message(w->hwnd, WM_SIZE, w->show_state == 2 ? 1 : w->show_state == 3 ? 2 : 0,
        w->show_state == 2 ? 0 : ((uint32_t)(w->h & 0xFFFF) << 16) | (uint32_t)(w->w & 0xFFFF));
    send_message(w->hwnd, WM_MOVE, 0, ((uint32_t)(w->y & 0xFFFF) << 16) | (uint32_t)(w->x & 0xFFFF));
}

static void activate(window_t *w)
{
    queue_t *q = queue_for(w->thread);
    if (q) q->active = w->hwnd;
    if (focus_window == w->hwnd) return;
    focus_window = w->hwnd;
    send_message(w->hwnd, WM_ACTIVATEAPP, 1, 0);
    send_message(w->hwnd, WM_NCACTIVATE, 1, 0);
    send_message(w->hwnd, WM_ACTIVATE, 1, 0);       /* WA_ACTIVE */
    send_message(w->hwnd, WM_SETFOCUS, 0, 0);
}

static void save_normal_position(window_t *w)
{
    if (w->show_state == 1 && !w->d3d_fullscreen_w) {
        w->normal_x = w->x; w->normal_y = w->y;
        w->normal_w = w->w; w->normal_h = w->h;
    }
}

static void apply_fullscreen_geometry(window_t *w)
{
    if (!w->d3d_fullscreen_w || w->show_state == 2) return;
    w->x = w->y = 0; w->w = w->d3d_fullscreen_w; w->h = w->d3d_fullscreen_h;
}

extern void void_input_resize_window_clip(int x, int y, int old_w, int old_h, int w, int h);
void void_window_d3d_fullscreen(uint32_t hwnd, uint32_t width, uint32_t height)
{
    window_t *w = find_window(hwnd ? hwnd : void_window_main());
    if (!w) return;
    /* A successful windowed transition or final device release ends ownership. */
    if (!width && !height) { w->d3d_fullscreen_w = w->d3d_fullscreen_h = 0; return; }
    if (!width || !height || width > 16384 || height > 16384) return;
    if (!w->d3d_fullscreen_w) save_normal_position(w);
    w->d3d_fullscreen_w = (int)width; w->d3d_fullscreen_h = (int)height;
    if (!w->x && !w->y && w->w == (int)width && w->h == (int)height) return;
    /* D3D9's fullscreen SetWindowPos filters WndProc resize messages during device changes. */
    void_input_resize_window_clip(w->x, w->y, w->w, w->h, (int)width, (int)height);
    w->x = w->y = 0; w->w = (int)width; w->h = (int)height;
    void_log("[d3d9] fullscreen guest window %08X synchronized to %ux%u", w->hwnd, width, height);
}

static uint32_t show_window(window_t *w, uint32_t command)
{
    uint32_t previous_visible = w->visible;
    if (command > 11) return previous_visible;
    uint32_t state = w->show_state;
    switch (command) {
    case 1: case 4: state = 1; w->restore_maximized = 0; break;
    case 2: case 6: case 7: case 11:
        if (state != 2) w->restore_maximized = state == 3;
        state = 2; break;
    case 3: state = 3; w->restore_maximized = 1; break;
    case 9: state = state == 2 && w->restore_maximized ? 3 : 1; break;
    default: break; /* HIDE/SHOW/SHOWNA/SHOWDEFAULT preserve placement mode. */
    }
    int changed = state != w->show_state;
    if (changed) {
        save_normal_position(w);
        if (state == 1) {
            w->x = w->normal_x; w->y = w->normal_y; w->w = w->normal_w; w->h = w->normal_h;
        } else if (state == 3) {
            void_input_screen_size(&w->w, &w->h);
            w->x = w->y = 0;
        } else {
            /* No taskbar icons on this borderless desktop: minimized windows
             * have an empty client, while normal geometry remains saved. */
            if (w->min_x != -1) w->x = w->min_x;
            if (w->min_y != -1) w->y = w->min_y;
            w->w = w->h = 0;
        }
        w->show_state = state;
    }
    apply_fullscreen_geometry(w);
    w->style = (w->style & ~0x21000000u) | (state == 2 ? 0x20000000u : state == 3 ? 0x01000000u : 0);
    w->visible = command != 0;
    if (w->visible != (int)previous_visible) send_message(w->hwnd, WM_SHOWWINDOW, w->visible, 0);
    if (changed || (w->visible && !previous_visible)) notify_geometry(w);
    if (w->visible && state != 2 && command != 4 && command != 8) activate(w);
    return previous_visible;
}

static uint32_t create_window(uint32_t exstyle, uint32_t class_va, uint32_t title_va, uint32_t style,
    int x, int y, int cx, int cy, uint32_t parent, uint32_t menu, uint32_t instance, uint32_t param, int wide)
{
    uint32_t atom;
    char *name = class_name(class_va, wide, &atom);
    char *title = wide ? void_wstr_utf8(title_va) : void_astr_utf8(title_va);
    pthread_mutex_lock(&user_lock);
    wclass_t *c = find_class(name, atom);
    if (!c || window_count >= 32) {
        pthread_mutex_unlock(&user_lock);
        void_log("[void] CreateWindowEx: unknown class %s", name ? name : "(atom)");
        free(name); free(title);
        void_set_last_error(1407u);    /* ERROR_CANNOT_FIND_WND_CLASS */
        return 0;
    }
    window_t *w = &windows[window_count];
    memset(w, 0, sizeof *w);
    w->hwnd = 0x00010010u + 0x10u * (uint32_t)window_count++;
    w->cls = c;
    w->wndproc = c->wndproc;
    w->style = style;
    w->exstyle = exstyle;
    w->instance = instance;
    w->parent = parent;
    w->id = menu;
    int sw, sh;
    void_input_screen_size(&sw, &sh);
    w->x = x == (int)0x80000000 ? 0 : x;
    w->y = y == (int)0x80000000 ? 0 : y;
    w->w = cx == (int)0x80000000 ? sw : cx;
    w->h = cy == (int)0x80000000 ? sh : cy;
    w->normal_x = w->x; w->normal_y = w->y; w->normal_w = w->w; w->normal_h = w->h;
    w->min_x = w->min_y = w->max_x = w->max_y = -1;
    w->show_state = style & 0x20000000u ? 2 : style & 0x01000000u ? 3 : 1;
    w->restore_maximized = w->show_state == 3;
    if (w->show_state == 3) { w->x = w->y = 0; w->w = sw; w->h = sh; }
    else if (w->show_state == 2) w->w = w->h = 0;
    w->unicode = wide;
    w->alive = 1;
    w->thread = void_thread_id();
    snprintf(w->title, sizeof w->title, "%s", title ? title : "");
    if (!parent && !main_window) main_window = w->hwnd;
    pthread_mutex_unlock(&user_lock);
    void_log("[void] CreateWindowEx class=%s title=\"%s\" style=%08X ex=%08X %dx%d at %d,%d -> %08X",
        c->name, w->title, style, exstyle, w->w, w->h, w->x, w->y, w->hwnd);
    free(name); free(title);
    /* CREATESTRUCT for WM_NCCREATE/WM_CREATE. */
    uint32_t cs = void_galloc(48);
    MEM32(cs + 0) = param; MEM32(cs + 4) = instance; MEM32(cs + 8) = menu; MEM32(cs + 12) = parent;
    MEM32(cs + 16) = (uint32_t)w->h; MEM32(cs + 20) = (uint32_t)w->w;
    MEM32(cs + 24) = (uint32_t)w->y; MEM32(cs + 28) = (uint32_t)w->x;
    MEM32(cs + 32) = style; MEM32(cs + 36) = title_va; MEM32(cs + 40) = class_va; MEM32(cs + 44) = exstyle;
    uint32_t hwnd = w->hwnd;
    uint32_t minmax = void_galloc(40);
    send_message(hwnd, WM_GETMINMAXINFO, 0, minmax);
    void_gfree(minmax);
    if (!send_message(hwnd, WM_NCCREATE, 0, cs)) {
        void_log("[void] WM_NCCREATE rejected window %08X", hwnd);
        void_gfree(cs);
        w->alive = 0;
        return 0;
    }
    send_message(hwnd, WM_NCCALCSIZE, 0, 0);
    if (send_message(hwnd, WM_CREATE, 0, cs) == 0xFFFFFFFFu) {
        void_log("[void] WM_CREATE rejected window %08X", hwnd);
        void_gfree(cs);
        w->alive = 0;
        return 0;
    }
    void_gfree(cs);
    notify_geometry(w);
    if (style & 0x10000000u) {           /* WS_VISIBLE */
        w->visible = 1;
        send_message(hwnd, WM_SHOWWINDOW, 1, 0);
        activate(w);
    }
    return hwnd;
}

WINAPI_EXPORT(U32, CreateWindowExA, 12)
{
    return create_window(ARG(0), ARG(1), ARG(2), ARG(3), (int)ARG(4), (int)ARG(5), (int)ARG(6), (int)ARG(7),
        ARG(8), ARG(9), ARG(10), ARG(11), 0);
}

WINAPI_EXPORT(U32, CreateWindowExW, 12)
{
    return create_window(ARG(0), ARG(1), ARG(2), ARG(3), (int)ARG(4), (int)ARG(5), (int)ARG(6), (int)ARG(7),
        ARG(8), ARG(9), ARG(10), ARG(11), 1);
}

static uint32_t create_dialog(uint32_t proc, uint32_t parameter)
{
    if (!proc || window_count >= 32) return 0;
    pthread_mutex_lock(&user_lock);
    window_t *w = &windows[window_count];
    memset(w, 0, sizeof *w);
    w->hwnd = 0x00010010u + 0x10u * (uint32_t)window_count++;
    w->wndproc = proc;
    w->thread = void_thread_id();
    w->alive = 1;
    w->visible = 0;
    void_android_display_size(&w->w, &w->h);
    if (!main_window) main_window = w->hwnd;
    pthread_mutex_unlock(&user_lock);
    uint32_t args[4] = { w->hwnd, 0x0110u, 0, parameter }; /* WM_INITDIALOG */
    void_call_guest(proc, 4, args, 1);
    return w->hwnd;
}

WINAPI_EXPORT(U32, CreateDialogParamA, 5) { return create_dialog(ARG(3), ARG(4)); }
WINAPI_EXPORT(U32, CreateDialogParamW, 5) { return create_dialog(ARG(3), ARG(4)); }
WINAPI_EXPORT(U32, DialogBoxParamA, 5) { return create_dialog(ARG(3), ARG(4)); }
WINAPI_EXPORT(U32, DialogBoxParamW, 5) { return create_dialog(ARG(3), ARG(4)); }
WINAPI_EXPORT(U32, EndDialog, 2)
{
    window_t *w = find_window(ARG(0));
    if (!w) return 0;
    w->alive = 0;
    if (main_window == w->hwnd) main_window = 0;
    return 1;
}
WINAPI_EXPORT(U32, GetDlgItem, 2)
{
    window_t *w = find_window(ARG(0));
    return w && ARG(1) ? w->hwnd : 0;
}
WINAPI_EXPORT(U32, GetParent, 1) { return 0; }
WINAPI_EXPORT(U32, EnableWindow, 2)
{
    window_t *w = find_window(ARG(0));
    if (!w) return 0;
    int previous = w->visible;
    w->visible = ARG(1) != 0;
    return previous;
}

WINAPI_EXPORT(U32, DestroyWindow, 1)
{
    window_t *w = find_window(ARG(0));
    if (!w) { void_set_last_error(1400u); return 0; }
    send_message(w->hwnd, WM_DESTROY, 0, 0);
    send_message(w->hwnd, WM_NCDESTROY, 0, 0);
    w->alive = 0;
    if (capture_window == w->hwnd) capture_window = 0;
    queue_t *q = queue_for(w->thread);
    if (q && q->active == w->hwnd) q->active = 0;
    if (focus_window == w->hwnd) focus_window = 0;
    if (main_window == w->hwnd) main_window = 0;
    return 1;
}

WINAPI_EXPORT(U32, ShowWindow, 2)
{
    window_t *w = find_window(ARG(0));
    if (!w) return 0;
    return show_window(w, ARG(1));
}

WINAPI_EXPORT(U32, UpdateWindow, 1)
{
    window_t *w = find_window(ARG(0));
    if (!w) return 0;
    send_message(w->hwnd, WM_PAINT, 0, 0);
    return 1;
}

WINAPI_EXPORT(U32, SetWindowPos, 7)
{
    window_t *w = find_window(ARG(0));
    if (!w) return 0;
    int old_x=w->x,old_y=w->y,old_w=w->w,old_h=w->h;
    uint32_t flags = ARG(6);
    if (!(flags & 0x0002u)) { w->x = (int)ARG(2); w->y = (int)ARG(3); }   /* SWP_NOMOVE */
    if (!(flags & 0x0001u)) { w->w = (int)ARG(4); w->h = (int)ARG(5); }   /* SWP_NOSIZE */
    if (flags & 0x0040u) w->visible = 1;                                  /* SWP_SHOWWINDOW */
    if (flags & 0x0080u) w->visible = 0;                                  /* SWP_HIDEWINDOW */
    apply_fullscreen_geometry(w);
    save_normal_position(w);
    VLOG("[void] SetWindowPos(%08X) -> %dx%d at %d,%d flags %X", w->hwnd, w->w, w->h, w->x, w->y, flags);
    if (!w->d3d_fullscreen_w || old_x!=w->x || old_y!=w->y || old_w!=w->w || old_h!=w->h) notify_geometry(w);
    return 1;
}

WINAPI_EXPORT(U32, MoveWindow, 6)
{
    window_t *w = find_window(ARG(0));
    if (!w) return 0;
    int old_x=w->x,old_y=w->y,old_w=w->w,old_h=w->h;
    w->x = (int)ARG(1); w->y = (int)ARG(2); w->w = (int)ARG(3); w->h = (int)ARG(4);
    apply_fullscreen_geometry(w);
    save_normal_position(w);
    if (!w->d3d_fullscreen_w || old_x!=w->x || old_y!=w->y || old_w!=w->w || old_h!=w->h) notify_geometry(w);
    return 1;
}

WINAPI_EXPORT(U32, AdjustWindowRectEx, 4)
{
    /* Windows here are borderless: the client rectangle is the window. */
    return 1;
}
WINAPI_EXPORT(U32, AdjustWindowRect, 3) { return 1; }

static uint32_t store_rect(uint32_t out, int l, int t, int r, int b)
{
    MEM32(out) = (uint32_t)l; MEM32(out + 4) = (uint32_t)t; MEM32(out + 8) = (uint32_t)r; MEM32(out + 12) = (uint32_t)b;
    return 1;
}

WINAPI_EXPORT(U32, GetClientRect, 2)
{
    window_t *w = find_window(ARG(0));
    if (!w) { void_set_last_error(1400u); return 0; }
    return store_rect(ARG(1), 0, 0, w->show_state == 2 ? 0 : w->w, w->show_state == 2 ? 0 : w->h);
}

WINAPI_EXPORT(U32, GetWindowRect, 2)
{
    window_t *w = find_window(ARG(0));
    if (!w) { void_set_last_error(1400u); return 0; }
    return store_rect(ARG(1), w->x, w->y, w->x + w->w, w->y + w->h);
}

WINAPI_EXPORT(U32, GetWindowPlacement, 2)
{
    window_t *w = find_window(ARG(0));
    if (!w) { void_set_last_error(1400); return 0; }
    uint32_t out = ARG(1);
    if (!out || !void_vm_writable(out, 44)) { void_set_last_error(998); return 0; }
    /* Native Windows accepts legacy callers with any incoming length, then
     * writes exactly the Win32 44-byte layout and its canonical length. */
    MEM32(out) = 44;
    MEM32(out + 4) = w->show_state == 3 || (w->show_state == 2 && w->restore_maximized) ? 2 : 0;
    MEM32(out + 8) = w->show_state;
    MEM32(out + 12) = w->min_x; MEM32(out + 16) = w->min_y;
    MEM32(out + 20) = w->max_x; MEM32(out + 24) = w->max_y;
    MEM32(out + 28) = w->normal_x; MEM32(out + 32) = w->normal_y;
    MEM32(out + 36) = (uint32_t)w->normal_x + (uint32_t)w->normal_w;
    MEM32(out + 40) = (uint32_t)w->normal_y + (uint32_t)w->normal_h;
    return 1;
}

WINAPI_EXPORT(U32, SetWindowPlacement, 2)
{
    window_t *w = find_window(ARG(0));
    if (!w) { void_set_last_error(1400); return 0; }
    uint32_t input = ARG(1), placement[11];
    if (!input || !void_vm_readable(input, 44)) { void_set_last_error(998); return 0; }
    memcpy(placement, GPTR(input), sizeof placement);
    if (placement[0] != 44 || placement[2] > 11) { void_set_last_error(ERROR_INVALID_PARAMETER_); return 0; }
    int64_t width = (int64_t)(int32_t)placement[9] - (int32_t)placement[7];
    int64_t height = (int64_t)(int32_t)placement[10] - (int32_t)placement[8];
    if (width < 0 || width > INT32_MAX || height < 0 || height > INT32_MAX) {
        void_set_last_error(ERROR_INVALID_PARAMETER_); return 0;
    }
    int x = (int32_t)placement[7], y = (int32_t)placement[8], sw, sh;
    void_input_screen_size(&sw, &sh);
    /* Keep a completely off-screen restored window visible, as documented. */
    if ((int64_t)x + width <= 0) x = 0;
    else if (x >= sw) x = width < sw ? sw - (int)width : 0;
    if ((int64_t)y + height <= 0) y = 0;
    else if (y >= sh) y = height < sh ? sh - (int)height : 0;
    if (w->show_state != 2 || (placement[1] & 1u)) {
        w->min_x = (int32_t)placement[3]; w->min_y = (int32_t)placement[4];
    }
    if (w->show_state == 2) w->restore_maximized = (placement[1] & 2u) != 0;
    w->normal_x = x; w->normal_y = y; w->normal_w = (int)width; w->normal_h = (int)height;
    if (w->show_state == 1) { w->x = x; w->y = y; w->w = width; w->h = height; }
    uint32_t old_state = w->show_state;
    int old_visible = w->visible;
    show_window(w, placement[2]);
    if (w->show_state == 2) w->restore_maximized = (placement[1] & 2u) != 0;
    if (!w->d3d_fullscreen_w && w->show_state == 1 && old_state == 1 && (old_visible || !w->visible)) notify_geometry(w);
    return 1;
}

WINAPI_EXPORT(U32, ClientToScreen, 2)
{
    window_t *w = find_window(ARG(0));
    if (!w) return 0;
    MEM32(ARG(1)) += (uint32_t)w->x;
    MEM32(ARG(1) + 4) += (uint32_t)w->y;
    return 1;
}

WINAPI_EXPORT(U32, ScreenToClient, 2)
{
    window_t *w = find_window(ARG(0));
    if (!w) return 0;
    MEM32(ARG(1)) -= (uint32_t)w->x;
    MEM32(ARG(1) + 4) -= (uint32_t)w->y;
    return 1;
}

WINAPI_EXPORT(U32, MapWindowPoints, 4)
{
    uint32_t from = ARG(0), to = ARG(1), points = ARG(2), count = ARG(3);
    window_t *source = from && from != 0x00010000u ? find_window(from) : NULL;
    window_t *target = to && to != 0x00010000u ? find_window(to) : NULL;
    if ((from && from != 0x00010000u && !source) || (to && to != 0x00010000u && !target)) {
        void_set_last_error(1400); return 0;
    }
    if ((source && (source->exstyle & 0x00400000u)) || (target && (target->exstyle & 0x00400000u))) {
        void_set_last_error(ERROR_NOT_SUPPORTED_); return 0; /* Mirrored RTL desktop is not implemented. */
    }
    uint64_t bytes = (uint64_t)count * 8;
    if (bytes > UINT32_MAX) { void_set_last_error(ERROR_INVALID_PARAMETER_); return 0; }
    if (count && (!points || !void_vm_readable(points, (uint32_t)bytes) || !void_vm_writable(points, (uint32_t)bytes))) {
        void_set_last_error(998); return 0;
    }
    uint32_t dx = (uint32_t)(source ? source->x : 0) - (uint32_t)(target ? target->x : 0);
    uint32_t dy = (uint32_t)(source ? source->y : 0) - (uint32_t)(target ? target->y : 0);
    for (uint32_t i = 0; i < count; ++i) { MEM32(points + i*8) += dx; MEM32(points + i*8 + 4) += dy; }
    return (dx & 0xFFFF) | ((dy & 0xFFFF) << 16);
}

WINAPI_EXPORT(U32, GetCapture, 0)
{
    window_t *w = find_window(capture_window);
    return w && w->thread == void_thread_id() ? w->hwnd : 0;
}
WINAPI_EXPORT(U32, SetCapture, 1)
{
    uint32_t hwnd = ARG(0);
    window_t *w = find_window(hwnd), *previous = find_window(capture_window);
    if (hwnd && (!w || w->thread != void_thread_id())) { void_set_last_error(1400); return 0; }
    uint32_t old = previous && previous->thread == void_thread_id() ? previous->hwnd : 0;
    if (hwnd || old) capture_window = hwnd;
    if (old && old != hwnd) send_message(old, 0x0215, 0, hwnd); /* WM_CAPTURECHANGED */
    return old;
}
WINAPI_EXPORT(U32, ReleaseCapture, 0)
{
    window_t *w = find_window(capture_window);
    if (w && w->thread == void_thread_id()) {
        uint32_t old = capture_window; capture_window = 0;
        send_message(old, 0x0215, 0, 0);
    }
    return 1;
}

static uint32_t get_window_long(void)
{
    window_t *w = find_window(ARG(0));
    if (!w) { void_set_last_error(1400u); return 0; }
    int32_t index = (int32_t)ARG(1);
    switch (index) {
    case -4: return w->wndproc;
    case -6: return w->instance;
    case -8: return w->parent;
    case -12: return w->id;
    case -16: return (w->style & ~0x10000000u) | (w->visible ? 0x10000000u : 0);
    case -20: return w->exstyle;
    case -21: return w->userdata;
    default:
        if (index >= 0 && index + 4 <= 64) { uint32_t v; memcpy(&v, w->extra + index, 4); return v; }
        return 0;
    }
}
WINAPI_EXPORT(U32, GetWindowLongA, 2) { return get_window_long(); }
WINAPI_EXPORT(U32, GetWindowLongW, 2) { return get_window_long(); }

static uint32_t set_window_long(void)
{
    window_t *w = find_window(ARG(0));
    if (!w) { void_set_last_error(1400u); return 0; }
    int32_t index = (int32_t)ARG(1);
    uint32_t value = ARG(2), old = 0;
    switch (index) {
    case -4: old = w->wndproc; w->wndproc = value; break;
    case -16: old = w->style; w->style = value; break;
    case -20: old = w->exstyle; w->exstyle = value; break;
    case -21: old = w->userdata; w->userdata = value; break;
    case -12: old = w->id; w->id = value; break;
    default:
        if (index >= 0 && index + 4 <= 64) { memcpy(&old, w->extra + index, 4); memcpy(w->extra + index, &value, 4); }
        break;
    }
    return old;
}

WINAPI_EXPORT(U32, SetWindowLongA, 3) { return set_window_long(); }
WINAPI_EXPORT(U32, SetWindowLongW, 3) { return set_window_long(); }

WINAPI_EXPORT(U32, SetWindowTextW, 2)
{
    char *t = void_wstr_utf8(ARG(1));
    pthread_mutex_lock(&user_lock);
    window_t *w = find_window(ARG(0));
    if (w && t) snprintf(w->title, sizeof w->title, "%s", t);
    pthread_mutex_unlock(&user_lock);
    VLOG("[void] SetWindowTextW: %s", t ? t : "");
    free(t);
    return w != NULL;
}

WINAPI_EXPORT(U32, SetWindowTextA, 2)
{
    char *t = void_astr_utf8(ARG(1));
    pthread_mutex_lock(&user_lock);
    window_t *w = find_window(ARG(0));
    if (w && t) snprintf(w->title, sizeof w->title, "%s", t);
    pthread_mutex_unlock(&user_lock);
    free(t);
    return w != NULL;
}

static uint32_t get_window_text(int wide)
{
    int32_t capacity = (int32_t)ARG(2);
    if (capacity <= 0) {
        if (capacity < 0) void_set_last_error(ERROR_INVALID_PARAMETER_);
        return 0;
    }
    char title[sizeof windows[0].title];
    pthread_mutex_lock(&user_lock);
    window_t *w = find_window(ARG(0));
    if (w) memcpy(title, w->title, sizeof title);
    else title[0] = 0;
    pthread_mutex_unlock(&user_lock);
    title[sizeof title - 1] = 0;
    if (!w) void_set_last_error(1400u);
    unsigned char *end = (unsigned char *)title;
    uint32_t units = 0, limit = (uint32_t)capacity - 1;
    uint16_t high = 0;
    while (*end && units < limit) {
        unsigned char *next = end + 1;
        while ((*next & 0xC0u) == 0x80u) ++next;
        uint32_t width = wide && *end >= 0xF0u && next - end == 4 ? 2u : 1u;
        if (width > limit - units) {
            /* Win32 truncates UTF-16 by code units, including a lone high surrogate. */
            uint32_t cp = ((uint32_t)(end[0] & 7u) << 18) | ((uint32_t)(end[1] & 63u) << 12) |
                          ((uint32_t)(end[2] & 63u) << 6) | (end[3] & 63u);
            high = (uint16_t)(0xD800u + ((cp - 0x10000u) >> 10));
            break;
        }
        units += width;
        end = next;
    }
    *end = 0;
    uint32_t out = ARG(1), bytes = (units + (high != 0) + 1u) * (wide ? 2u : 1u);
    if (!out || !void_vm_writable(out, bytes)) {
        void_set_last_error(ERROR_INVALID_ADDRESS_);
        return 0;
    }
    uint32_t written = wide ? void_utf8_to_gw(title, out, units + 1) : void_utf8_to_ga(title, out, units + 1);
    if (high) { MEM16(out + 2 * written++) = high; MEM16(out + 2 * written) = 0; }
    return written;
}

WINAPI_EXPORT(U32, GetWindowTextA, 3) { return get_window_text(0); }
WINAPI_EXPORT(U32, GetWindowTextW, 3) { return get_window_text(1); }

WINAPI_EXPORT(U32, GetForegroundWindow, 0) { return focus_window ? focus_window : main_window; }
WINAPI_EXPORT(U32, GetActiveWindow, 0) { queue_t *q = queue_for(void_thread_id()); return q ? q->active : 0; }
WINAPI_EXPORT(U32, SetActiveWindow, 1)
{
    window_t *w = ARG(0) ? find_window(ARG(0)) : NULL;
    if (ARG(0) && !w) { void_set_last_error(1400u); return 0; }
    queue_t *q = queue_for(void_thread_id());
    uint32_t old = q ? q->active : 0;
    if (w && w->thread == void_thread_id()) {
        if (w->style & 0x40000000u) return 0;   /* WS_CHILD cannot be active. */
        activate(w);
        return old;
    }
    if (q) q->active = 0;
    if (old && focus_window == old) {
        focus_window = 0;
        send_message(old, WM_NCACTIVATE, 0, 0);
        send_message(old, WM_ACTIVATE, 0, 0);
        send_message(old, WM_KILLFOCUS, 0, 0);
    }
    return w ? 0 : old;   /* A foreign thread's window cannot become this queue's active window. */
}
WINAPI_EXPORT(U32, GetFocus, 0) { return focus_window; }
WINAPI_EXPORT(U32, SetForegroundWindow, 1) { return 1; }
WINAPI_EXPORT(U32, SetFocus, 1) { uint32_t old = focus_window; focus_window = ARG(0); return old; }
WINAPI_EXPORT(U32, GetMenu, 1) { window_t *w = find_window(ARG(0)); return w ? w->menu : 0; }
WINAPI_EXPORT(U32, SetMenu, 2)
{
    window_t *w = find_window(ARG(0));
    if (!w) { void_set_last_error(1400); return 0; }
    uint32_t previous = w->menu;
    w->menu = ARG(1);
    return previous;
}
WINAPI_EXPORT(U32, IsWindow, 1) { return find_window(ARG(0)) != NULL; }
WINAPI_EXPORT(U32, GetWindowThreadProcessId, 2)
{
    window_t *window = find_window(ARG(0));
    if (!window) { void_set_last_error(1400); return 0; }
    uint32_t process = ARG(1);
    if (process && !void_vm_writable(process, 4)) { void_set_last_error(998); return 0; }
    /* Windows in this host belong to its single virtual process, whose
     * Kernel32 GetCurrentProcessId is 0x100; the stored creator TID may differ
     * from the thread making this query. */
    if (process) MEM32(process) = 0x100u;
    return window->thread;
}
WINAPI_EXPORT(U32, IsWindowVisible, 1) { window_t *w = find_window(ARG(0)); return w && w->visible; }
WINAPI_EXPORT(U32, IsIconic, 1) { window_t *w = find_window(ARG(0)); return w && w->show_state == 2; }
WINAPI_EXPORT(U32, IsZoomed, 1) { window_t *w = find_window(ARG(0)); return w && w->show_state == 3; }
WINAPI_EXPORT(U32, GetDesktopWindow, 0) { return 0x00010000u; }

/* ------------------------------------------------------------- monitors */
/* Same stable system handle returned by IDirect3D9::GetAdapterMonitor. */
#define PRIMARY_MONITOR_ 0x00070001u

static uint32_t monitor_for_rect(int64_t left, int64_t top, int64_t right, int64_t bottom, uint32_t flags)
{
    if (flags > 2) { void_set_last_error(1004); return 0; } /* ERROR_INVALID_FLAGS */
    int width, height; void_input_screen_size(&width, &height);
    int hit;
    /* Native MonitorFromRect treats an empty/inverted rectangle as its
     * top-left point. Screen right/bottom edges remain exclusive. */
    if (right <= left || bottom <= top)
        hit = left >= 0 && left < width && top >= 0 && top < height;
    else hit = left < width && top < height && right > 0 && bottom > 0;
    return hit || flags ? PRIMARY_MONITOR_ : 0;
}

WINAPI_EXPORT(U32, MonitorFromWindow, 2)
{
    uint32_t hwnd = ARG(0), flags = ARG(1);
    if (flags > 2) { void_set_last_error(1004); return 0; }
    if (hwnd == 0x00010000u) return PRIMARY_MONITOR_; /* Existing GetDesktopWindow. */
    if (!hwnd) return flags ? PRIMARY_MONITOR_ : 0;
    window_t *window = find_window(hwnd);
    if (!window) { void_set_last_error(1400); return 0; }
    if (window->show_state == 2)
        return monitor_for_rect(window->normal_x, window->normal_y,
            (int64_t)window->normal_x + window->normal_w, (int64_t)window->normal_y + window->normal_h, flags);
    return monitor_for_rect(window->x, window->y, (int64_t)window->x + window->w,
        (int64_t)window->y + window->h, flags);
}

/* POINT is passed by value: two guest DWORDs, followed by dwFlags. */
WINAPI_EXPORT(U32, MonitorFromPoint, 3)
{
    int32_t x = (int32_t)ARG(0), y = (int32_t)ARG(1);
    return monitor_for_rect(x, y, x, y, ARG(2));
}

WINAPI_EXPORT(U32, MonitorFromRect, 2)
{
    if (ARG(1) > 2) { void_set_last_error(1004); return 0; }
    uint32_t rect = ARG(0);
    if (!rect || !void_vm_readable(rect, 16)) { void_set_last_error(998); return 0; }
    return monitor_for_rect((int32_t)MEM32(rect), (int32_t)MEM32(rect + 4),
        (int32_t)MEM32(rect + 8), (int32_t)MEM32(rect + 12), ARG(1));
}

static uint32_t get_monitor_info(uint32_t monitor, uint32_t out, int wide)
{
    if (monitor != PRIMARY_MONITOR_) { void_set_last_error(1461); return 0; } /* ERROR_INVALID_MONITOR_HANDLE */
    if (!out || !void_vm_readable(out, 4)) { void_set_last_error(998); return 0; }
    uint32_t size = MEM32(out), extended_size = wide ? 104 : 72;
    if (size != 40 && size != extended_size) { void_set_last_error(ERROR_INVALID_PARAMETER_); return 0; }
    if (!void_vm_writable(out, size)) { void_set_last_error(998); return 0; }
    int width, height; void_input_screen_size(&width, &height);
    store_rect(out + 4, 0, 0, width, height);
    /* The single Android guest desktop has no reserved taskbar/work area. */
    store_rect(out + 20, 0, 0, width, height);
    MEM32(out + 36) = 1; /* MONITORINFOF_PRIMARY */
    if (size == extended_size) {
        static const char device[] = "\\\\.\\DISPLAY1";
        if (wide) {
            for (unsigned i = 0; i < sizeof device; ++i) MEM16(out + 40 + 2 * i) = (uint8_t)device[i];
        } else memcpy(GPTR(out + 40), device, sizeof device);
    }
    return 1;
}
WINAPI_EXPORT(U32, GetMonitorInfoA, 2) { return get_monitor_info(ARG(0), ARG(1), 0); }
WINAPI_EXPORT(U32, GetMonitorInfoW, 2) { return get_monitor_info(ARG(0), ARG(1), 1); }

/* ------------------------------------------------------------- metrics */
WINAPI_EXPORT(U32, GetSystemMetrics, 1)
{
    int sw, sh;
    void_input_screen_size(&sw, &sh);
    switch (ARG(0)) {
    case 0: case 16: case 78: return (uint32_t)sw;    /* SM_CXSCREEN, SM_CXFULLSCREEN, SM_CXVIRTUALSCREEN */
    case 1: case 17: case 79: return (uint32_t)sh;
    case 2: case 3: return 17;                        /* scroll bars */
    case 4: return 23;                                /* SM_CYCAPTION */
    case 5: case 6: return 1;                         /* borders */
    case 7: case 8: return 3;                         /* dialog frame */
    case 11: case 12: return 32;                      /* icon */
    case 13: case 14: return 32;                      /* cursor */
    case 19: return 1;                                /* SM_MOUSEPRESENT */
    case 23: return 0;                                /* SM_SWAPBUTTON */
    case 32: case 33: return 4;                       /* frame */
    case 36: case 37: return 4;                       /* double click */
    case 43: return 3;                                /* SM_CMOUSEBUTTONS */
    case 75: return 1;                                /* SM_MOUSEWHEELPRESENT */
    case 76: case 77: return 0;                       /* virtual screen origin */
    case 80: return 1;                                /* SM_CMONITORS */
    case 0x1000: return 0;                            /* SM_REMOTESESSION */
    default: return 0;
    }
}

/* -------------------------------------------------------- cursor & keys */
WINAPI_EXPORT(U32, GetCursorPos, 1)
{
    if (!ARG(0) || !void_vm_writable(ARG(0), 8)) { void_set_last_error(998); return 0; }
    uint32_t x, y;
    void_input_cursor(&x, &y);
    if (x != 1200 || y != 540) {
        static unsigned cursor_trace;
        if (cursor_trace++ < 32) void_log("[cursor] GetCursorPos=%d,%d caller=%08X", (int)x, (int)y, g_cur_func);
    }
    MEM32(ARG(0)) = x;
    MEM32(ARG(0) + 4) = y;
    return 1;
}

WINAPI_EXPORT(U32, SetCursorPos, 2) { void_input_set_cursor((int)ARG(0), (int)ARG(1)); return 1; }

static int cursor_count;
extern void void_input_cursor_visible(int shown);
static uint32_t current_cursor = 0x00020001u;
static uint8_t shared_cursors[256] = {1};
WINAPI_EXPORT(U32, ShowCursor, 1)
{
    cursor_count += ARG(0) ? 1 : -1;
    void_input_cursor_visible(cursor_count >= 0);
    return (uint32_t)cursor_count;
}

extern int void_input_clip_cursor(const int32_t *rect);
extern void void_input_clip_bounds(int32_t rect[4]);
WINAPI_EXPORT(U32, ClipCursor, 1)
{
    uint32_t input = ARG(0); int32_t rect[4];
    if (input) {
        if (!void_vm_readable(input, 16)) { void_set_last_error(998); return 0; }
        memcpy(rect, GPTR(input), sizeof rect);
    }
    if (!void_input_clip_cursor(input ? rect : NULL)) { void_set_last_error(ERROR_ACCESS_DENIED_); return 0; }
    return 1;
}
WINAPI_EXPORT(U32, GetClipCursor, 1)
{
    uint32_t out = ARG(0); int32_t rect[4];
    if (!out || !void_vm_writable(out, 16)) { void_set_last_error(998); return 0; }
    void_input_clip_bounds(rect); memcpy(GPTR(out), rect, sizeof rect); return 1;
}
WINAPI_EXPORT(U32, GetCursorInfo, 1)
{
    uint32_t out = ARG(0);
    if (!out || !void_vm_readable(out, 4)) { void_set_last_error(998); return 0; }
    if (MEM32(out) != 20) { void_set_last_error(ERROR_INVALID_PARAMETER_); return 0; }
    if (!void_vm_writable(out, 20)) { void_set_last_error(998); return 0; }
    uint32_t x, y; void_input_cursor(&x, &y);
    MEM32(out) = 0; /* Native Win32 output, independent of the input cbSize. */
    MEM32(out + 4) = current_cursor && cursor_count >= 0 ? 1 : 0;
    MEM32(out + 8) = current_cursor; MEM32(out + 12) = x; MEM32(out + 16) = y;
    return 1;
}
static int shared_cursor(uint32_t handle)
{
    return handle >= 0x00020001u && handle <= 0x00020100u && shared_cursors[handle - 0x00020001u];
}
WINAPI_EXPORT(U32, SetCursor, 1)
{
    if (ARG(0) && !shared_cursor(ARG(0))) { void_set_last_error(1402u); return 0; }
    uint32_t old = current_cursor;
    current_cursor = ARG(0);
    return old;
}
WINAPI_EXPORT(U32, GetCursor, 0) { return current_cursor; }
WINAPI_EXPORT(U32, GetKeyState, 1)
{
    uint32_t vk = ARG(0) & 0xFF;
    int16_t state = void_input_key_state(vk);
    if (vk == 13 && state < 0) {
        static unsigned enter_logs;
        if (enter_logs++ < 12) void_log("[input] Enter down state=%04X", (unsigned)(uint16_t)state);
    }
    return (uint32_t)(int32_t)state;
}
WINAPI_EXPORT(U32, GetAsyncKeyState, 1) { return (uint32_t)(int32_t)void_input_key_state(ARG(0) & 0xFF); }

static const uint16_t virtual_scan_codes[256] = {
    [0x08] = 0x0E, [0x09] = 0x0F, [0x0C] = 0x4C, [0x0D] = 0x1C,
    [0x10] = 0x2A, [0x11] = 0x1D, [0x12] = 0x38,
    [0x14] = 0x3A, [0x1B] = 0x01, [0x20] = 0x39,
    [0x21] = 0xE049, [0x22] = 0xE051, [0x23] = 0xE04F, [0x24] = 0xE047,
    [0x25] = 0xE04B, [0x26] = 0xE048, [0x27] = 0xE04D, [0x28] = 0xE050,
    [0x2C] = 0xE037, [0x2D] = 0xE052, [0x2E] = 0xE053,
    [0x30] = 0x0B, [0x31] = 0x02, [0x32] = 0x03, [0x33] = 0x04,
    [0x34] = 0x05, [0x35] = 0x06, [0x36] = 0x07, [0x37] = 0x08,
    [0x38] = 0x09, [0x39] = 0x0A,
    [0x41] = 0x1E, [0x42] = 0x30, [0x43] = 0x2E, [0x44] = 0x20,
    [0x45] = 0x12, [0x46] = 0x21, [0x47] = 0x22, [0x48] = 0x23,
    [0x49] = 0x17, [0x4A] = 0x24, [0x4B] = 0x25, [0x4C] = 0x26,
    [0x4D] = 0x32, [0x4E] = 0x31, [0x4F] = 0x18, [0x50] = 0x19,
    [0x51] = 0x10, [0x52] = 0x13, [0x53] = 0x1F, [0x54] = 0x14,
    [0x55] = 0x16, [0x56] = 0x2F, [0x57] = 0x11, [0x58] = 0x2D,
    [0x59] = 0x15, [0x5A] = 0x2C,
    [0x5B] = 0xE05B, [0x5C] = 0xE05C, [0x5D] = 0xE05D,
    [0x60] = 0x52, [0x61] = 0x4F, [0x62] = 0x50, [0x63] = 0x51,
    [0x64] = 0x4B, [0x65] = 0x4C, [0x66] = 0x4D, [0x67] = 0x47,
    [0x68] = 0x48, [0x69] = 0x49, [0x6A] = 0x37, [0x6B] = 0x4E,
    [0x6D] = 0x4A, [0x6E] = 0x53, [0x6F] = 0xE035,
    [0x70] = 0x3B, [0x71] = 0x3C, [0x72] = 0x3D, [0x73] = 0x3E,
    [0x74] = 0x3F, [0x75] = 0x40, [0x76] = 0x41, [0x77] = 0x42,
    [0x78] = 0x43, [0x79] = 0x44, [0x7A] = 0x57, [0x7B] = 0x58,
    [0x90] = 0x45, [0x91] = 0x46,
    [0xA0] = 0x2A, [0xA1] = 0x36, [0xA2] = 0x1D, [0xA3] = 0xE01D,
    [0xA4] = 0x38, [0xA5] = 0xE038,
    [0xA6] = 0xE06A, [0xA7] = 0xE069, [0xA8] = 0xE067, [0xA9] = 0xE068,
    [0xAA] = 0xE065, [0xAB] = 0xE066, [0xAC] = 0xE032, [0xAD] = 0xE020,
    [0xAE] = 0xE02E, [0xAF] = 0xE030, [0xB0] = 0xE019, [0xB1] = 0xE010,
    [0xB2] = 0xE024, [0xB3] = 0xE022, [0xB4] = 0xE06C, [0xB5] = 0xE06D,
    [0xB6] = 0xE06B, [0xB7] = 0xE021,
    [0xBA] = 0x27, [0xBB] = 0x0D, [0xBC] = 0x33, [0xBD] = 0x0C,
    [0xBE] = 0x34, [0xBF] = 0x35, [0xC0] = 0x29,
    [0xDB] = 0x1A, [0xDC] = 0x2B, [0xDD] = 0x1B, [0xDE] = 0x28,
};

static uint32_t virtual_key_scan(uint32_t code)
{
    return code < 256 ? virtual_scan_codes[code] : 0;
}

static uint32_t virtual_scan_key(uint32_t code, int extended_first)
{
    if (!(code & 0xFFu)) return 0;
    if ((code & 0xFFu) >= 0x47 && (code & 0xFFu) <= 0x53 && (code & 0xFFu) != 0x4C)
        extended_first = 1;
    if (extended_first) {
        for (uint32_t vk = 0; vk < 256; ++vk)
            if ((virtual_scan_codes[vk] & 0xFF00u) == 0xE000u && (virtual_scan_codes[vk] & 0xFFu) == (code & 0xFFu))
                return vk;
    }
    for (uint32_t vk = 0; vk < 256; ++vk)
        if (!(virtual_scan_codes[vk] & 0xFF00u) && (virtual_scan_codes[vk] & 0xFFu) == (code & 0xFFu))
            return vk;
    if (!extended_first) {
        for (uint32_t vk = 0; vk < 256; ++vk)
            if ((virtual_scan_codes[vk] & 0xFF00u) == 0xE000u && (virtual_scan_codes[vk] & 0xFFu) == (code & 0xFFu))
                return vk;
    }
    return 0;
}

static uint32_t map_virtual_key(uint32_t code, uint32_t type)
{
    switch (type) {
    case 0: return virtual_key_scan(code) & 0xFFu;
    case 1: return virtual_scan_key(code, 0);
    case 2: return virtual_key_scan(code);
    case 3: return virtual_scan_key(code, (code & 0xFF00u) == 0xE000u);
    case 4: return virtual_scan_key(code & 0xFFu, 0);
    default: return 0;
    }
}

WINAPI_EXPORT(U32, MapVirtualKeyA, 2) { return map_virtual_key(ARG(0), ARG(1)); }
WINAPI_EXPORT(U32, MapVirtualKeyW, 2) { return map_virtual_key(ARG(0), ARG(1)); }

static const char *const virtual_key_names[256] = {
    [0x08] = "Backspace", [0x09] = "Tab", [0x0C] = "Clear", [0x0D] = "Enter",
    [0x10] = "Shift", [0x11] = "Ctrl", [0x12] = "Alt", [0x13] = "Pause",
    [0x14] = "Caps Lock", [0x1B] = "Esc", [0x20] = "Space",
    [0x21] = "Page Up", [0x22] = "Page Down", [0x23] = "End", [0x24] = "Home",
    [0x25] = "Left", [0x26] = "Up", [0x27] = "Right", [0x28] = "Down",
    [0x29] = "Select", [0x2A] = "Print", [0x2B] = "Execute",
    [0x2C] = "Print Screen", [0x2D] = "Insert", [0x2E] = "Delete", [0x2F] = "Help",
    [0x5B] = "Left Windows", [0x5C] = "Right Windows", [0x5D] = "Application", [0x5F] = "Sleep",
    [0x60] = "Num 0", [0x61] = "Num 1", [0x62] = "Num 2", [0x63] = "Num 3",
    [0x64] = "Num 4", [0x65] = "Num 5", [0x66] = "Num 6", [0x67] = "Num 7",
    [0x68] = "Num 8", [0x69] = "Num 9", [0x6A] = "Num *", [0x6B] = "Num +",
    [0x6C] = "Separator", [0x6D] = "Num -", [0x6E] = "Num .", [0x6F] = "Num /",
    [0x90] = "Num Lock", [0x91] = "Scroll Lock",
    [0xA0] = "Left Shift", [0xA1] = "Right Shift", [0xA2] = "Left Ctrl", [0xA3] = "Right Ctrl",
    [0xA4] = "Left Alt", [0xA5] = "Right Alt",
    [0xA6] = "Browser Back", [0xA7] = "Browser Forward", [0xA8] = "Browser Refresh",
    [0xA9] = "Browser Stop", [0xAA] = "Browser Search", [0xAB] = "Browser Favorites",
    [0xAC] = "Browser Home", [0xAD] = "Volume Mute", [0xAE] = "Volume Down", [0xAF] = "Volume Up",
    [0xB0] = "Next Track", [0xB1] = "Previous Track", [0xB2] = "Stop Media",
    [0xB3] = "Play/Pause", [0xB4] = "Start Mail", [0xB5] = "Select Media",
    [0xB6] = "Launch Application 1", [0xB7] = "Launch Application 2",
};

static uint32_t key_name_text(uint32_t lparam, uint32_t output, int32_t capacity, int wide)
{
    uint32_t scan = (lparam >> 16) & 0xFFu;
    if (!scan || scan == 0xC5 || capacity <= 0 || !output) return 0;
    int extended = (lparam & 0x01000000u) != 0;
    uint32_t vk = virtual_scan_key(scan, extended);
    if (!extended && scan >= 0x47 && scan <= 0x53 && scan != 0x4C) {
        for (vk = 0; vk < 256; ++vk)
            if (!(virtual_scan_codes[vk] & 0xFF00u) && (virtual_scan_codes[vk] & 0xFFu) == scan) break;
    }
    if (scan == 0x45) vk = 0x13;
    if (vk >= 256) return 0;
    char text[16];
    const char *name = virtual_key_names[vk];
    if ((vk >= '0' && vk <= '9') || (vk >= 'A' && vk <= 'Z')) {
        text[0] = (char)vk; text[1] = 0; name = text;
    } else if (vk >= 0x70 && vk <= 0x7B) {
        snprintf(text, sizeof text, "F%u", vk - 0x70 + 1); name = text;
    } else {
        switch (vk) {
        case 0xBA: name = ";"; break; case 0xBB: name = "="; break;
        case 0xBC: name = ","; break; case 0xBD: name = "-"; break;
        case 0xBE: name = "."; break; case 0xBF: name = "/"; break;
        case 0xC0: name = "`"; break; case 0xDB: name = "["; break;
        case 0xDC: name = "\\"; break; case 0xDD: name = "]"; break;
        case 0xDE: name = "'"; break;
        default: break;
        }
    }
    if (!name) return 0;
    uint32_t length = (uint32_t)strlen(name);
    uint32_t units = length < (uint32_t)capacity ? length : (uint32_t)capacity;
    int terminated = length < (uint32_t)capacity;
    if (!void_vm_writable(output, (units + (terminated ? 1u : 0u)) * (wide ? 2u : 1u))) return 0;
    for (uint32_t i = 0; i < units; ++i) {
        if (wide) MEM16(output + 2 * i) = (uint8_t)name[i];
        else MEM8(output + i) = (uint8_t)name[i];
    }
    if (terminated) {
        if (wide) MEM16(output + 2 * units) = 0;
        else MEM8(output + units) = 0;
    }
    return units;
}

WINAPI_EXPORT(U32, GetKeyNameTextA, 3) { return key_name_text(ARG(0), ARG(1), (int32_t)ARG(2), 0); }
WINAPI_EXPORT(U32, GetKeyNameTextW, 3) { return key_name_text(ARG(0), ARG(1), (int32_t)ARG(2), 1); }

WINAPI_EXPORT(U32, GetKeyboardState, 1)
{
    if (!ARG(0) || !void_vm_writable(ARG(0), 256)) { void_set_last_error(ERROR_INVALID_PARAMETER_); return 0; }
    for (int i = 0; i < 256; ++i) MEM8(ARG(0) + i) = (uint8_t)void_input_key_state((uint32_t)i);
    return 1;
}
WINAPI_EXPORT(U32, SetKeyboardState, 1)
{
    if (!ARG(0) || !void_vm_readable(ARG(0), 256)) { void_set_last_error(ERROR_INVALID_PARAMETER_); return 0; }
    void_input_set_keyboard(GPTR(ARG(0)));
    return 1;
}
static struct { uint32_t from, to; } input_attachments[32];
WINAPI_EXPORT(U32, AttachThreadInput, 3)
{
    uint32_t from = ARG(0), to = ARG(1);
    if (!from || !to || from == to) { void_set_last_error(ERROR_INVALID_PARAMETER_); return 0; }
    int attach = ARG(2) != 0, found = -1, empty = -1;
    pthread_mutex_lock(&user_lock);
    for (int i = 0; i < 32; ++i) {
        if (input_attachments[i].from == from && input_attachments[i].to == to) found = i;
        if (!input_attachments[i].from && empty < 0) empty = i;
    }
    int result = 0;
    if (attach) {
        if (found >= 0) result = 1;
        else if (empty >= 0) { input_attachments[empty] = (typeof(input_attachments[0])){from, to}; result = 1; }
    } else if (found >= 0) {
        input_attachments[found].from = input_attachments[found].to = 0;
        result = 1;
    }
    pthread_mutex_unlock(&user_lock);
    return result;
}
static uint32_t load_shared_cursor(uint32_t instance, uint32_t name)
{
    if (instance || name >= 0x10000u) { void_set_last_error(ERROR_NOT_SUPPORTED_); return 0; }
    switch (name) {
    case 32512: case 32513: case 32514: case 32515: case 32516:
    case 32640: case 32641: case 32642: case 32643: case 32644: case 32645:
    case 32646: case 32648: case 32649: case 32650: case 32651: case 32671: case 32672:
        shared_cursors[name & 0xFFu] = 1;
        return 0x00020001u + (name & 0xFFu);
    default: void_set_last_error(1814u); return 0;
    }
}
WINAPI_EXPORT(U32, LoadCursorA, 2) { return load_shared_cursor(ARG(0), ARG(1)); }
WINAPI_EXPORT(U32, LoadCursorW, 2) { return load_shared_cursor(ARG(0), ARG(1)); }
WINAPI_EXPORT(U32, LoadCursorFromFileA, 1) { void_set_last_error(ERROR_NOT_SUPPORTED_); return 0; }
WINAPI_EXPORT(U32, DestroyCursor, 1)
{
    if (!shared_cursor(ARG(0))) { void_set_last_error(1402u); return 0; }
    if (current_cursor == ARG(0)) { void_set_last_error(170u); return 0; }   /* ERROR_BUSY */
    return 1;   /* Shared system cursors remain valid for the module lifetime. */
}
WINAPI_EXPORT(U32, LoadIconW, 2) { return 0x00030001u + (ARG(1) & 0xFFu); }
WINAPI_EXPORT(U32, LoadIconA, 2) { return 0x00030001u + (ARG(1) & 0xFFu); }

WINAPI_EXPORT(U32, CharLowerA, 1)
{
    uint32_t address = ARG(0);
    if (address < 0x10000u) return (address & 0xFF00u) | void_ansi_lower((uint8_t)address);
    uint32_t end = address;
    for (;;) {
        if (!void_vm_readable(end, 1)) { void_set_last_error(ERROR_INVALID_ADDRESS_); return 0; }
        if (!MEM8(end)) break;
        if (end == UINT32_MAX) { void_set_last_error(ERROR_INVALID_ADDRESS_); return 0; }
        ++end;
    }
    if (!void_vm_writable(address, end - address + 1)) { void_set_last_error(ERROR_INVALID_ADDRESS_); return 0; }
    for (uint32_t p = address; p < end; ++p) MEM8(p) = void_ansi_lower(MEM8(p));
    return address;
}

/* ---------------------------------------------------------- resources */
/* pe_resources.c: the same bounded lookup used by the kernel32 resource APIs. */
uint32_t void_resource_find(uint32_t module, uint32_t type, uint32_t name, uint32_t *size);

WINAPI_EXPORT(U32, LoadStringW, 4)
{
    uint32_t module = ARG(0), id = ARG(1), buffer = ARG(2), cap = ARG(3);
    uint32_t size = 0;
    uint32_t block = void_resource_find(module, 6u, id / 16u + 1u, &size);
    if (!block) { if (cap) MEM16(buffer) = 0; return 0; }
    uint32_t p = block;
    for (uint32_t i = 0; i < (id & 15u); ++i) p += 2u + 2u * MEM16(p);
    uint32_t len = MEM16(p);
    if (!cap) { MEM32(buffer) = p + 2; return len; }   /* read-only pointer form */
    uint32_t n = len < cap - 1 ? len : cap - 1;
    memcpy(GPTR(buffer), GPTR(p + 2), 2 * n);
    MEM16(buffer + 2 * n) = 0;
    return n;
}

WINAPI_EXPORT(U32, LoadStringA, 4)
{
    uint32_t module = ARG(0), id = ARG(1), buffer = ARG(2), cap = ARG(3);
    uint32_t size = 0;
    uint32_t block = void_resource_find(module, 6u, id / 16u + 1u, &size);
    if (!block || !cap) { if (cap) MEM8(buffer) = 0; return 0; }
    uint32_t p = block;
    for (uint32_t i = 0; i < (id & 15u); ++i) p += 2u + 2u * MEM16(p);
    uint32_t len = MEM16(p), n = len < cap - 1 ? len : cap - 1;
    for (uint32_t i = 0; i < n; ++i) {
        uint8_t b;
        void_u16_to_ansi(MEM16(p + 2 + 2 * i), &b);
        MEM8(buffer + i) = b;
    }
    MEM8(buffer + n) = 0;
    return n;
}

/* --------------------------------------------------------- message boxes */
static uint32_t message_box(uint32_t text_va, uint32_t caption_va, uint32_t type, int wide)
{
    char *text = wide ? void_wstr_utf8(text_va) : void_astr_utf8(text_va);
    char *caption = wide ? void_wstr_utf8(caption_va) : void_astr_utf8(caption_va);
    void_log("[void] MessageBox(\"%s\", \"%s\", %08X) from %08X", caption ? caption : "", text ? text : "", type, g_cur_func);
    free(text); free(caption);
    /* No interactive dialogs: the affirmative default the guest offers. */
    switch (type & 0xFu) {
    case 1: return 1;      /* MB_OKCANCEL -> IDOK */
    case 2: return 3;      /* MB_ABORTRETRYIGNORE -> IDABORT */
    case 3: case 4: return 6;   /* YESNO(CANCEL) -> IDYES */
    case 5: return 2;      /* MB_RETRYCANCEL -> IDCANCEL */
    default: return 1;
    }
}

WINAPI_EXPORT(U32, MessageBoxA, 4) { return message_box(ARG(1), ARG(2), ARG(3), 0); }
WINAPI_EXPORT(U32, MessageBoxW, 4) { return message_box(ARG(1), ARG(2), ARG(3), 1); }

WINAPI_EXPORT(U32, DrawTextA, 5)
{
    char *text = void_astr_utf8(ARG(1));
    VLOG("[void] DrawTextA(%08X, \"%s\")", ARG(0), text ? text : "");
    free(text);
    return 16;
}

WINAPI_EXPORT(U32, GetDC, 1) { return 0x00040001u; }
WINAPI_EXPORT(U32, ReleaseDC, 2) { return 1; }
/* shortcut: GDI splash bitmaps are unavailable; add bitmap surfaces if game rendering requires them. */
WINAPI_EXPORT(U32, LoadImageA, 6)
{
    void_log("[user32] LoadImageA type %u unavailable (caller %08X)", ARG(2), g_cur_func);
    void_set_last_error(ERROR_NOT_SUPPORTED_);
    return 0;
}
WINAPI_EXPORT(U32, InvalidateRect, 3) { return 1; }
WINAPI_EXPORT(U32, ValidateRect, 2) { return 1; }

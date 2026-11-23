/* Opaque input contexts. Composition is empty until an Android IME session exists. */
#include "host.h"

typedef struct imm_context {
    uint32_t token, hwnd, open;
    struct imm_context *next;
} imm_context;
static imm_context *contexts;
static uint32_t next_token = 0x494D0000u;
static pthread_mutex_t imm_lock = PTHREAD_MUTEX_INITIALIZER;

static imm_context *find_context(uint32_t token)
{
    for (imm_context *c = contexts; c; c = c->next) if (c->token == token) return c;
    return NULL;
}

WINAPI_EXPORT("imm32.dll", ImmGetContext, 1)
{
    uint32_t window = ARG(0), result = 0;
    if (!window) return 0;
    if (!void_window_thread(window)) { void_set_last_error(1400); return 0; }
    pthread_mutex_lock(&imm_lock);
    for (imm_context *c = contexts; c; c = c->next) if (c->hwnd == window) { result = c->token; break; }
    pthread_mutex_unlock(&imm_lock);
    return result;
}

WINAPI_EXPORT("imm32.dll", ImmCreateContext, 0)
{
    pthread_mutex_lock(&imm_lock);
    imm_context *c = calloc(1, sizeof *c);
    if (c) {
        c->token = next_token;
        next_token += 4;
        c->next = contexts;
        contexts = c;
    }
    pthread_mutex_unlock(&imm_lock);
    if (!c) { void_set_last_error(14); return 0; }
    return c->token;
}

WINAPI_EXPORT("imm32.dll", ImmAssociateContext, 2)
{
    uint32_t window = ARG(0), token = ARG(1), previous = 0;
    if (window && !void_window_thread(window)) { void_set_last_error(1400); return 0; }
    pthread_mutex_lock(&imm_lock);
    imm_context *c = find_context(token);
    if (token && c) {
        previous = c->hwnd;
        c->hwnd = window;
    }
    pthread_mutex_unlock(&imm_lock);
    return previous;
}

WINAPI_EXPORT("imm32.dll", ImmReleaseContext, 2)
{
    pthread_mutex_lock(&imm_lock);
    int found = find_context(ARG(1)) != NULL;
    pthread_mutex_unlock(&imm_lock);
    return found;
}

WINAPI_EXPORT("imm32.dll", ImmDestroyContext, 1)
{
    pthread_mutex_lock(&imm_lock);
    imm_context **link = &contexts;
    while (*link && (*link)->token != ARG(0)) link = &(*link)->next;
    imm_context *old = *link;
    if (old) { *link = old->next; free(old); }
    pthread_mutex_unlock(&imm_lock);
    return old != NULL;
}

WINAPI_EXPORT("imm32.dll", ImmGetCompositionStringW, 4)
{
    pthread_mutex_lock(&imm_lock);
    int valid = find_context(ARG(0)) != NULL;
    pthread_mutex_unlock(&imm_lock);
    return valid ? 0 : -1;
}

WINAPI_EXPORT("imm32.dll", ImmGetOpenStatus, 1)
{
    pthread_mutex_lock(&imm_lock);
    imm_context *c = find_context(ARG(0));
    uint32_t result = c ? c->open : 0;
    pthread_mutex_unlock(&imm_lock);
    return result;
}

WINAPI_EXPORT("imm32.dll", ImmSetOpenStatus, 2)
{
    pthread_mutex_lock(&imm_lock);
    imm_context *c = find_context(ARG(0));
    if (c) c->open = ARG(1) != 0;
    pthread_mutex_unlock(&imm_lock);
    return c != NULL;
}

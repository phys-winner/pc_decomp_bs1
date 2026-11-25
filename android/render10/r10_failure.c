/* The launcher's renderer failure policy (owner's decision, 24 November 2026;
 * launcher/native/replacements/renderer_failure.h), both backends.
 *
 * When the launcher's renderer cannot start (its driver did not load or is another device; the instance,
 * device, surface, swap chain or EGL context could not be made; a required feature is missing) or its GPU
 * device is lost (VK_ERROR_DEVICE_LOST, EGL_CONTEXT_LOST), this start stops: no backend replaces another and
 * no driver the chosen one. r10_renderer_failed writes the record PORT_RENDERER_FAILURE names (open/write/close
 * only), keeps the reason for GameActivity, which polls nativeRendererFailure every half second and hands it to
 * Launch.rendererFailed (the launcher opens and offers the next renderer of its list; the process ends), and
 * parks the failing thread. Should the activity not act (it is in the background and may not start the
 * launcher), the process ends by itself after a while; the launcher then shows the offer when it opens. A device
 * loss more than renderers.failureWindowSeconds after the first presented frame is recorded the same way; the
 * launcher then shows it as an ordinary failure without the offer.
 *
 * QA switch, off by default: `adb shell setprop debug.bioshock.renderer_fail start` makes the selected renderer
 * report a start failure, `lost` a device loss 120 frames after the first present; `start:<id>` / `lost:<id>`
 * only when this start's renderer is <id> (gles, vulkan, vulkan-turnip, vulkan-panvk), so that the renderer the
 * launcher offers next starts normally. */
#include "r10.h"
#include "replacements/renderer_failure.h"
#include <jni.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/system_properties.h>
#include <time.h>
#include <unistd.h>

static char start_error[PORT_RENDERER_FAILURE_REASON_MAX];
static char failure_text[PORT_RENDERER_FAILURE_REASON_MAX + 8];
static atomic_int frames_shown, failing, published;

void r10_renderer_failure_init(void)
{
    static int done;
    if (done) return;
    done = 1;
    port_renderer_failure_init();
}

void r10_start_error(const char *format, ...)
{
    if (start_error[0]) return;                     /* the first reason is the cause */
    va_list args;
    va_start(args, format);
    vsnprintf(start_error, sizeof start_error, format, args);
    va_end(args);
}

const char *r10_start_error_text(void) { return start_error; }

/* The launcher's id of this start's renderer (android_main.c sets both variables from it). */
static const char *renderer_id(void)
{
    const char *mode = getenv("BIOSHOCK_RENDERER"), *driver = getenv("BIOSHOCK_VULKAN_DRIVER");
    if (!mode || strcmp(mode, "vulkan")) return "gles";
    return driver && !strcmp(driver, "turnip") ? "vulkan-turnip" : driver && !strcmp(driver, "panvk") ? "vulkan-panvk" : "vulkan";
}

/* debug.bioshock.renderer_fail as "<kind>[:<renderer id>]": 1 when it asks for `kind` with this start's renderer. */
static int qa_failure(const char *kind)
{
    char value[PROP_VALUE_MAX] = "";
    if (__system_property_get("debug.bioshock.renderer_fail", value) <= 0) return 0;
    size_t n = strlen(kind);
    if (strncmp(value, kind, n) || (value[n] && value[n] != ':')) return 0;
    return !value[n] || !strcmp(value + n + 1, renderer_id());
}

int r10_renderer_qa_start(void) { return qa_failure("start"); }

int r10_renderer_has_presented(void) { return atomic_load(&frames_shown) > 0; }

/* r10_note_present: eglSwapBuffers true; vkQueuePresentKHR VK_SUCCESS or VK_SUBOPTIMAL_KHR. */
void r10_renderer_presented(void)
{
    port_renderer_presented();
    if (atomic_fetch_add(&frames_shown, 1) + 1 == 120 && qa_failure("lost"))
        r10_renderer_failed(1, strncmp(renderer_id(), "vulkan", 6)
            ? "QA debug.bioshock.renderer_fail: simulated EGL_CONTEXT_LOST"
            : "QA debug.bioshock.renderer_fail: simulated VK_ERROR_DEVICE_LOST (-4)");
}

static void *watchdog(void *arg)
{
    (void)arg;
    struct timespec wait = { 15, 0 };
    while (nanosleep(&wait, &wait)) {}
    void_log("[r10] renderer failure: the activity did not end the game, the process ends (the launcher shows the offer when it opens)");
    _exit(3);
}

void r10_renderer_failed(int device_lost, const char *reason)
{
    /* One report: a lost device fails every call that follows on any thread. */
    if (!atomic_exchange(&failing, 1)) {
        R10_LOG("RENDERER_FAILED %s (%s, %d frames presented): %s", device_lost ? "device lost" : "did not start",
                renderer_id(), atomic_load(&frames_shown), reason);
        int written = port_renderer_failed(reason, device_lost);
        if (written) R10_LOG("renderer failure record not written (%d): started without the launcher?", written);
        snprintf(failure_text, sizeof failure_text, "%s\n%s", device_lost ? "lost" : "start", reason ? reason : "");
        atomic_store(&published, 1);
        pthread_t thread;
        if (pthread_create(&thread, NULL, watchdog, NULL) == 0) pthread_detach(thread);
        else _exit(3);
    }
    for (;;) pause();
}

/* GameActivity's poll: "start\n<reason>" or "lost\n<reason>" once the renderer failed, else null. */
JNIEXPORT jstring JNICALL Java_org_bioshock_port_GameActivity_nativeRendererFailure(JNIEnv *env, jclass type)
{
    (void)type;
    if (!atomic_load(&published)) return NULL;
    char text[sizeof failure_text];
    snprintf(text, sizeof text, "%s", failure_text);
    for (char *c = text; *c; ++c) if ((unsigned char)*c >= 0x80) *c = '?';   /* modified UTF-8 for NewStringUTF */
    return (*env)->NewStringUTF(env, text);
}

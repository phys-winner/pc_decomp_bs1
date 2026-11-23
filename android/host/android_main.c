/* Surface lifecycle and ordinary Win32 input; no game addresses are patched here. */
#ifdef UT3_ANDROID_APP
#define _GNU_SOURCE
#include "host.h"
#include "input.h"
#include <aaudio/AAudio.h>
#include <android/native_window.h>
#include <android/native_window_jni.h>
#include <errno.h>
#include <dlfcn.h>
#include <jni.h>
#include <android/asset_manager_jni.h>
#include <limits.h>
#include <time.h>
#include <sys/stat.h>
#include <unistd.h>

int ut3_game_main(const char *, const char *, const char *, int);
static ANativeWindow *window;
static pthread_mutex_t window_lock = PTHREAD_MUTEX_INITIALIZER;
static void update_guest_clock(void);
static pthread_cond_t window_cond = PTHREAD_COND_INITIALIZER;
static uint32_t window_serial, window_acked;
static int display_width, display_height, game_started;
static float selected_fps = 60;
static int benchmark_start;
static int game_result = INT_MIN, frame_ready;
static uint64_t pause_state;
static float pointer_remainder_x, pointer_remainder_y;
static char game_dir[1024], data_dir[1024], image_dir[1024];
static JavaVM *crypto_vm;
static jclass crypto_class;
static jmethodID crypto_transform, crypto_adapter_mac;
static jclass settings_class;
static jmethodID settings_callback;
extern void ut3_openal_pause(int paused) __attribute__((weak));
extern void ut3_openal_log_stats(void) __attribute__((weak));
extern void ut3_adpf_cancel(int blocked);
extern void ut3_adpf_thread_close(void);
extern void ut3_adpf_set_target(float fps);
extern void d3d_gl_set_frame_limit(float fps) __attribute__((weak));

static int crypto_exception(JNIEnv *env)
{
    if (!(*env)->ExceptionCheck(env)) return 0;
    (*env)->ExceptionClear(env);
    return 1;
}

JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM *vm, void *reserved)
{
    (void)reserved;
    JNIEnv *env = NULL;
    if ((*vm)->GetEnv(vm, (void **)&env, JNI_VERSION_1_6) != JNI_OK) return JNI_ERR;
    jclass local = (*env)->FindClass(env, "org/bioshock/port/PortCrypto");
    int failed = crypto_exception(env);
    if (!local || failed) return JNI_ERR;
    crypto_class = (jclass)(*env)->NewGlobalRef(env, local);
    (*env)->DeleteLocalRef(env, local);
    failed = crypto_exception(env);
    if (!crypto_class || failed) return JNI_ERR;
    crypto_transform = (*env)->GetStaticMethodID(env, crypto_class, "transform", "(Z[B[B[B)[[B");
    failed = crypto_exception(env);
    if (!failed && crypto_transform)
        crypto_adapter_mac = (*env)->GetStaticMethodID(env, crypto_class, "getAdapterMacHex", "()Ljava/lang/String;");
    failed |= crypto_exception(env);
    if (!crypto_transform || !crypto_adapter_mac || failed) {
        (*env)->DeleteGlobalRef(env, crypto_class); crypto_class = NULL;
        return JNI_ERR;
    }
    crypto_vm = vm;
    return JNI_VERSION_1_6;
}

JNIEXPORT void JNICALL JNI_OnUnload(JavaVM *vm, void *reserved)
{
    (void)reserved;
    JNIEnv *env = NULL;
    if ((*vm)->GetEnv(vm, (void **)&env, JNI_VERSION_1_6) == JNI_OK) {
        if (crypto_class) (*env)->DeleteGlobalRef(env, crypto_class);
        if (settings_class) (*env)->DeleteGlobalRef(env, settings_class);
    }
    settings_class = NULL; settings_callback = NULL;
    crypto_class = NULL; crypto_vm = NULL;
}

int void_android_port_setting(const char *key, const char *value, char *out, size_t cap)
{
    if (!out || !cap) return EINVAL;
    out[0] = 0;
    if (!key || strnlen(key, 65) > 64 || (value && strnlen(value, 129) > 128)) return EINVAL;
    if (!crypto_vm || !settings_class || !settings_callback) return ENOSYS;
    JNIEnv *env = NULL;
    int attached = 0, error = EINVAL;
    jint status = (*crypto_vm)->GetEnv(crypto_vm, (void **)&env, JNI_VERSION_1_6);
    if (status == JNI_EDETACHED) {
        if ((*crypto_vm)->AttachCurrentThread(crypto_vm, &env, NULL) != JNI_OK) return EIO;
        attached = 1;
    } else if (status != JNI_OK) return EIO;
    jstring name = (*env)->NewStringUTF(env, key), input = NULL, result = NULL;
    if (!name || crypto_exception(env)) { error = ENOMEM; goto cleanup; }
    if (value) {
        input = (*env)->NewStringUTF(env, value);
        if (!input || crypto_exception(env)) { error = ENOMEM; goto cleanup; }
    }
    result = (jstring)(*env)->CallStaticObjectMethod(env, settings_class, settings_callback, name, input);
    if (crypto_exception(env) || !result) goto cleanup;
    const char *text = (*env)->GetStringUTFChars(env, result, NULL);
    if (!text || crypto_exception(env)) { error = ENOMEM; goto cleanup; }
    size_t bytes = strlen(text);
    if (bytes >= cap) error = EOVERFLOW;
    else { memcpy(out, text, bytes + 1); error = 0; }
    (*env)->ReleaseStringUTFChars(env, result, text);
cleanup:
    if (result) (*env)->DeleteLocalRef(env, result);
    if (input) (*env)->DeleteLocalRef(env, input);
    if (name) (*env)->DeleteLocalRef(env, name);
    crypto_exception(env);
    if (attached) (*crypto_vm)->DetachCurrentThread(crypto_vm);
    return error;
}

static void crypto_wipe_array(JNIEnv *env, jbyteArray array)
{
    static const jbyte zeros[4096];
    if (!array) return;
    jsize size = (*env)->GetArrayLength(env, array);
    if (crypto_exception(env)) return;
    for (jsize offset = 0; offset < size;) {
        jsize count = size - offset < (jsize)sizeof zeros ? size - offset : (jsize)sizeof zeros;
        (*env)->SetByteArrayRegion(env, array, offset, count, zeros);
        if (crypto_exception(env)) return;
        offset += count;
    }
}

/* crypt32.c has already copied all guest inputs and released the machine.
 * Only native heap arrays cross this JNI boundary. Never enter the VM here. */
int void_android_crypt(int protect, const uint8_t *input, uint32_t input_bytes,
    const uint8_t *entropy, uint32_t entropy_bytes, const uint8_t *description, uint32_t description_bytes,
    uint8_t **output, uint32_t *output_bytes, uint8_t **output_description, uint32_t *output_description_bytes)
{
    if (!crypto_vm || !crypto_class || !crypto_transform) return 50;
    if (input_bytes > (1u << 20) || entropy_bytes > (1u << 20) || description_bytes > 8192u) return 87;
    *output = NULL; *output_description = NULL; *output_bytes = *output_description_bytes = 0;
    JNIEnv *env = NULL; int attached = 0, error = 13;
    jint status = (*crypto_vm)->GetEnv(crypto_vm, (void **)&env, JNI_VERSION_1_6);
    if (status == JNI_EDETACHED) {
        if ((*crypto_vm)->AttachCurrentThread(crypto_vm, &env, NULL) != JNI_OK) return 31;
        attached = 1;
    } else if (status != JNI_OK) return 31;
    jbyteArray arrays[3] = {NULL, NULL, NULL}, result_data = NULL, result_description = NULL;
    jobjectArray result = NULL;
    uint8_t *data = NULL, *descr = NULL;
    uint32_t sizes[3] = {input_bytes, entropy_bytes, description_bytes};
    const uint8_t *sources[3] = {input, entropy, description};
    for (int i = 0; i < 3; ++i) {
        arrays[i] = (*env)->NewByteArray(env, (jsize)sizes[i]);
        if (!arrays[i] || crypto_exception(env)) { error = 8; goto cleanup; }
        if (sizes[i]) (*env)->SetByteArrayRegion(env, arrays[i], 0, (jsize)sizes[i], (const jbyte *)sources[i]);
        if (crypto_exception(env)) goto cleanup;
    }
    result = (jobjectArray)(*env)->CallStaticObjectMethod(env, crypto_class, crypto_transform,
        protect ? JNI_TRUE : JNI_FALSE, arrays[0], arrays[1], arrays[2]);
    if (crypto_exception(env) || !result || (*env)->GetArrayLength(env, result) != 2) goto cleanup;
    result_data = (jbyteArray)(*env)->GetObjectArrayElement(env, result, 0);
    if (crypto_exception(env) || !result_data) goto cleanup;
    result_description = (jbyteArray)(*env)->GetObjectArrayElement(env, result, 1);
    if (crypto_exception(env) || !result_data || !result_description) goto cleanup;
    jsize size = (*env)->GetArrayLength(env, result_data), descr_size = (*env)->GetArrayLength(env, result_description);
    if (crypto_exception(env) || size < 0 || size > (1 << 20) || descr_size < 0 || descr_size > 8192 || (descr_size & 1)) goto cleanup;
    data = malloc(size ? (size_t)size : 1); descr = malloc(descr_size ? (size_t)descr_size : 1);
    if (!data || !descr) { error = 8; goto cleanup; }
    if (size) (*env)->GetByteArrayRegion(env, result_data, 0, size, (jbyte *)data);
    if (crypto_exception(env)) goto cleanup;
    if (descr_size) (*env)->GetByteArrayRegion(env, result_description, 0, descr_size, (jbyte *)descr);
    if (crypto_exception(env)) goto cleanup;
    *output = data; *output_bytes = (uint32_t)size;
    *output_description = descr; *output_description_bytes = (uint32_t)descr_size;
    data = descr = NULL; error = 0;
cleanup:
    crypto_exception(env);
    if (data) { volatile uint8_t *p = data; for (jsize i = 0; i < size; ++i) p[i] = 0; free(data); }
    if (descr) { volatile uint8_t *p = descr; for (jsize i = 0; i < descr_size; ++i) p[i] = 0; free(descr); }
    crypto_wipe_array(env, result_data); crypto_wipe_array(env, result_description);
    if (result_data) (*env)->DeleteLocalRef(env, result_data);
    if (result_description) (*env)->DeleteLocalRef(env, result_description);
    if (result) (*env)->DeleteLocalRef(env, result);
    for (int i = 0; i < 3; ++i) if (arrays[i]) { crypto_wipe_array(env, arrays[i]); (*env)->DeleteLocalRef(env, arrays[i]); }
    crypto_exception(env);
    if (attached) (*crypto_vm)->DetachCurrentThread(crypto_vm);
    return error;
}

static int crypto_set_adapter_identity(JNIEnv *env)
{
    if (!crypto_class || !crypto_adapter_mac) return EINVAL;
    jstring identity = (jstring)(*env)->CallStaticObjectMethod(env, crypto_class, crypto_adapter_mac);
    if (crypto_exception(env) || !identity) return EINVAL;
    const char *value = (*env)->GetStringUTFChars(env, identity, NULL);
    int error = 0;
    int failed = crypto_exception(env);
    if (!value || failed) error = ENOMEM;
    else {
        if (strlen(value) != 12 || strspn(value, "0123456789abcdefABCDEF") != 12) error = EINVAL;
        else if (setenv("UT3_ADAPTER_MAC", value, 1)) error = errno;
        (*env)->ReleaseStringUTFChars(env, identity, value);
    }
    (*env)->DeleteLocalRef(env, identity);
    return error;
}

uint64_t void_audio_pause_state(void) { return __atomic_load_n(&pause_state, __ATOMIC_ACQUIRE); }
/* One successful present writes to the FPS history; the UI thread copies it. */
#define FRAME_TIMES 304u
static uint32_t frame_times_us[FRAME_TIMES];
static uint64_t frame_intervals;
static int collect_frame_times;

void void_android_frame_presented(void)
{
    if (!__atomic_load_n(&frame_ready, __ATOMIC_RELAXED)) {
        void_note_first_present();
        __atomic_store_n(&frame_ready, 1, __ATOMIC_RELEASE);
    }
    if (!collect_frame_times) return;
    static uint64_t last_present;
    uint64_t now = void_monotonic_ns();
    if (last_present) {
        uint64_t us = (now - last_present) / 1000u, n = __atomic_load_n(&frame_intervals, __ATOMIC_RELAXED);
        __atomic_store_n(&frame_times_us[n % FRAME_TIMES], us > UINT32_MAX ? UINT32_MAX : (uint32_t)us, __ATOMIC_RELAXED);
        __atomic_store_n(&frame_intervals, n + 1, __ATOMIC_RELEASE);
    }
    last_present = now;
}

/* GL_RENDERER and GL_VERSION of the OpenGL ES renderer's context (d3d9_gl.c), for the FPS counter. */
static char gpu_renderer[128], gpu_version[160];
static pthread_mutex_t gpu_strings_lock = PTHREAD_MUTEX_INITIALIZER;

void void_android_gpu_strings(const char *renderer, const char *version)
{
    pthread_mutex_lock(&gpu_strings_lock);
    snprintf(gpu_renderer, sizeof gpu_renderer, "%s", renderer ? renderer : "");
    snprintf(gpu_version, sizeof gpu_version, "%s", version ? version : "");
    pthread_mutex_unlock(&gpu_strings_lock);
}

JNIEXPORT jstring JNICALL Java_org_bioshock_port_GameActivity_nativeGpuStrings(JNIEnv *env, jclass type)
{
    (void)type;
    char text[sizeof gpu_renderer + sizeof gpu_version + 2];
    pthread_mutex_lock(&gpu_strings_lock);
    snprintf(text, sizeof text, "%s\n%s", gpu_renderer, gpu_version);
    pthread_mutex_unlock(&gpu_strings_lock);
    /* GL strings are ASCII; anything else would not be modified UTF-8 for NewStringUTF. */
    for (char *c = text; *c; ++c) if ((unsigned char)*c >= 0x80) *c = '?';
    return (*env)->NewStringUTF(env, text);
}

JNIEXPORT jlong JNICALL Java_org_bioshock_port_GameActivity_nativeFrameTimes(JNIEnv *env, jclass type, jintArray out)
{
    (void)type;
    uint64_t n = __atomic_load_n(&frame_intervals, __ATOMIC_ACQUIRE);
    jsize length = out ? (*env)->GetArrayLength(env, out) : 0;
    if (length > (jsize)FRAME_TIMES) length = (jsize)FRAME_TIMES;
    jint values[FRAME_TIMES];
    for (jsize i = 0; i < length; ++i) {
        /* Oldest first; the slots before the first frames stay 0, as DXVK's graph starts. */
        uint64_t back = (uint64_t)(length - i);
        uint32_t us = back <= n ? __atomic_load_n(&frame_times_us[(n - back) % FRAME_TIMES], __ATOMIC_RELAXED) : 0;
        values[i] = us > INT32_MAX ? INT32_MAX : (jint)us;
    }
    if (length) (*env)->SetIntArrayRegion(env, out, 0, length, values);
    return (jlong)n;
}
void void_android_load_ready(void) {}

int void_android_display_size(int *w, int *h)
{
    pthread_mutex_lock(&window_lock);
    *w = display_width;
    *h = display_height;
    pthread_mutex_unlock(&window_lock);
    return *w > 0 && *h > 0;
}

int void_android_render_ready(void)
{
    pthread_mutex_lock(&window_lock);
    int ready = window != NULL && !(void_audio_pause_state() & 1u);
    pthread_mutex_unlock(&window_lock);
    return ready;
}

void *void_android_window_current(uint32_t *serial, int wait)
{
    pthread_mutex_lock(&window_lock);
    while (wait && !window) pthread_cond_wait(&window_cond, &window_lock);
    *serial = window_serial;
    ANativeWindow *result = window;
    pthread_mutex_unlock(&window_lock);
    return result;
}

void *void_android_window(void)
{
    uint32_t serial;
    return void_android_window_current(&serial, 1);
}

void *ut3_dxvk_native_window(void)
{
    pthread_mutex_lock(&window_lock);
    ANativeWindow *result = window;
    if (result) ANativeWindow_acquire(result);
    pthread_mutex_unlock(&window_lock);
    return result;
}

void void_android_window_ack(uint32_t serial)
{
    pthread_mutex_lock(&window_lock);
    window_acked = serial;
    pthread_cond_broadcast(&window_cond);
    pthread_mutex_unlock(&window_lock);
}

JNIEXPORT void JNICALL Java_org_bioshock_port_GameActivity_nativeSurface(JNIEnv *env, jclass type, jobject surface)
{
    (void)type;
    /* GameActivity alone votes the surface's panel rate (its top mode): a native
     * vote for the frame cap held a 120 Hz panel at 60 Hz. */
    ANativeWindow *next = surface ? ANativeWindow_fromSurface(env, surface) : NULL;
    pthread_mutex_lock(&window_lock);
    ANativeWindow *old = window;
    window = next;
    if (window) { display_width = ANativeWindow_getWidth(window); display_height = ANativeWindow_getHeight(window); }
    uint32_t serial = ++window_serial;
    /* Atomic epoch invalidation only: no API/TLS/session work under this lock. */
    ut3_adpf_cancel(!window || (void_audio_pause_state() & 1u));
    pthread_cond_broadcast(&window_cond);
    if (!surface && old) {
        struct timespec until;
        clock_gettime(CLOCK_REALTIME, &until);
        until.tv_sec += 1;
        while (window_acked != serial && pthread_cond_timedwait(&window_cond, &window_lock, &until) == 0) {}
    }
    pthread_mutex_unlock(&window_lock);
    if (old) ANativeWindow_release(old);
    void_guest_affinity_refresh();
    update_guest_clock();
}

/* The game's clocks stop while the app is paused and has no window (the game thread then waits in its
 * present); a port dialog over a live window keeps them running (kernel32.c). */
static void update_guest_clock(void)
{
    pthread_mutex_lock(&window_lock);
    int stop = !window && (void_audio_pause_state() & 1u);
    pthread_mutex_unlock(&window_lock);
    void_guest_clock_freeze(stop);
}

static void release_input(void)
{
    pointer_remainder_x = pointer_remainder_y = 0;
    for (unsigned vk = 1; vk < 256; ++vk) if (void_input_key_state(vk) < 0) void_input_key(vk, 0);
    for (int i = 0; i < 8; ++i) void_input_mouse_button(i, 0);
    int32_t dx, dy, dz;
    uint8_t buttons[8], keys[256];
    void_input_keyboard_snapshot(keys);
    void_input_mouse_snapshot(&dx, &dy, &dz, buttons);
}

JNIEXPORT void JNICALL Java_org_bioshock_port_GameActivity_nativePause(JNIEnv *env, jclass type, jboolean pause)
{
    (void)env; (void)type;
    uint64_t state = void_audio_pause_state(), next;
    do {
        if ((state & 1u) == (pause ? 1u : 0u)) break;
        next = ((state & ~UINT64_C(1)) + 2u) | (pause ? 1u : 0u);
    } while (!__atomic_compare_exchange_n(&pause_state, &state, next, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE));
    ut3_adpf_cancel(!void_android_render_ready());
    /* Android moves the process between cpusets here, which resets thread masks. */
    void_guest_affinity_refresh();
    if (pause) release_input();
    if (ut3_openal_pause) ut3_openal_pause(pause != JNI_FALSE);
    update_guest_clock();
}

static void *run_game(void *unused)
{
    (void)unused;
    int result = ut3_game_main(game_dir, data_dir, image_dir, 0);
    ut3_adpf_thread_close();
    __atomic_store_n(&game_result, result, __ATOMIC_RELEASE);
    void_log("[ut3] Android game thread returned %d", result);
    return NULL;
}

static int select_frame_rate(float fps)
{
    if (!(fps >= 1.0f && fps <= 1000.0f)) return EINVAL;
    char value[32];
    snprintf(value, sizeof value, "%.9g", (double)fps);
    selected_fps = fps;
    ut3_adpf_set_target(fps);
    if (d3d_gl_set_frame_limit) d3d_gl_set_frame_limit(fps);
    if (setenv("UT3_FPS_LIMIT", value, 1) || setenv("DXVK_FRAME_RATE", value, 1)) return errno;
    void_log("[fps] target %.9g Hz", (double)fps);
    return 0;
}

JNIEXPORT jint JNICALL Java_org_bioshock_port_GameActivity_nativeFrameRate(JNIEnv *env, jclass type, jfloat fps)
{
    (void)env; (void)type;
    return select_frame_rate(fps);
}

JNIEXPORT jint JNICALL Java_org_bioshock_port_GameActivity_nativeConfigure(JNIEnv *env, jclass type, jfloat fps, jint query_wait_us, jint precache_wait_us, jboolean benchmark)
{
    (void)env; (void)type;
    if (game_started) return EBUSY;
    if (query_wait_us < 0 || query_wait_us > 1000 || precache_wait_us < 0 || precache_wait_us > 1000) return EINVAL;
    char wait_value[16];
    snprintf(wait_value, sizeof wait_value, "%d", query_wait_us);
    if (setenv("BIOSHOCK_QUERY_WAIT_US", wait_value, 1)) return errno;
    snprintf(wait_value, sizeof wait_value, "%d", precache_wait_us);
    if (setenv("UT3_PRECACHE_WAIT_US", wait_value, 1)) return errno;
    int error = select_frame_rate(fps);
    if (error) return error;
    benchmark_start = benchmark != JNI_FALSE;
    return 0;
}

JNIEXPORT jint JNICALL Java_org_bioshock_port_GameActivity_nativePortOverlay(JNIEnv *env, jclass type, jstring path, jboolean mobile)
{
    (void)type;
    if (!path) return EINVAL;
    if (game_started) return 0;
    const char *directory = (*env)->GetStringUTFChars(env, path, NULL);
    if (!directory) return ENOMEM;
    int error = 0;
    if (!*directory || strlen(directory) >= 1024 || *directory != '/') error = EINVAL;
    else if (setenv("BIOSHOCK_MOBILE_SHADERS", mobile ? "1" : "0", 1)) error = errno;
    else void_files_set_overlay(directory);
    (*env)->ReleaseStringUTFChars(env, path, directory);
    return error;
}

JNIEXPORT jint JNICALL Java_org_bioshock_port_GameActivity_nativeStart(JNIEnv *env, jclass type, jobject assets, jstring data, jstring obb, jstring renderer, jboolean fixed_seed, jint render_width, jint render_height)
{
    if (!assets || !renderer || !data || !obb || render_width < 8 || render_width > 8192 || render_height < 8 || render_height > 8192 || render_width < render_height) return EINVAL;
    if (!settings_class) {
        settings_class = (jclass)(*env)->NewGlobalRef(env, type);
        int failed = crypto_exception(env);
        if (!settings_class || failed) {
            if (settings_class) (*env)->DeleteGlobalRef(env, settings_class);
            settings_class = NULL;
            return ENOMEM;
        }
        settings_callback = (*env)->GetStaticMethodID(env, settings_class, "portSetting", "(Ljava/lang/String;Ljava/lang/String;)Ljava/lang/String;");
        failed = crypto_exception(env);
        if (!settings_callback || failed) {
            (*env)->DeleteGlobalRef(env, settings_class);
            settings_class = NULL; settings_callback = NULL;
            return EINVAL;
        }
    }
    if (!game_started) { int identity_error = crypto_set_adapter_identity(env); if (identity_error) return identity_error; }
    if (!game_started) {
        int obb_error = void_obb_init_assets(env, assets);
        if (!obb_error && !void_obb_ready()) {
            const char *directory = (*env)->GetStringUTFChars(env, obb, NULL);
            if (!directory) return ENOMEM;
            obb_error = void_obb_init_external(directory);
            (*env)->ReleaseStringUTFChars(env, obb, directory);
        }
        if (obb_error) return obb_error;
        if (!void_obb_ready()) void_log("[void] no OBB: using the selected game folder and any stored ZIP");
    }
    const char *mode = (*env)->GetStringUTFChars(env, renderer, NULL);
    if (!mode) return ENOMEM;
    /* The native Direct3D 10 renderer (android/render10): OpenGL ES 3.2, or Vulkan 1.1 on the system
     * driver, Mesa Turnip (Adreno) or Mesa PanVK (Mali). */
    int turnip = !strcmp(mode, "vulkan-turnip"), panvk = !strcmp(mode, "vulkan-panvk");
    int vulkan = !strcmp(mode, "vulkan") || turnip || panvk;
    int valid = !strcmp(mode, "gles") || vulkan;
    int mode_error = 0;
    if (valid && !game_started &&
        (setenv("BIOSHOCK_RENDERER", vulkan ? "vulkan" : "gles", 1) ||
         setenv("BIOSHOCK_VULKAN_DRIVER", turnip ? "turnip" : panvk ? "panvk" : "system", 1))) mode_error = errno;
    (*env)->ReleaseStringUTFChars(env, renderer, mode);
    if (!valid) return EINVAL;
    if (mode_error) return mode_error;
    if (game_started) return 0;
    const char *hud = getenv("PORT_OPTION_FPS_COUNTER");
    collect_frame_times = hud && (!strcmp(hud, "1") || !strcmp(hud, "2"));
    {
        extern void r10_shaderlib_set_manager(void *manager);
        r10_shaderlib_set_manager(AAssetManager_fromJava(env, assets));
    }
    /* Keep the serialized guest on the strongest-core policy to avoid
     * migration/lower-capacity placement; native workers retain Android's mask. */
    if (setenv("VOID_BIG_CORES", "1", 1)) return errno;
    char render_size[16];
    snprintf(render_size, sizeof render_size, "%d", render_width);
    if (setenv("UT3_RENDER_WIDTH", render_size, 1)) return errno;
    snprintf(render_size, sizeof render_size, "%d", render_height);
    if (setenv("UT3_RENDER_HEIGHT", render_size, 1)) return errno;
    snprintf(render_size, sizeof render_size, "%.9g", (double)selected_fps);
    if (setenv("UT3_FPS_LIMIT", render_size, 1)) return errno;
    if (setenv("VOID_TRACE_CALLS", "10CDFA40,10CE7D30,10CE2670,10E4AC2B,10E58014,10CE87F0,10E4A8D1,10CE9C00,10CE70C0,10E4AD77,10E4AEDF,10CE8520", 1) ||
        unsetenv("VOID_DSOUND_TRACE")) return errno;
    const char *skip = getenv("PORT_SKIP_INTRO");
    int skip_intro = !skip || !strcmp(skip, "1") || !strcasecmp(skip, "true");
    char command_line[256];
    int command_size = snprintf(command_line, sizeof command_line,
        "\"C:\\Games\\BioShock\\Builds\\Release\\Bioshock.exe\"%s", skip_intro ? " -nointro" : "");
    if (command_size < 0 || command_size >= (int)sizeof command_line) return EOVERFLOW;
    if (setenv("UT3_COMMAND_LINE", command_line, 1)) return errno;
    const char *user = (*env)->GetStringUTFChars(env, data, NULL);
    if (!user) return -1;
    int size = snprintf(data_dir, sizeof data_dir, "%s", user);
    (*env)->ReleaseStringUTFChars(env, data, user);
    if (size < 0 || size >= (int)sizeof data_dir) return -1;
    {
        /* QA-only A/B switches: a private profile may carry perf-flags.env with
         * whitelisted KEY=VALUE lines. Ordinary profiles have no such file and
         * nothing here is a player setting. */
        static const char *const allowed[] = { "BIOSHOCK_LEGACY_WAKE", "VOID_GUEST_CORES", "VOID_GUEST_PIN_EVERY",
                                               "BIOSHOCK_THREAD_SNAPSHOT", "BIOSHOCK_SPIN_HINT",
                                               "BIOSHOCK_MACHINE_STATS", "BIOSHOCK_WAIT_RELEASE_FIRST",
                                               "BIOSHOCK_ENGINE_SINGLE_CORE",
                                               "BIOSHOCK_API_STATS", "BIOSHOCK_SHADERLIB10", "BIOSHOCK_R10_SHADERS", "BIOSHOCK_R10_SHADER_TRACE", "BIOSHOCK_R10_TRACE", "BIOSHOCK_R10_DEBUG", "BIOSHOCK_R10_OCCLUSION_WAIT",
                                               "BIOSHOCK_DUMP_PROPERTIES", "BIOSHOCK_DSOUND_STALL", "BIOSHOCK_HOLD_SAMPLER",
                                               "BIOSHOCK_HUD_ASPECT", "BIOSHOCK_HUD_LAYOUT", "BIOSHOCK_HUD_TRACE",
                                               "BIOSHOCK_HUD_SCALE", "BIOSHOCK_HUD_CLEAR", "BIOSHOCK_HUD_SHIFTMODE", "BIOSHOCK_SLOW_CALLS", "BIOSHOCK_PRESENT_RELEASE",
                                               "BIOSHOCK_QA_CONSOLE", "BIOSHOCK_LOADING_BACKOFF", "BIOSHOCK_NATIVE_ZLIB", "BIOSHOCK_NATIVE_CRT",
                                               "BIOSHOCK_FMOD_OUTPUT", "BIOSHOCK_FMOD_RATE", "BIOSHOCK_BINK_OUTPUT",
                                               "BIOSHOCK_AUDIO_URGENT", "BIOSHOCK_TEXTURE_PACK", "BIOSHOCK_ADPF", "BIOSHOCK_ADPF_TARGET",
                                               "BIOSHOCK_SCRIPT_CYCLES", "BIOSHOCK_FREEVERB_MEMORY", "BIOSHOCK_VERBOSE" };
        char path[1100], line[1024];   /* a QA console list of every level is ~300 characters */
        snprintf(path, sizeof path, "%s/perf-flags.env", data_dir);
        FILE *flags = fopen(path, "r");
        while (flags && fgets(line, sizeof line, flags)) {
            line[strcspn(line, "\r\n")] = 0;
            char *equals = strchr(line, '=');
            if (!equals || line[0] == '#') continue;
            *equals = 0;
            for (size_t i = 0; i < sizeof allowed / sizeof allowed[0]; ++i) {
                if (strcmp(line, allowed[i])) continue;
                if (!setenv(line, equals + 1, 1)) void_log("[perf-flags] %s=%s", line, equals + 1);
            }
        }
        if (flags) fclose(flags);
        /* QA: the host's verbose log (file lookups, FindFirstFile results, every exception's text). */
        extern int void_verbose;
        const char *verbose = getenv("BIOSHOCK_VERBOSE");
        if (verbose && verbose[0] == '1') void_verbose = 1;
    }
    {
        /* GLES program binaries (d3d9_shader.c) need a cache folder; without one every start
         * compiled and linked each material's program again in the frame that first drew it.
         * Mobile v3 isolates per-tap bloom from old mobile PCS/program binaries without deleting them. */
        char cache[1100];
        const char *mobile = getenv("BIOSHOCK_MOBILE_SHADERS");
        extern int void_files_cache_directory(const char *, const char *, char *, size_t);
        if (void_files_cache_directory(data_dir,
                mobile && !strcmp(mobile, "1") ? "ShaderCache-mobile-v3" : "ShaderCache", cache, sizeof cache))
            void_files_set_cache(cache);
    }
    if (turnip) {
        char debug_file[1100];
        int count = snprintf(debug_file, sizeof debug_file, "%s/tu-debug.txt", data_dir);
        if (count < 0 || count >= (int)sizeof debug_file) return EOVERFLOW;
        if (setenv("TU_DEBUG_FILE", debug_file, 1)) return errno;
    } else unsetenv("TU_DEBUG_FILE");
    const char *game = getenv("PORT_PART_GAME");
    int game_size = snprintf(game_dir, sizeof game_dir, "%s", game && *game ? game : "/sdcard/BioShock/game");
    if (game_size < 0 || game_size >= (int)sizeof game_dir) return EOVERFLOW;
    snprintf(image_dir, sizeof image_dir, "%s", game_dir);
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 64u << 20);
    pthread_t thread;
    int error = pthread_create(&thread, &attr, run_game, NULL);
    pthread_attr_destroy(&attr);
    if (error) return error;
    pthread_detach(thread);
    game_started = 1;
    return 0;
}

JNIEXPORT jint JNICALL Java_org_bioshock_port_GameActivity_nativeStatus(JNIEnv *env, jclass type)
{
    (void)env; (void)type;
    int result = __atomic_load_n(&game_result, __ATOMIC_ACQUIRE);
    if (result != INT_MIN) return result;
    /* Diagnostic only: every 100 s this walked and logged every guest thread
     * from the UI thread. QA enables it with BIOSHOCK_THREAD_SNAPSHOT=1. */
    static unsigned debug_ticks;
    static int snapshots = -1;
    if (snapshots < 0) {
        const char *value = getenv("BIOSHOCK_THREAD_SNAPSHOT");
        snapshots = value && *value == '1';
    }
    if (snapshots && ++debug_ticks % 200 == 0) void_debug_thread_snapshot();
    return __atomic_load_n(&frame_ready, __ATOMIC_ACQUIRE) ? INT_MAX : INT_MIN;
}

void void_bioshock_request_navigation(void);
void void_bioshock_request_medical_navigation(void);

JNIEXPORT void JNICALL Java_org_bioshock_port_GameActivity_nativeKey(JNIEnv *env, jclass type, jint vk, jboolean down)
{
    (void)env; (void)type;
    if (vk == 13) void_log("[input] native Enter down=%d", down);
    /* QA travel/console keys exist only for private profiles whose perf-flags.env sets
     * BIOSHOCK_QA_CONSOLE; a player's keyboard F10/F11 reach the game unchanged. */
    const char *qa_console = getenv("BIOSHOCK_QA_CONSOLE");
    if (qa_console && *qa_console && down && vk == 0x79) void_bioshock_request_navigation();
    if (qa_console && *qa_console && down && vk == 0x7A) void_bioshock_request_medical_navigation();
    if (vk > 0 && vk < 256) void_input_key((uint32_t)vk, down);
}

JNIEXPORT void JNICALL Java_org_bioshock_port_GameActivity_nativeButton(JNIEnv *env, jclass type, jboolean down)
{
    (void)env; (void)type;
    void_input_mouse_button(0, down);
}

extern void void_power_android_snapshot(int plugged, int present, int level, int scale, int status);
JNIEXPORT void JNICALL Java_org_bioshock_port_GameActivity_nativePower(JNIEnv *env, jclass type,
    jint plugged, jint present, jint level, jint scale, jint status)
{
    (void)env; (void)type;
    void_power_android_snapshot(plugged, present, level, scale, status);
}

JNIEXPORT jboolean JNICALL Java_org_bioshock_port_GameActivity_nativePointer(JNIEnv *env, jclass type,
    jfloat x, jfloat y, jboolean relative)
{
    (void)env; (void)type;
    if (!isfinite(x) || !isfinite(y) || fabsf(x) > 4 || fabsf(y) > 4) return JNI_FALSE;
    int w, h, sw, sh;
    void_input_screen_size(&w, &h);
    if (w <= 0 || h <= 0) return JNI_FALSE;
    if (relative) {
        float dx = x * w + pointer_remainder_x, dy = y * h + pointer_remainder_y;
        int ix = (int)dx, iy = (int)dy;
        pointer_remainder_x = dx - ix; pointer_remainder_y = dy - iy;
        void_input_mouse_move(ix, iy);
        return JNI_TRUE;
    }
    if (!void_android_display_size(&sw, &sh)) return JNI_FALSE;
    float scale = fminf((float)sw / w, (float)sh / h);
    float px = (x * sw - (sw - w * scale) * 0.5f) / scale;
    float py = (y * sh - (sh - h * scale) * 0.5f) / scale;
    if (px < 0 || px >= w || py < 0 || py >= h) return JNI_FALSE;
    void_input_mouse_warp((int)px, (int)py);
    return JNI_TRUE;
}
/* ------------------------------------------------------------- audio */
static AAudioStream *stream;
static pthread_mutex_t audio_lock = PTHREAD_MUTEX_INITIALIZER;
static uint64_t stream_pause_state;
static uint64_t audio_short_writes, audio_dropped_frames, audio_errors;
static unsigned audio_zero_chunks;
#define AUDIO_PRIME_FRAMES 1440u       /* Three 10 ms mixer chunks at 48 kHz. */
/* shortcut: retry low latency only after process restart; revisit for route-aware retries. */
static int audio_normal_output;

static void audio_close_stream(void)
{
    if (stream) {
        AAudioStream_requestStop(stream);
        AAudioStream_close(stream);
    }
    stream = NULL;
    audio_zero_chunks = 0;
}

/* A changed output route (a Bluetooth headset arriving or leaving, a plug, the
 * system capturing the output) or a restarted audio server disconnects the
 * stream while the game runs: a write or an open fails. The next chunk reopens on
 * the current route; an open that fails, or a stream that fails within a second
 * of opening, is retried 20 ms, 40 ms, ... up to 2 s later while the DirectSound
 * mixer keeps pacing the game's buffers (no open attempt every 10 ms chunk).
 * The unwritten tail of the chunk that hit the disconnect starts the next stream.
 * Losses and returns are logged at most 8 lines per 10 s, for the whole session
 * (the old writer logged only the first 4 errors of the process). Transferred
 * from Heroes V (wiki note 13, A6). */
#define AUDIO_RETRY_FIRST_NS 20000000LL
#define AUDIO_RETRY_MAX_NS 2000000000LL
static int64_t audio_retry_at, audio_retry_delay, audio_stream_opened, audio_lost_at, audio_log_window;
static unsigned audio_lost_attempts, audio_log_count;
static uint64_t audio_lost_frames, audio_reconnects;
#define AUDIO_PENDING_FRAMES 480u
static int16_t audio_pending[2 * AUDIO_PENDING_FRAMES];
static uint32_t audio_pending_frames;
static uint64_t audio_pending_state;
/* QA: <userdata>/audio_fault, read at most once a second, holds "disconnect [n]"
 * (the next n writes fail with AAUDIO_ERROR_DISCONNECTED) or "unavailable [n]"
 * (the next n opens fail with AAUDIO_ERROR_UNAVAILABLE). The file is removed;
 * where it cannot be, the same text does not apply again. */
static unsigned audio_fault_writes, audio_fault_opens;
static int64_t audio_fault_polled;
static char audio_fault_last[64];

static int64_t audio_now(void)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (int64_t)now.tv_sec * 1000000000LL + now.tv_nsec;
}

static int audio_may_log(int64_t now)
{
    if (!audio_log_window || now - audio_log_window >= 10000000000LL) {
        if (audio_log_count > 8) void_log("[audio] %u more loss/return lines suppressed", audio_log_count - 8);
        audio_log_window = now;
        audio_log_count = 0;
    }
    return ++audio_log_count <= 8;
}

static void audio_fault_poll(int64_t now)
{
    if (audio_fault_polled && now - audio_fault_polled < 1000000000LL) return;
    audio_fault_polled = now;
    char path[1200], line[64] = "", kind[16] = "";
    snprintf(path, sizeof path, "%s/audio_fault", data_dir);
    FILE *file = fopen(path, "r");
    if (!file) return;
    if (!fgets(line, sizeof line, file)) line[0] = 0;
    fclose(file);
    if (unlink(path)) {
        if (!strcmp(line, audio_fault_last)) return;
        snprintf(audio_fault_last, sizeof audio_fault_last, "%s", line);
    }
    unsigned count = 1;
    if (sscanf(line, "%15s %u", kind, &count) < 1) return;
    if (count > 1000) count = 1000;
    if (!strcmp(kind, "disconnect")) audio_fault_writes = count;
    else if (!strcmp(kind, "unavailable")) audio_fault_opens = count;
    else { void_log("[audio] fault injection: unknown request '%s'", kind); return; }
    void_log("[audio] fault injection: %s x%u", kind, count);
}

static void audio_mark_lost(int64_t now)
{
    if (audio_lost_at) return;
    audio_lost_at = now;
    audio_lost_attempts = 0;
    audio_lost_frames = 0;
}

/* An open, prime or start failed: the output is unavailable for now. */
static void audio_open_failed(aaudio_result_t error)
{
    int64_t now = audio_now();
    ++audio_errors;
    audio_mark_lost(now);
    ++audio_lost_attempts;
    audio_retry_delay = !audio_retry_delay ? AUDIO_RETRY_FIRST_NS :
        audio_retry_delay < AUDIO_RETRY_MAX_NS / 2 ? audio_retry_delay * 2 : AUDIO_RETRY_MAX_NS;
    audio_retry_at = now + audio_retry_delay;
    if (audio_may_log(now))
        void_log("[audio] output unavailable: error %d (%s); next attempt in %lld ms", (int)error,
                 AAudio_convertResultToText(error), (long long)(audio_retry_delay / 1000000));
    audio_close_stream();
}

/* A running stream failed (a write: the route changed): reopen at the next chunk. */
static void audio_close_failed(aaudio_result_t error)
{
    int64_t now = audio_now();
    if (stream && audio_stream_opened && now - audio_stream_opened < 1000000000LL) {
        audio_open_failed(error);
        return;
    }
    ++audio_errors;
    audio_mark_lost(now);
    audio_retry_at = 0;
    audio_retry_delay = 0;
    if (audio_may_log(now))
        void_log("[audio] stream error %d (%s); reopening on next chunk", (int)error, AAudio_convertResultToText(error));
    audio_close_stream();
}

/* A timed-out write can accept a prefix. Never resend it or silently discard
 * the suffix; retry within a bounded budget, with no guest VM lock held. After
 * a failed write the suffix of game PCM (keep_tail) waits for the next stream. */
static uint32_t audio_write_frames(const int16_t *frames, uint32_t count, int keep_tail)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    int64_t deadline = (int64_t)now.tv_sec * 1000000000LL + now.tv_nsec + 100000000LL;
    uint32_t written = 0;
    uint64_t pause = stream_pause_state;
    int failed = 0;
    while (stream && written < count && void_audio_pause_state() == pause) {
        clock_gettime(CLOCK_MONOTONIC, &now);
        int64_t remaining = deadline - ((int64_t)now.tv_sec * 1000000000LL + now.tv_nsec);
        if (remaining <= 0) break;
        aaudio_result_t result;
        if (audio_fault_writes) {
            --audio_fault_writes;
            result = AAUDIO_ERROR_DISCONNECTED;
            void_log("[audio] fault injection: write fails with %d", (int)result);
        } else {
            result = AAudioStream_write(stream, frames + written * 2,
                (int32_t)(count - written), remaining < 20000000LL ? remaining : 20000000LL);
        }
        if (result < 0) { audio_close_failed(result); failed = 1; break; }
        if ((uint32_t)result < count - written) ++audio_short_writes;
        written += (uint32_t)result;
        if (!result) usleep(1000);
    }
    if (written < count && void_audio_pause_state() == pause) {
        uint32_t left = count - written;
        if (failed && keep_tail && !audio_retry_at) {   /* kept only for an immediate reopen */
            uint32_t keep = left < AUDIO_PENDING_FRAMES ? left : AUDIO_PENDING_FRAMES;
            memmove(audio_pending, frames + (size_t)written * 2, (size_t)keep * 2 * sizeof *frames);
            audio_pending_frames = keep;
            audio_pending_state = pause;
            left -= keep;
        }
        if (left) {
            if (audio_dropped_frames < 1920)
                void_log("[audio] discarded %u unwritten frames after timeout/error", left);
            audio_dropped_frames += left;
            if (failed) audio_lost_frames += left;
        }
    }
    return written;
}

static aaudio_result_t audio_start_stream(uint64_t pause)
{
    int32_t capacity = AAudioStream_getBufferCapacityInFrames(stream);
    int32_t burst = AAudioStream_getFramesPerBurst(stream);
    int32_t requested = AUDIO_PRIME_FRAMES;
    if (burst > requested / 2 && burst <= INT32_MAX / 2) requested = burst * 2;
    if (capacity > 0 && requested > capacity) requested = capacity;
    aaudio_result_t configured = AAudioStream_setBufferSizeInFrames(stream, requested);
    int32_t size = AAudioStream_getBufferSizeInFrames(stream);
    uint32_t prime = size > 0 && size < (int32_t)AUDIO_PRIME_FRAMES ? (uint32_t)size : AUDIO_PRIME_FRAMES;
    void_log("[audio] output capacity=%d buffer=%d burst=%d requested=%d configured=%d prime=%u rate=%d mode=%d",
             capacity, size, burst, requested, configured, prime,
             AAudioStream_getSampleRate(stream), AAudioStream_getPerformanceMode(stream));
    if (void_audio_pause_state() != pause) { audio_close_stream(); return AAUDIO_OK; }
    aaudio_result_t result = AAudioStream_requestStart(stream);
    if (result < 0) return result;
    /* Start can discard pre-start PCM. Fill the live output, so the 10 ms
     * producer uses the buffer's jitter margin instead of leaving it empty. */
    static const int16_t silence[2 * AUDIO_PRIME_FRAMES] = {0};
    (void)audio_write_frames(silence, prime, 0);
    /* A prime write that failed already closed the stream and scheduled the retry. */
    return AAUDIO_OK;
}

void void_audio_output(const int16_t *frames, uint32_t count)
{
    pthread_mutex_lock(&audio_lock);
    uint64_t pause = void_audio_pause_state();
    /* Pause retains queued PCM; a fresh foreground epoch needs a fresh stream. */
    if (stream && pause != stream_pause_state) audio_close_stream();
    if (audio_pending_frames && pause != audio_pending_state) audio_pending_frames = 0;
    if (pause & 1u) {
        /* mixer_main already paces every chunk, including in the background;
           the next foreground epoch opens on whatever route is current. */
        audio_retry_at = 0;
        audio_retry_delay = 0;
        audio_lost_at = 0;
        pthread_mutex_unlock(&audio_lock);
        return;
    }
    int64_t now = audio_now();
    audio_fault_poll(now);
    if (!stream && audio_retry_at && now < audio_retry_at) {
        /* The output is gone: the mixer's clock keeps the game's buffers moving. */
        audio_dropped_frames += count + audio_pending_frames;
        audio_lost_frames += count + audio_pending_frames;
        audio_pending_frames = 0;
        pthread_mutex_unlock(&audio_lock);
        return;
    }
    if (!stream) {
        AAudioStreamBuilder *b = NULL;
        aaudio_result_t created = AAUDIO_ERROR_UNAVAILABLE;
        if (audio_fault_opens) {
            --audio_fault_opens;
            void_log("[audio] fault injection: open fails with %d", (int)created);
        } else created = AAudio_createStreamBuilder(&b);
        if (created == AAUDIO_OK) {
            AAudioStreamBuilder_setFormat(b, AAUDIO_FORMAT_PCM_I16);
            AAudioStreamBuilder_setChannelCount(b, 2);
            AAudioStreamBuilder_setSampleRate(b, 48000);
            AAudioStreamBuilder_setBufferCapacityInFrames(b, AUDIO_PRIME_FRAMES);
            AAudioStreamBuilder_setPerformanceMode(b, audio_normal_output ?
                AAUDIO_PERFORMANCE_MODE_NONE : AAUDIO_PERFORMANCE_MODE_LOW_LATENCY);
            aaudio_result_t result = AAudioStreamBuilder_openStream(b, &stream);
            if (result == AAUDIO_OK) {
                stream_pause_state = pause;
                audio_stream_opened = audio_now();
                result = audio_start_stream(pause);
                if (result < 0) audio_open_failed(result);
            } else { stream = NULL; audio_open_failed(result); }
            AAudioStreamBuilder_delete(b);
        } else audio_open_failed(created);
        if (!stream) {
            if (void_audio_pause_state() == pause) {
                audio_dropped_frames += count + audio_pending_frames;
                if (audio_lost_at) audio_lost_frames += count + audio_pending_frames;
            }
            audio_pending_frames = 0;
            pthread_mutex_unlock(&audio_lock);
            return;
        }
        /* The delay is kept: a stream that fails again within a second backs off further. */
        audio_retry_at = 0;
        if (audio_lost_at) {
            int64_t back = audio_now();
            ++audio_reconnects;
            /* The return is always logged, with the number of loss lines the limit hid before it: one return
               follows each loss, and an output that fails again within a second backs off. */
            unsigned hidden = audio_may_log(back) ? 0 : audio_log_count - 9;
            void_log("[audio] output back after %lld ms: %u failed opens, %llu frames not played, reconnect %llu, %u loss lines not shown",
                     (long long)((back - audio_lost_at) / 1000000), audio_lost_attempts,
                     (unsigned long long)audio_lost_frames, (unsigned long long)audio_reconnects, hidden);
            audio_lost_at = 0;
        }
    }
    if (audio_pending_frames) {
        /* The tail of the chunk that hit the disconnect plays first. */
        uint32_t pending = audio_pending_frames;
        audio_pending_frames = 0;
        (void)audio_write_frames(audio_pending, pending, 0);
    }
    uint32_t written = audio_write_frames(frames, count, 1);
    static FILE *pcm_capture;
    static uint64_t pcm_capture_bytes;
    static unsigned pcm_capture_segment;
    static int pcm_capture_unavailable;
    if (!pcm_capture && !pcm_capture_unavailable && pcm_capture_segment < 4) {
        char path[1100];
        snprintf(path, sizeof path, "%s/capture-pcm-enabled", data_dir);
        FILE *enabled = fopen(path, "rb");
        if (enabled) {
            fclose(enabled);
            snprintf(path, sizeof path, "%s/bioshock-output-s16le-48000-stereo-%02u.pcm", data_dir, pcm_capture_segment);
            pcm_capture = fopen(path, "wb");
            if (pcm_capture) void_log("[audio-capture] output PCM path=%s", path);
            else void_log("[audio-capture] cannot open %s", path);
        } else pcm_capture_unavailable = 1;
    }
    if (pcm_capture && written) {
        pcm_capture_bytes += fwrite(frames, sizeof(int16_t), (size_t)written * 2, pcm_capture) * sizeof(int16_t);
        fflush(pcm_capture);
        if (pcm_capture_bytes >= 64000000u) {
            fclose(pcm_capture);
            pcm_capture = NULL;
            pcm_capture_bytes = 0;
            ++pcm_capture_segment;
            void_log("[audio-capture] completed segment %u of 4", pcm_capture_segment);
        }
    }
    if (written || void_audio_pause_state() != pause) audio_zero_chunks = 0;
    else if (stream && count && !audio_normal_output && ++audio_zero_chunks == 3) {
        audio_normal_output = 1;
        ++audio_errors;
        void_log("[audio] low latency stream stalled for 3 chunks; using normal output until process exit");
        audio_close_stream();
    }
    /* Level of what reached the device, every 10 s of audio: silence or
     * underruns show up in the log rather than only by ear. */
    static uint32_t level_frames;
    static double level_sum;
    static int level_peak;
    for (uint32_t i = 0; i < written * 2; ++i) {
        int v = frames[i] < 0 ? -frames[i] : frames[i];
        if (v > level_peak) level_peak = v;
        level_sum += (double)frames[i] * frames[i];
    }
    level_frames += written;
    if (level_frames >= 48000u * 10u) {
        void_log("[audio] last 10 s: peak %d, rms %.0f, underruns %d, short writes %llu, dropped frames %llu, errors %llu", level_peak,
                 sqrt(level_sum / (2.0 * level_frames)), stream ? (int)AAudioStream_getXRunCount(stream) : -1,
                 (unsigned long long)audio_short_writes, (unsigned long long)audio_dropped_frames,
                 (unsigned long long)audio_errors);
        level_frames = 0;
        level_sum = 0;
        level_peak = 0;
    }
    pthread_mutex_unlock(&audio_lock);
}
#endif

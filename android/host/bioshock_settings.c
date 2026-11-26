/* Original Flash Graphics widgets send port-only commands through the stock FS callback. */
#include "host.h"
#include <errno.h>
#ifdef UT3_ANDROID_APP
#include <jni.h>
#endif

extern int void_machine_is_owned(void);
extern uint32_t void_bioshock_touch_movie(void);
extern int void_android_port_setting(const char *key, const char *value, char *out, size_t cap) __attribute__((weak));
extern void sub_10A65370(void);
extern void void_bioshock_flash_relayout(void);
extern void void_bioshock_render_hdr(int wanted) __attribute__((weak));
extern void void_bioshock_render_poll(void) __attribute__((weak));
extern void void_bioshock_render_scale(int percent) __attribute__((weak));
extern void void_bioshock_upscaler(int mode) __attribute__((weak));
extern void void_bioshock_touch_context(uint32_t source, uint32_t flags) __attribute__((weak));
extern void void_bioshock_tweak(const char *key, const char *value) __attribute__((weak));
extern void void_bioshock_tweaks_poll(void) __attribute__((weak));
static int settings_requested;
static uint64_t settings_deadline;

static const char *setting_keys[] = {
    "fpsChoices", "renderer", "fpsLimit", "mobileShaders", "hdr", "touchSensitivity", "touchScale", "touchOpacity",
    "touchLeftHanded", "touchInvertY", "touchGyro", "touchGyroSensitivity", "touchMore", "touchVitals", "uiScale",
    "touchAltSensitivity", "touchAcceleration", "touchEnabled", "touchUse", "touchPrompts", "renderScale", "upscaler",
    "anisotropy", "playerFov", "weaponFov", "physicsRate", "fixScrounger", "cutsceneFps", "mouseRaw",
    "touchScheme", "iosUiScale", "haptic", "hapticStrength", "touchAimAssist", "touchWeaponBar", "renderResolutions",
    "cheats", "touchStickDeadZone", "touchStickVisibility", "touchStaticStick",
    "resetLayout", "editLayout"
};
/* The movement page fetches its options together when opened. */
enum { MOVEMENT_SETTINGS = 3, SETTINGS_COMMANDS = 2 };

static int setting_key(const char *key)
{
    for (unsigned i = 0; i < sizeof setting_keys / sizeof *setting_keys; ++i)
        if (!strcmp(key, setting_keys[i])) return (int)i;
    return -1;
}

static int guest_string(uint32_t address, char *out, size_t capacity)
{
    if (!address || !capacity) return 0;
    for (size_t i = 0; i < capacity; ++i) {
        uint64_t at = (uint64_t)address + i;
        if (at >= UINT64_C(0x100000000) || !void_vm_readable((uint32_t)at, 1)) return 0;
        out[i] = (char)MEM8((uint32_t)at);
        if (!out[i]) return 1;
    }
    return 0;
}

static uint32_t pause_movie(void)
{
    uint32_t movie = void_bioshock_touch_movie();
    if (movie < 0x10000u || movie > UINT32_MAX - 272 || !void_vm_readable(movie, 272)) return 0;
    uint32_t table = MEM32(0x11297070u), count = MEM32(0x11297074u), name = MEM32(movie + 36);
    if (!table || name >= count || name > (UINT32_MAX - table) / 4 || !void_vm_readable(table + 4 * name, 4)) return 0;
    uint32_t entry = MEM32(table + 4 * name);
    static const uint16_t pause[] = {'P','a','u','s','e',0};
    return entry >= 0x10000u && entry <= UINT32_MAX - 28 && void_vm_readable(entry + 16, sizeof pause) &&
        !memcmp(GPTR(entry + 16), pause, sizeof pause) && MEM32(movie + 268) ? movie : 0;
}

void void_bioshock_movie_call(uint32_t movie, const char *method, const char *key, const char *value)
{
    void_regs saved;
    void_regs_save(&saved);
#ifdef RECOMP_X87_WINDOW
    double window[RECOMP_X87_SLOTS];
    int64_t integers[RECOMP_X87_SLOTS];
    int sp = g_fp_sp;
    memcpy(window, g_st_win, sizeof window); memcpy(integers, g_st_i64_win, sizeof integers);
#endif
    uint32_t name = void_gstrdup(method), format = void_gstrdup(key ? "%s,%s" : "");
    uint32_t guest_key = key ? void_gstrdup(key) : 0, guest_value = key ? void_gstrdup(value) : 0;
    if (name && format && (!key || (guest_key && guest_value))) {
        uint32_t args[] = {movie, name, format, guest_key, guest_value};
        (void)void_call_guest(0x10A66CD0u, key ? 5 : 3, args, 0);
    }
    if (guest_value) void_gfree(guest_value);
    if (guest_key) void_gfree(guest_key);
    if (format) void_gfree(format);
    if (name) void_gfree(name);
#ifdef RECOMP_X87_WINDOW
    g_fp_sp = sp;
    memcpy(g_st_win, window, sizeof window); memcpy(g_st_i64_win, integers, sizeof integers);
#endif
    void_regs_load(&saved);
}

static int graphics_setting(const char *key)
{
    return !strcmp(key, "uiScale") || !strcmp(key, "renderScale") || !strcmp(key, "renderResolutions");
}

static void publish_setting(uint32_t movie, const char *key, const char *value)
{
    char result[4096];
    int error = void_android_port_setting ? void_android_port_setting(key, value, result, sizeof result) : ENOSYS;
    if (error) {
        void_log("[port-settings] %s failed: %d", key, error);
        return;
    }
    if (graphics_setting(key)) void_bioshock_movie_call(movie, "PortGraphicsSet", key, result);
    void_bioshock_movie_call(movie, "PortSet", key, result);
    /* The live HUD follows a new interface scale at once. */
    if (value && !strcmp(key, "uiScale")) void_bioshock_flash_relayout();
    /* HDR rendering switches on the next frame (the engine's own render config). */
    if (!strcmp(key, "hdr") && void_bioshock_render_hdr) void_bioshock_render_hdr(!strcmp(result, "1"));
    if (!strcmp(key, "renderScale") && void_bioshock_render_scale) void_bioshock_render_scale(atoi(result));
    if (!strcmp(key, "upscaler") && void_bioshock_upscaler) void_bioshock_upscaler(atoi(result));
    /* Game tweaks and fixes (bioshock_tweaks.c) follow a write at once. */
    if (value && void_bioshock_tweak) void_bioshock_tweak(key, result);
}

static void settings_fscommand(void)
{
    char command[64];
    if (!void_vm_readable(g_esp, 20) || !guest_string(MEM32(g_esp + 8), command, sizeof command)) {
        sub_10A65370();
        return;
    }
    char argument[256];
    if (!strcmp(command, "PortTouchContext")) {
        if (void_bioshock_touch_context && guest_string(MEM32(g_esp + 12), argument, sizeof argument) &&
            argument[0] >= '0' && argument[0] <= '9') {
            char *end;
            unsigned long flags = strtoul(argument, &end, 10);
            if (!*end && flags <= 31) void_bioshock_touch_context(MEM32(g_esp + 4), (uint32_t)flags);
        }
        g_eax = 0;
        g_esp += 4;
        return;
    }
    if (strcmp(command, "PortSettings")) {
        sub_10A65370();
        return;
    }
    uint32_t movie = pause_movie();
    if (movie && guest_string(MEM32(g_esp + 12), argument, sizeof argument)) {
        if (!strcmp(argument, "refresh") || !strcmp(argument, "refreshMovement")) {
            unsigned end = sizeof setting_keys / sizeof *setting_keys - SETTINGS_COMMANDS;
            unsigned first = end - MOVEMENT_SETTINGS;
            int movement = !strcmp(argument, "refreshMovement");
            for (unsigned i = movement ? first : 0; i < (movement ? end : first); ++i)
                publish_setting(movie, setting_keys[i], NULL);
        } else {
            char *equals = strchr(argument, '=');
            if (equals) {
                *equals++ = 0;
                if (setting_key(argument) > 0 && strlen(equals) <= 128)
                    publish_setting(movie, argument, equals);
            }
        }
    }
    g_eax = 0;
    g_esp += 4; /* The original callback is cdecl; its caller retires four arguments. */
}

void void_bioshock_settings_register(void)
{
    if (!void_dispatch_find(0x10A65370u)) void_fatal("original Flash FS callback is not lifted");
    void_dispatch_add(0x10A65370u, settings_fscommand);
}

void void_bioshock_settings_poll(void)
{
    static int render_setting_loaded;
    static uint32_t language_movie;
    if (void_bioshock_tweaks_poll) void_bioshock_tweaks_poll();
    if (void_machine_is_owned()) {
        uint32_t movie = pause_movie();
        if (movie && movie != language_movie) {
            const char *language = getenv("PORT_OPTION_GAME_LANGUAGE");
            void_bioshock_movie_call(movie, "PortLanguage", language ? language : "int", "");
            if (void_android_port_setting) {
                const char *const keys[] = {"uiScale", "renderResolutions", "renderScale"};
                char value[256];
                for (unsigned i = 0; i < sizeof keys / sizeof *keys; ++i)
                    if (!void_android_port_setting(keys[i], NULL, value, sizeof value) && *value)
                        void_bioshock_movie_call(movie, "PortGraphicsSet", keys[i], value);
            }
        }
        language_movie = movie;
    }
    if (void_machine_is_owned() && void_bioshock_render_hdr) {
        if (!render_setting_loaded && void_android_port_setting) {
            char value[4096];
            render_setting_loaded = 1;
            int error = void_android_port_setting("hdr", NULL, value, sizeof value);
            if (!error) void_bioshock_render_hdr(!strcmp(value, "1"));
            void_log("[render] HDR setting %s", error ? "unavailable" : value);
            if (void_bioshock_render_scale && !void_android_port_setting("renderScale", NULL, value, sizeof value))
                void_bioshock_render_scale(atoi(value));
            if (void_bioshock_upscaler && !void_android_port_setting("upscaler", NULL, value, sizeof value))
                void_bioshock_upscaler(atoi(value));
        }
        if (void_bioshock_render_poll) void_bioshock_render_poll();
    }
    if (!void_machine_is_owned() || !__atomic_load_n(&settings_requested, __ATOMIC_ACQUIRE)) return;
    uint32_t movie = pause_movie();
    if (!movie) {
        if (void_monotonic_ns() >= __atomic_load_n(&settings_deadline, __ATOMIC_RELAXED))
            __atomic_store_n(&settings_requested, 0, __ATOMIC_RELEASE);
        return;
    }
    int page = __atomic_exchange_n(&settings_requested, 0, __ATOMIC_ACQ_REL);
    if (page) {
        void_bioshock_movie_call(movie, page == 2 ? "PortOpenControls" : "PortOpenAdditional", NULL, NULL);
        void_log("[port-settings] opening additional settings page %d movie=%08X", page, movie);
    }
}

#ifdef UT3_ANDROID_APP
JNIEXPORT void JNICALL Java_org_bioshock_port_GameActivity_nativeSettings(JNIEnv *env, jclass cls, jboolean controls)
{
    (void)env; (void)cls;
    /* Keep the request while Escape opens the stock menu, including with touch off. */
    __atomic_store_n(&settings_deadline, void_monotonic_ns() + 5000000000ull, __ATOMIC_RELAXED);
    __atomic_store_n(&settings_requested, controls ? 2 : 1, __ATOMIC_RELEASE);
}
#endif

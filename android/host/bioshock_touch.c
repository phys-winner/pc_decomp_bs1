#include "host.h"
#include "input.h"
#include <math.h>
#ifdef UT3_ANDROID_APP
#include <jni.h>
#endif

extern int void_machine_is_owned(void);
extern int void_input_pad_present(void) __attribute__((weak));
extern int void_input_pad_prompt(void) __attribute__((weak));
extern unsigned void_input_pad_menu(int enabled) __attribute__((weak));
extern void void_bioshock_movie_call(uint32_t movie, const char *method, const char *key, const char *value);
static void touch_list_follow(uint32_t scene, int y);

static uint32_t touch_epoch = 1;
static int touch_gamepad_present;
static uint32_t touch_live_pawn;   /* the player's pawn during touch gameplay, else 0 */
static uint32_t touch_live_player, touch_hud_movie, touch_context_movie, touch_context_flags;
enum { SCENE_INPUT_NORMAL, SCENE_INPUT_LOCKED, SCENE_INPUT_MOVEMENT };
static int touch_scene_input;
static uint32_t touch_options = 3; /* enabled, touch prompts */
static int touch_aim, touch_aim_ready;

void void_bioshock_touch_options(int enabled, int prompts)
{
    __atomic_store_n(&touch_options, (enabled ? 1u : 0u) | (prompts ? 2u : 0u), __ATOMIC_RELEASE);
}

int void_bioshock_touch_enabled(void)
{
    return (__atomic_load_n(&touch_options, __ATOMIC_ACQUIRE) & 1u) != 0;
}

void void_bioshock_touch_aim(int on)
{
    __atomic_store_n(&touch_aim, on ? 1 : 0, __ATOMIC_RELEASE);
}

int void_bioshock_touch_aim_active(void)
{
    return __atomic_load_n(&touch_aim, __ATOMIC_ACQUIRE) &&
        __atomic_load_n(&touch_aim_ready, __ATOMIC_ACQUIRE);
}

#ifdef UT3_ANDROID_APP
JNIEXPORT void JNICALL Java_org_bioshock_port_GameActivity_nativeTouchOptions(JNIEnv *env, jclass cls,
    jboolean enabled, jboolean prompts)
{
    (void)env; (void)cls;
    void_bioshock_touch_options(enabled, prompts);
}

JNIEXPORT void JNICALL Java_org_bioshock_port_GameActivity_nativeTouchAim(JNIEnv *env, jclass cls, jboolean on)
{
    (void)env; (void)cls;
    void_bioshock_touch_aim(on == JNI_TRUE);
}
#endif

static int touch_member(uint32_t object, uint32_t offset)
{
    return object >= 0x10000u && offset <= 65536u && object <= UINT32_MAX - offset &&
        void_vm_readable(object + offset, 4);
}

static uint32_t touch_class(uint32_t object)
{
    return touch_member(object, 44) ? MEM32(object + 44) : 0;
}

static char *touch_name(uint32_t name)
{
    uint32_t table = MEM32(0x11297070), count = MEM32(0x11297074);
    if (!table || name >= count || name > (UINT32_MAX - table) / 4 ||
        !void_vm_readable(table + 4 * name, 4)) return NULL;
    uint32_t entry = MEM32(table + 4 * name);
    return touch_member(entry, 16) ? void_wstr_utf8(entry + 16) : NULL;
}

static char *touch_object_name(uint32_t object)
{
    return touch_member(object, 36) ? touch_name(MEM32(object + 36)) : NULL;
}

static char *touch_class_name(uint32_t object)
{
    return touch_object_name(touch_class(object));
}

static int touch_is_a(uint32_t cls, const char *expected)
{
    for (unsigned depth = 0; depth < 16 && cls >= 0x10000u; ++depth) {
        char *name = touch_object_name(cls);
        int matches = name && !strcmp(name, expected);
        free(name);
        if (matches) return 1;
        if (!touch_member(cls, 56)) return 0;
        uint32_t next = MEM32(cls + 56);
        if (next == cls) return 0;
        cls = next;
    }
    return 0;
}

static uint32_t touch_typed_field(uint32_t cls, const char *expected, const char *expected_type)
{
    for (unsigned depth = 0; depth < 16 && cls >= 0x10000u; ++depth) {
        if (!touch_member(cls, 84)) return 0;
        uint32_t field = MEM32(cls + 84);
        for (unsigned i = 0; i < 4096 && field >= 0x10000u; ++i) {
            char *name = touch_object_name(field);
            char *type = touch_class_name(field);
            int matches = name && type && !strcmp(name, expected) && !strcmp(type, expected_type);
            free(name);
            free(type);
            if (matches) return field;
            if (!touch_member(field, 60)) return 0;
            uint32_t next = MEM32(field + 60);
            if (next == field) return 0;
            field = next;
        }
        if (!touch_member(cls, 56)) return 0;
        uint32_t next = MEM32(cls + 56);
        if (next == cls) return 0;
        cls = next;
    }
    return 0;
}

static uint32_t touch_typed_property(uint32_t cls, const char *expected, const char *expected_type)
{
    uint32_t field = touch_typed_field(cls, expected, expected_type);
    return field && touch_member(field, 108) ? MEM32(field + 108) : 0;
}

static uint32_t touch_property(uint32_t cls, const char *expected)
{
    return touch_typed_property(cls, expected, "ObjectProperty");
}

/* Saved controller aim bits, restored when touch gameplay ends or a physical pad arrives. */
typedef struct {
    uint32_t object, offset, mask;
    int saved_on, valid;
} touch_aim_slot;
static touch_aim_slot touch_aim_soft, touch_aim_sticky;

static void touch_aim_restore(touch_aim_slot *slot)
{
    if (!slot->valid) return;
    if (slot->mask && touch_member(slot->object, slot->offset) &&
        void_vm_writable(slot->object + slot->offset, 4)) {
        uint32_t value = MEM32(slot->object + slot->offset);
        if (slot->saved_on) value |= slot->mask;
        else value &= ~slot->mask;
        MEM32(slot->object + slot->offset) = value;
    }
    slot->valid = 0;
}

static void touch_aim_write(touch_aim_slot *slot, uint32_t object, uint32_t field, int on)
{
    if (!field || !touch_member(field, 148)) return;
    uint32_t offset = touch_member(field, 108) ? MEM32(field + 108) : 0;
    uint32_t mask = MEM32(field + 148);
    if (!offset || !mask || offset > 65532u || (offset & 3u) ||
        !touch_member(object, offset) || !void_vm_writable(object + offset, 4)) return;
    if (slot->valid && (slot->object != object || slot->offset != offset || slot->mask != mask))
        touch_aim_restore(slot);
    uint32_t value = MEM32(object + offset);
    int current = (value & mask) != 0;
    if (!slot->valid) {
        slot->object = object;
        slot->offset = offset;
        slot->mask = mask;
        slot->saved_on = current;
        slot->valid = 1;
    }
    if (current != on) MEM32(object + offset) = on ? value | mask : value & ~mask;
}

static void touch_apply_aim(uint32_t player)
{
    int physical = void_input_pad_present && void_input_pad_present();
    int apply = touch_gamepad_present && !physical && player;
    if (!apply) {
        __atomic_store_n(&touch_aim_ready, 0, __ATOMIC_RELEASE);
        touch_aim_restore(&touch_aim_soft);
        touch_aim_restore(&touch_aim_sticky);
        return;
    }
    static uint32_t owner_class, input_field, input_class, soft_field, sticky_field;
    static int missing_logged;
    uint32_t cls = touch_class(player);
    if (cls != owner_class) {
        owner_class = cls;
        input_field = touch_property(cls, "PlayerInput");
        input_class = 0;
    }
    uint32_t player_input = input_field && touch_member(player, input_field) ? MEM32(player + input_field) : 0;
    uint32_t input_cls = touch_class(player_input);
    if (!player_input || input_cls != input_class) {
        touch_aim_restore(&touch_aim_soft);
        touch_aim_restore(&touch_aim_sticky);
        input_class = input_cls;
        soft_field = input_cls ? touch_typed_field(input_cls, "SoftLockOn", "BoolProperty") : 0;
        sticky_field = input_cls ? touch_typed_field(input_cls, "StickyTargetOn", "BoolProperty") : 0;
        if (input_cls && !soft_field && !sticky_field && !missing_logged) {
            missing_logged = 1;
            void_log("[touch] SoftLockOn/StickyTargetOn unavailable on PlayerInput; keeping direct touch look");
        }
    }
    int enabled = __atomic_load_n(&touch_aim, __ATOMIC_ACQUIRE);
    if (player_input) {
        touch_aim_write(&touch_aim_soft, player_input, soft_field, enabled);
        touch_aim_write(&touch_aim_sticky, player_input, sticky_field, enabled);
    }
    int ready = enabled && (touch_aim_soft.valid || touch_aim_sticky.valid);
    if (ready && !__atomic_load_n(&touch_aim_ready, __ATOMIC_RELAXED))
        void_log("[touch] aim assist active (SoftLock=%d, StickyTargeting=%d)", touch_aim_soft.valid, touch_aim_sticky.valid);
    __atomic_store_n(&touch_aim_ready, ready, __ATOMIC_RELEASE);
}

static int touch_scene_input_mode(uint32_t viewport)
{
    /* UViewport::Input and the live FName used by GETINPUTCONTEXT (10BAAFC0). */
    uint32_t input = touch_member(viewport, 124) ? MEM32(viewport + 124) : 0;
    if (!touch_member(input, 0) || MEM32(input) != 0x1103CB3Cu || !touch_member(input, 1960) || MEM32(input + 1960))
        return SCENE_INPUT_NORMAL;
    uint32_t name = MEM32(input + 1956);
    static uint32_t previous_name = UINT32_MAX;
    static int mode;
    if (name != previous_name) {
        char *context = touch_name(name);
        if (!context) return SCENE_INPUT_NORMAL;
        mode = !strcmp(context, "NullInput") ? SCENE_INPUT_LOCKED :
               !strcmp(context, "MovementOnly") ? SCENE_INPUT_MOVEMENT : SCENE_INPUT_NORMAL;
        previous_name = name;
        free(context);
    }
    return mode;
}

static void touch_apply_fov(uint32_t player)
{
    extern int void_bioshock_fov(int foreground) __attribute__((weak));
    if (!void_bioshock_fov) return;
    static const char *const properties[] = {"DefaultFOV", "DesiredFOV", "FovAngle", "DefaultForegroundFOV", "ForegroundFovAngle"};
    static uint32_t cls, offsets[5];
    uint32_t current = touch_class(player);
    if (current != cls) {
        cls = current;
        for (unsigned i = 0; i < 5; ++i) offsets[i] = touch_typed_property(cls, properties[i], "FloatProperty");
    }
    for (unsigned first = 0; first < 5; first += 3) {
        float wanted = (float)void_bioshock_fov(first != 0);
        uint32_t offset = offsets[first];
        if (offset < 48 || !touch_member(player, offset) || !void_vm_writable(player + offset, 4)) continue;
        float previous = MEMF(player + offset);
        if (previous == wanted) continue;
        MEMF(player + offset) = wanted;
        /* Retarget plain view angles while preserving active scripted angles and zoom. */
        for (unsigned i = first + 1; i < first + 3 && i < 5; ++i) {
            offset = offsets[i];
            if (offset >= 48 && touch_member(player, offset) && MEMF(player + offset) == previous &&
                void_vm_writable(player + offset, 4)) MEMF(player + offset) = wanted;
        }
    }
}

/* QA (BIOSHOCK_DUMP_PROPERTIES=1): once per class, every reflected property of
 * a live object with its type, offset and current value; an object value names
 * its class. Read-only; for choosing gameplay fields, never shipped enabled. */
static void touch_dump_properties(const char *label, uint32_t object)
{
    static int enabled = -1;
    static uint32_t dumped[32];
    static unsigned dumped_count;
    if (enabled < 0) {
        const char *env = getenv("BIOSHOCK_DUMP_PROPERTIES");
        enabled = env && *env == '1';
    }
    uint32_t cls = touch_class(object);
    if (!enabled || !cls) return;
    for (unsigned i = 0; i < dumped_count; ++i) if (dumped[i] == cls) return;
    if (dumped_count == sizeof dumped / sizeof *dumped) return;
    dumped[dumped_count++] = cls;
    char *class_name = touch_object_name(cls);
    void_log("[props] %s object=%08X class=%s", label, object, class_name ? class_name : "?");
    free(class_name);
    for (unsigned depth = 0; depth < 16 && cls >= 0x10000u; ++depth) {
        char *owner = touch_object_name(cls);
        uint32_t field = touch_member(cls, 84) ? MEM32(cls + 84) : 0;
        for (unsigned i = 0; i < 4096 && field >= 0x10000u; ++i) {
            char *name = touch_object_name(field);
            char *type = touch_class_name(field);
            if (name && type && strstr(type, "Property") && touch_member(field, 108)) {
                uint32_t offset = MEM32(field + 108);
                uint32_t value = offset <= 65532u && touch_member(object, offset) ? MEM32(object + offset) : 0;
                char *detail = NULL;
                if (!strcmp(type, "ObjectProperty")) detail = touch_class_name(value);
                else if (!strcmp(type, "ClassProperty")) detail = touch_object_name(value);
                if (!strcmp(type, "BoolProperty") && touch_member(field, 148))
                    void_log("[props] %s.%s %s +%u = %u (mask %08X)", owner ? owner : "?", name, type, offset,
                        (value & MEM32(field + 148)) != 0, MEM32(field + 148));
                else if (!strcmp(type, "ArrayProperty"))
                    void_log("[props] %s.%s %s +%u = data %08X count %u", owner ? owner : "?", name, type, offset, value,
                        offset <= 65528u && touch_member(object, offset + 4) ? MEM32(object + offset + 4) : 0);
                else if (!strcmp(type, "FloatProperty")) {
                    float f; memcpy(&f, &value, 4);
                    void_log("[props] %s.%s %s +%u = %g", owner ? owner : "?", name, type, offset, (double)f);
                } else
                    void_log("[props] %s.%s %s +%u = %08X%s%s", owner ? owner : "?", name, type, offset, value,
                        detail ? " " : "", detail ? detail : "");
                free(detail);
            }
            free(name);
            free(type);
            if (!touch_member(field, 60)) break;
            uint32_t next = MEM32(field + 60);
            if (next == field) break;
            field = next;
        }
        free(owner);
        if (!touch_member(cls, 56)) break;
        uint32_t next = MEM32(cls + 56);
        if (next == cls) break;
        cls = next;
    }
}

/* QA: the named object properties of a dumped object, one level down. */
static void touch_dump_property_objects(uint32_t object, const char *const *names, unsigned count)
{
    uint32_t cls = touch_class(object);
    for (unsigned i = 0; i < count; ++i) {
        uint32_t offset = touch_property(cls, names[i]);
        uint32_t value = offset && touch_member(object, offset) ? MEM32(object + offset) : 0;
        if (value) touch_dump_properties(names[i], value);
    }
}

static uint32_t menu_scene;
static int menu_control = 1;
static int menu_brightness;
static int menu_picker;
static int menu_pause;

static int touch_movie_input(uint32_t movie)
{
    if (!touch_member(movie, 268) || (MEM32(movie + 256) & 2u)) return 0;
    /* Original 10A65FF0 builds two-byte key ranges; 10A758E0 consumes them. */
    uint32_t ranges = MEM32(movie + 228), count = MEM32(movie + 232);
    return count && count <= 256 && ranges >= 0x10000u &&
        ranges <= UINT32_MAX - count * 2 && void_vm_readable(ranges, count * 2);
}

static int touch_movie_menu(uint32_t viewport)
{
    menu_scene = 0;
    touch_hud_movie = 0;
    menu_control = 1;
    menu_brightness = 0;
    static uint32_t viewport_class, gui_offset;
    uint32_t cls = touch_class(viewport);
    if (cls != viewport_class) {
        viewport_class = cls;
        gui_offset = touch_is_a(cls, "Viewport") ? touch_property(cls, "GUIController") : 0;
    }
    uint32_t gui = gui_offset && touch_member(viewport, gui_offset) ? MEM32(viewport + gui_offset) : 0;
    static uint32_t gui_class, movies_offset;
    cls = touch_class(gui);
    if (cls != gui_class) {
        gui_class = cls;
        movies_offset = touch_is_a(cls, "FlashGUIController") ?
            touch_typed_property(cls, "PlayingMovies", "ArrayProperty") : 0;
    }
    uint32_t signature = 0;
    int menu = 0;
    menu_picker = 0;
    menu_pause = 0;
    if (!movies_offset || !touch_member(gui, movies_offset + 4)) goto done;
    uint32_t array = MEM32(gui + movies_offset), count = MEM32(gui + movies_offset + 4);
    if (!count) goto done;
    if (count > 16 || !touch_member(array, (count - 1) * 4)) goto done;
    for (uint32_t i = 0; i < count; ++i) {
        uint32_t movie = MEM32(array + i * 4);
        char *name = touch_object_name(movie);
        if (name) {
            if (!strcmp(name, "HUD") && touch_member(movie, 268) && MEM32(movie + 268)) touch_hud_movie = movie;
            if (strcmp(name, "HUD") && touch_movie_input(movie)) {
                menu = 1;
                menu_scene = movie;
                menu_brightness = !strcmp(name, "NTSCColorBars");
                menu_picker = !strcmp(name, "PCWeaponSelection");
                menu_pause = !strcmp(name, "Pause");
                signature = movie * 33u + MEM32(movie + 268);
                for (const unsigned char *p = (const unsigned char *)name; *p; ++p) signature = signature * 33u + *p;
                menu_control = !strcmp(name, "Hacking") || !strcmp(name, "Maps") ||
                    !strcmp(name, "PlasmidEquip") ? 1 | 4 | 16 : !strcmp(name, "Pause") ? 1 | 4 : 1;
            }
        }
        free(name);
    }
done: {
    static uint32_t previous_signature;
    static int previous_menu = -1;
    if (signature != previous_signature) {
        ++touch_epoch;
        previous_signature = signature;
    }
    if (menu != previous_menu) {
        void_log("[touch] movies menu=%u signature=%08X", menu, signature);
        previous_menu = menu;
    }
    return menu;
}
}

/* Port setting "hdr" (off by default). The original renderer always runs its FP16 HDR pipeline:
 * scene and bloom in FP16 targets and the scene copies that water refraction, distortion and soft
 * particles sample. It decodes sRGB textures to linear (the *_SRGB formats of its textures) and
 * encodes the tonemapped result with pow(1/2.2) because UseLinearSpace (D3DRenderDevice10 +248,
 * D3D9's D3DRenderDevice +268) is set to 1 by its init (sub_10980B60), so dark linear values come
 * out lifted (a haze). "HDR off" keeps that pipeline but computes in the game's non-HDR color space:
 * the native renderer samples the sRGB textures without decode (void_bioshock_srgb_textures) and the
 * device flag is cleared, so the tonemap writes the color as is (deep blacks, more contrast). The
 * flag is re-applied after device resets; the tonemap shader permutation follows on the next frame.
 * D3D10's +268 is UseHighDetailShadowMaps ("DirectX 10 Detail Surfaces"): never touched here. */
static int render_hdr = -1;

void void_bioshock_render_hdr(int wanted)
{
    __atomic_store_n(&render_hdr, wanted ? 1 : 0, __ATOMIC_RELEASE);
}

/* 0 while textures must be sampled without sRGB decode ("HDR off"); read by the native renderer. */
int void_bioshock_srgb_textures(void)
{
    return __atomic_load_n(&render_hdr, __ATOMIC_ACQUIRE) != 0;
}

void void_bioshock_render_poll(void)
{
    static unsigned ticks;
    static uint32_t checked_device;
    int wanted = __atomic_load_n(&render_hdr, __ATOMIC_ACQUIRE);
    if (wanted < 0 || !void_machine_is_owned() || ticks++ % 15) return;
    uint32_t engine = MEM32(0x111BE314u), device = touch_member(engine, 0x4C) ? MEM32(engine + 0x4C) : 0;
    if (!touch_member(device, 568)) return;
    if (device != checked_device) {
        char *name = touch_class_name(device);
        int matches = name && !strcmp(name, "D3DRenderDevice10");
        free(name);
        if (!matches) return;
        checked_device = device;
    }
    if (MEM32(device + 248) != (uint32_t)wanted) {
        void_log("[render] HDR %s (D3DRenderDevice10 %08X UseLinearSpace %u -> %d)", wanted ? "on" : "off",
            device, MEM32(device + 248), wanted);
        MEM32(device + 248) = (uint32_t)wanted;
    }
}

/* The UFunction `name` of the live object's class chain (QA console path), or 0. */
uint32_t void_bioshock_find_function(uint32_t object, const char *name)
{
    return void_machine_is_owned() ? touch_typed_field(touch_class(object), name, "Function") : 0;
}

int void_bioshock_is_a(uint32_t object, const char *name)
{
    return touch_is_a(touch_class(object), name);
}

/* A playing original Flash movie by its object name ("HUD"), or 0. Machine owned. */
uint32_t void_bioshock_named_movie(const char *wanted)
{
    if (!void_machine_is_owned()) return 0;
    static uint32_t engine_class, client_offset, viewport_class, gui_offset, gui_class, movies_offset;
    uint32_t engine = MEM32(0x111BE314);
    uint32_t cls = touch_class(engine);
    if (cls != engine_class) {
        engine_class = cls;
        client_offset = touch_is_a(cls, "Engine") ? touch_property(cls, "Client") : 0;
    }
    uint32_t client = client_offset && touch_member(engine, client_offset) ? MEM32(engine + client_offset) : 0;
    uint32_t viewport = client && touch_is_a(touch_class(client), "Client") && touch_member(client, 196) ? MEM32(client + 196) : 0;
    cls = touch_class(viewport);
    if (cls != viewport_class) {
        viewport_class = cls;
        gui_offset = touch_is_a(cls, "Viewport") ? touch_property(cls, "GUIController") : 0;
    }
    uint32_t gui = gui_offset && touch_member(viewport, gui_offset) ? MEM32(viewport + gui_offset) : 0;
    cls = touch_class(gui);
    if (cls != gui_class) {
        gui_class = cls;
        movies_offset = touch_is_a(cls, "FlashGUIController") ?
            touch_typed_property(cls, "PlayingMovies", "ArrayProperty") : 0;
    }
    if (!movies_offset || !touch_member(gui, movies_offset + 4)) return 0;
    uint32_t array = MEM32(gui + movies_offset), count = MEM32(gui + movies_offset + 4);
    if (!count || count > 16 || !touch_member(array, (count - 1) * 4)) return 0;
    for (uint32_t i = 0; i < count; ++i) {
        uint32_t movie = MEM32(array + i * 4);
        char *name = touch_object_name(movie);
        int match = name && !strcmp(name, wanted);
        free(name);
        if (match) return movie;
    }
    return 0;
}

/* Original HUDMessage reports availability after the engine's distance and CanBe* checks. */
void void_bioshock_touch_context(uint32_t source, uint32_t flags)
{
    if (!void_machine_is_owned() || flags > 31) return;
    uint32_t movie = void_bioshock_named_movie("HUD");
    if (!movie || !touch_member(movie, 268) || !MEM32(movie + 268)) return;
    static unsigned context_logs;
    uint32_t wanted = MEM32(movie + 268);
    /* The FS callback's first argument is the sending gameswf movie_root, not its UObject. */
    if (source != wanted) {
        if (context_logs++ < 128)
            void_log("[touch-context] rejected flags=%u source=%08X wanted=%08X movie=%08X", flags, source, wanted, movie);
        return;
    }
    if ((touch_context_movie != movie || (touch_context_flags & (31u << 4)) != (flags << 4)) && context_logs++ < 128)
        void_log("[touch-context] flags=%u previous=%u source=%08X wanted=%08X movie=%08X", flags,
            (touch_context_flags >> 4) & 31u, source, wanted, movie);
    touch_context_movie = movie;
    touch_context_flags = (flags << 4) | (1u << 9);
}

void void_bioshock_touch_xinput_state(uint32_t index, int connected)
{
    if (index || !void_machine_is_owned()) return;
    static int previous_source; /* Last successful slot zero: virtual or physical. */
    int physical = void_input_pad_present && void_input_pad_present();
    if (connected) { previous_source = physical ? 2 : 1; return; }
    /* Retire an expected virtual disconnect before original UpdateInput reads its latch. */
    if (previous_source == 1 && !void_bioshock_touch_enabled() && !physical &&
        void_vm_writable(0x11171E38u, 4)) MEM32(0x11171E38u) = 0;
}

int void_touch_game_read(uint32_t out[4])
{
    if (!void_machine_is_owned()) return -1;
    static int previous = -1;
    static uint32_t engine_class, client_offset;
    static uint32_t client_class, client_valid, logged_client;
    static uint32_t viewport_class, actor_offset;
    static uint32_t player_class, player_offset, pawn_offset, player_valid;
    static uint32_t pawn_class, controller_offset, pawn_valid, photo_offset, photo_mask;
    static uint32_t previous_viewport, previous_player, previous_pawn;
    uint32_t engine = MEM32(0x111BE314);
    uint32_t cls = touch_class(engine);
    if (cls != engine_class) {
        engine_class = cls;
        client_offset = touch_is_a(cls, "Engine") ? touch_property(cls, "Client") : 0;
    }
    uint32_t client = client_offset && touch_member(engine, client_offset) ? MEM32(engine + client_offset) : 0;
    cls = touch_class(client);
    if (cls != client_class) {
        client_class = cls;
        client_valid = touch_is_a(cls, "Client");
    }
    uint32_t viewport = client_valid && touch_member(client, 196) ? MEM32(client + 196) : 0;
    cls = touch_class(viewport);
    if (cls != viewport_class) {
        viewport_class = cls;
        actor_offset = touch_is_a(cls, "Viewport") ? touch_property(cls, "Actor") : 0;
        char *name = touch_class_name(viewport);
        void_log("[touch] engine=%08X client=%08X viewport=%08X class=%s Actor offset=%u",
            engine, client, viewport, name ? name : "?", actor_offset);
        free(name);
    }
    uint32_t player = actor_offset && touch_member(viewport, actor_offset) ? MEM32(viewport + actor_offset) : 0;
    cls = touch_class(player);
    if (cls != player_class) {
        player_class = cls;
        player_valid = touch_is_a(cls, "PlayerController");
        player_offset = player_valid ? touch_property(cls, "Player") : 0;
        pawn_offset = player_valid ? touch_property(cls, "Pawn") : 0;
    }
    uint32_t pawn = player_valid && pawn_offset && touch_member(player, pawn_offset) ? MEM32(player + pawn_offset) : 0;
    cls = touch_class(pawn);
    if (cls != pawn_class) {
        pawn_class = cls;
        pawn_valid = touch_is_a(cls, "Pawn");
        controller_offset = pawn_valid ? touch_property(cls, "Controller") : 0;
        photo_offset = photo_mask = 0;
        uint32_t field = pawn_valid && touch_is_a(cls, "ShockPlayer") ?
            touch_typed_field(cls, "IsDisplayingPhoto", "BoolProperty") : 0;
        if (field && touch_member(field, 108) && touch_member(field, 148)) {
            uint32_t offset = MEM32(field + 108), mask = MEM32(field + 148);
            /* Original InstanceVariable/1095F780 uses +0x6C; the packed
             * BoolVariable/1095E0C0 reads the property's +0x94 mask. */
            if (offset >= 48 && offset <= 65532 && !(offset & 3u) && mask && !(mask & (mask - 1))) {
                photo_offset = offset;
                photo_mask = mask;
            }
        }
    }
    int playing = player_valid && player_offset && pawn_valid && controller_offset &&
        touch_member(player, player_offset) && MEM32(player + player_offset) == viewport &&
        touch_member(pawn, controller_offset) && MEM32(pawn + controller_offset) == player;
    if (!playing) pawn = 0;
    if (viewport != previous_viewport || player != previous_player || pawn != previous_pawn) {
        ++touch_epoch;
        previous_viewport = viewport;
        previous_player = player;
        previous_pawn = pawn;
        if (pawn) {
            static const char *const linked[] = { "Weapon", "PendingWeapon", "Inventory", "InvManager",
                "PlasmidManager", "AbilityManager", "WeaponManager", "InventoryManager", "Controller", "myHUD", "HUD" };
            touch_dump_properties("pawn", pawn);
            touch_dump_properties("player", player);
            touch_dump_property_objects(pawn, linked, sizeof linked / sizeof *linked);
            touch_dump_property_objects(player, linked, sizeof linked / sizeof *linked);
        }
    }
    int menu = touch_movie_menu(viewport);
    static uint32_t previous_picker;
    static uint64_t reveal_picker_at;
    uint32_t picker = menu_picker ? menu_scene : 0;
    if (picker != previous_picker) {
        previous_picker = picker;
        // The first Flash frames contain the unmasked opening artwork.
        reveal_picker_at = picker ? void_monotonic_ns() + 135000000u : 0;
    }
    if (reveal_picker_at && void_monotonic_ns() >= reveal_picker_at) {
        reveal_picker_at = 0;
        void_bioshock_movie_call(picker, "PortPickerReveal", NULL, NULL);
    }
    {
        static uint32_t published_hud;
        static int published_prompt = -1;
        uint32_t options = __atomic_load_n(&touch_options, __ATOMIC_ACQUIRE);
        int prompt = void_input_pad_prompt ? void_input_pad_prompt() : 0;
        if (!prompt && options == 3) prompt = 1;
        if (touch_hud_movie && (touch_hud_movie != published_hud || prompt != published_prompt)) {
            extern void void_bioshock_movie_call(uint32_t, const char *, const char *, const char *);
            const char *language = getenv("PORT_OPTION_GAME_LANGUAGE");
            static const char *const modes[] = {"0", "1", "2", "3"};
            void_bioshock_movie_call(touch_hud_movie, "PortTouchPrompts", modes[prompt], language ? language : "int");
            published_prompt = prompt;
        }
        published_hud = touch_hud_movie;
    }
    /* Research grading is drawn through HUD, not an input-bearing movie.
     * Its authoritative ShockPlayer flag guards FinishedPhotoDisplay. */
    int photo = playing && !menu && photo_mask && touch_member(pawn, photo_offset) &&
        (MEM32(pawn + photo_offset) & photo_mask);
    int use_controller = playing && !menu && !photo;
    if (use_controller && (previous != use_controller || logged_client != player)) {
        extern void void_bioshock_flash_relayout(void);
        void_bioshock_flash_relayout();
    }
    /* The options bit hides on-screen controls and their HUD prompts. Gameplay touch stays connected. */
    touch_gamepad_present = use_controller;
    {
        /* QA: HUD gauge trace a few seconds into each gameplay stretch. */
        extern void void_bioshock_flash_trace(void);
        static unsigned trace_polls;
        trace_polls = use_controller ? trace_polls + 1 : 0;
        if (trace_polls == 40 || trace_polls == 400) void_bioshock_flash_trace();
    }
    /* Touch look reaches the game as one record per frame in 1/16 counts
     * (input.c). The original SmoothMouse divides each frame by its records
     * per 8.3 ms mouse sample, so its gain swung with the frame time: apply
     * the game's own SetSmoothingMode 0 to this player's live input object
     * only; the saved setting is unchanged. The port setting mouseRaw
     * (bioshock_tweaks.c) does the same for a physical mouse in gameplay. */
    extern int void_bioshock_mouse_raw(void) __attribute__((weak));
    if (touch_gamepad_present || (playing && !menu && void_bioshock_mouse_raw && void_bioshock_mouse_raw())) {
        static uint32_t input_owner_class, input_field, smooth_class, smooth_field, smoothed;
        uint32_t owner_class = touch_class(player);
        if (owner_class != input_owner_class) {
            input_owner_class = owner_class;
            input_field = touch_property(owner_class, "PlayerInput");
        }
        uint32_t player_input = input_field && touch_member(player, input_field) ? MEM32(player + input_field) : 0;
        uint32_t player_input_class = touch_class(player_input);
        if (player_input_class && player_input_class != smooth_class) {
            smooth_class = player_input_class;
            smooth_field = touch_typed_property(player_input_class, "MouseSmoothingMode", "ByteProperty");
        }
        if (player_input && player_input != smoothed && smooth_field && smooth_field <= 65535u &&
            void_vm_writable(player_input + smooth_field, 1)) {
            unsigned mode = MEM8(player_input + smooth_field);
            MEM8(player_input + smooth_field) = 0;
            smoothed = player_input;
            void_log("[touch] PlayerInput smoothing %u -> 0 for %s", mode, touch_gamepad_present ? "touch look" : "raw mouse");
        }
    }
    /* A physical pad drives the original controller path in menus too: every
     * *UIActive context has XENON_* bindings, and the Pause movie shows "Use
     * Xbox controller"/"Rumble" while the XInput gate is set (10A77CD0). */
    int physical = client_valid && void_input_pad_present && void_input_pad_present();
    int want_controller = touch_gamepad_present || physical;
    static int previous_want = -1;
    if (use_controller != previous || want_controller != previous_want ||
        (use_controller && logged_client != player)) {
        static uint32_t controller_option, restore_controller_option, option_client;
        if (want_controller && !controller_option)
            controller_option = touch_typed_property(touch_class(client), "UseController", "BoolProperty");
        if (want_controller)
            void_log("[touch] UseController metadata offset=%u flags=%08X writable=%u", controller_option,
                controller_option && touch_member(client, controller_option) ? MEM32(client + controller_option) : 0,
                controller_option && touch_member(client, controller_option) ? void_vm_writable(client + controller_option, 4) : 0);
        if (want_controller && controller_option && touch_member(client, controller_option) &&
            !(MEM32(client + controller_option) & 1) && void_vm_writable(client + controller_option, 4)) {
            MEM32(client + controller_option) |= 1;
            restore_controller_option = 1;
            option_client = client;
            void_log("[touch] UseController enabled for %s XInput at offset=%u",
                use_controller ? "virtual" : "physical", controller_option);
        } else if (!want_controller && restore_controller_option && client == option_client &&
            touch_member(client, controller_option) && void_vm_writable(client + controller_option, 4)) {
            MEM32(client + controller_option) &= ~1u;
            restore_controller_option = 0;
            void_log("[touch] UseController disabled for PC menu");
        }
        previous_want = want_controller;
        char *name = touch_class_name(player);
        char *pawn_name = touch_class_name(pawn);
        void_log("[touch] game context viewport=%08X player=%08X class=%s pawn=%08X class=%s ready=%u gameplay_ready=%u",
            viewport, player, name ? name : "?", pawn, pawn_name ? pawn_name : "?", playing,use_controller);
        free(name);
        free(pawn_name);
        previous = use_controller;
        logged_client = player;
    }
    touch_apply_aim(playing ? player : 0);
    touch_live_pawn = playing ? pawn : 0;
    touch_live_player = playing ? player : 0;
    int input_mode = playing ? touch_scene_input_mode(viewport) : SCENE_INPUT_NORMAL;
    if (touch_scene_input != input_mode) void_log("[touch] scripted input mode %d -> %d", touch_scene_input, input_mode);
    touch_scene_input = input_mode;
    extern int void_bioshock_is_cinematic(void) __attribute__((weak));
    if (playing && !menu && input_mode == SCENE_INPUT_NORMAL && (!void_bioshock_is_cinematic || !void_bioshock_is_cinematic()))
        touch_apply_fov(player);
    out[0] = photo ? 3 : use_controller ? 1 : 2;
    out[1] = touch_epoch;
    out[2] = 0;
    out[3] = playing ? player : viewport;
    if (void_input_pad_menu) {
        unsigned buttons = void_input_pad_menu(menu_pause);
        const char *method = buttons & 0x2000 ? "CancelOrExit" : buttons & 0x1000 ? "PortPadAccept" :
                             buttons & 0x10 ? "PortPadStart" : buttons & 1 ? "PortPadUp" :
                             buttons & 2 ? "PortPadDown" : buttons & 4 ? "UIHighlightLeft" :
                             buttons & 8 ? "UIHighlightRight" : NULL;
        if (method) void_bioshock_movie_call(menu_scene, method, NULL, NULL);
    }
    return 1;
}

int void_touch_gamepad_present(void)
{
    return touch_gamepad_present;
}

/* What the hands can do, for the overlay (details word bits 16-20):
 * 16 something is held, 17 a weapon is owned, 18 a plasmid is owned,
 * 19 the held item is the melee Wrench (nothing to reload), 20 valid, 21 picker open,
 * 22 a weapon switch still refers to the previous active holdable.
 * ShockPawn.ActiveHoldable, ShockPlayer.LastWeapon/AvailableAbilities. */
int void_bioshock_scripted_scene(void)
{
    return touch_scene_input == SCENE_INPUT_LOCKED;
}

/* ActionSetHUDDisplayState also hides the normal HUD during playable sections of the prologue. */
static int touch_hud_visible(uint32_t pawn)
{
    static uint32_t cached_class, offset, mask;
    uint32_t cls = touch_class(pawn);
    if (cls != cached_class) {
        cached_class = cls;
        uint32_t field = touch_typed_field(cls, "HudElementsDisabled", "BoolProperty");
        offset = field && touch_member(field, 108) ? MEM32(field + 108) : 0;
        mask = field && touch_member(field, 148) ? MEM32(field + 148) : 0;
    }
    return touch_hud_movie && offset >= 48 && offset <= 65532u && mask && touch_member(pawn, offset) &&
        !(MEM32(pawn + offset) & mask);
}

enum { HANDS_HOLDING = 1u << 16, HANDS_WEAPON = 1u << 17, HANDS_PLASMID = 1u << 18,
       HANDS_MELEE = 1u << 19, HANDS_VALID = 1u << 20, HANDS_SWITCHING = 1u << 22 };
/* Image IDs match tools/extract_ios_controls.py. Unknown classes leave the slot blank. */
static unsigned touch_ios_weapon(uint32_t cls)
{
    static const char *const names[] = {
        "Wrench", "Pistol", "MachineGun", "Shotgun", "GrenadeLauncher",
        "ChemicalThrower", "Crossbow", "ResearchCamera"
    };
    char *name = touch_object_name(cls);
    unsigned id = 0;
    if (name) {
        for (unsigned i = 0; i < sizeof names / sizeof *names; ++i) {
            if (!strcmp(name, names[i])) { id = i + 1; break; }
        }
    }
    free(name);
    return id;
}

static unsigned touch_ios_ability(uint32_t cls)
{
    static const char *const names[] = {
        "ElectricBoltAbility", "ElectricBoltTwoAbility", "ElectricBoltThreeAbility", "TelekinesisAbility",
        "IncinerationAbility", "IncinerationTwoAbility", "IncinerationThreeAbility",
        "IcicleAssaultAbility", "IcicleAssaultTwoAbility", "IcicleAssaultThreeAbility",
        "BerserkRageAbility", "SecurityBeaconAbility", "DecoyHumanAbility",
        "SummonProtectorAbility", "SummonProtectorTwoAbility",
        "InsectSwarmAbility", "InsectSwarmTwoAbility", "InsectSwarmThreeAbility",
        "SpringBoardTrapAbility", "SpringBoardTrapTwoAbility", "AirBlastAbility", "AirBlastTwoAbility"
    };
    char *name = touch_object_name(cls);
    unsigned id = 0;
    if (name) {
        for (unsigned i = 0; i < sizeof names / sizeof *names; ++i) {
            if (!strcmp(name, names[i])) { id = i + 1; break; }
        }
        if (!strcmp(name, "ElectricBoltZeroAbility")) id = 1;
        else if (!strcmp(name, "TelekinesisTwoAbility")) id = 4;
        else if (!strcmp(name, "IncinerationZeroAbility")) id = 5;
        else if (!strcmp(name, "BerserkRageZeroAbility") || !strcmp(name, "EnrageZeroAbility")) id = 11;
    }
    free(name);
    return id;
}

static int touch_read_bool(uint32_t object, uint32_t field)
{
    if (!field || !touch_member(field, 108) || !touch_member(field, 148)) return -1;
    uint32_t offset = MEM32(field + 108), mask = MEM32(field + 148);
    if (!offset || !mask || (offset & 3u) || !touch_member(object, offset)) return -1;
    return (MEM32(object + offset) & mask) != 0;
}

int void_touch_game_details(uint32_t out[2])
{
    extern int void_bioshock_is_cinematic(void) __attribute__((weak));
    out[0] = menu_picker ? 1u << 21 : 0;
    int cinematic = touch_scene_input == SCENE_INPUT_LOCKED || (void_bioshock_is_cinematic && void_bioshock_is_cinematic());
    out[1] = cinematic ? 1u << 12 : 0;
    uint32_t pawn = touch_live_pawn;
    if (!pawn || !void_machine_is_owned()) return 1;
    if (touch_hud_visible(pawn)) out[1] |= 1u << 13;
    if (touch_scene_input == SCENE_INPUT_MOVEMENT) out[1] |= 1u << 14;
    if (touch_gamepad_present && touch_context_movie == touch_hud_movie) out[1] |= touch_context_flags;
    static uint32_t zoom_class, zoom_states, use_focus_offset, use_focus_class, use_focus_actor;
    uint32_t player_class = touch_class(touch_live_player);
    if (player_class != zoom_class) {
        zoom_class = player_class;
        zoom_states = touch_typed_property(player_class, "HeadbobContextStates", "IntProperty");
        use_focus_offset = touch_is_a(player_class, "ShockPlayerController") ? touch_property(player_class, "CurrentUseFocus") : 0;
    }
    /* Original 10CDB610 writes CurrentUseFocus only after CanBeUsedNow and range succeed; HUD text can be empty. */
    out[1] &= ~(1u << 4);
    if (touch_gamepad_present && use_focus_offset >= 48 && use_focus_offset <= 65532 && !(use_focus_offset & 3u) &&
        touch_member(touch_live_player, use_focus_offset)) {
        uint32_t focus = MEM32(touch_live_player + use_focus_offset), cls = touch_class(focus);
        if (cls != use_focus_class) {
            use_focus_class = cls;
            use_focus_actor = touch_is_a(cls, "Actor");
        }
        if (focus && use_focus_actor) out[1] |= (1u << 4) | (1u << 9);
    }
    if (cinematic || touch_scene_input == SCENE_INPUT_MOVEMENT) out[1] &= ~0x1F0u; /* Use, Hack, Rescue, Harvest, Reroll */
    /* EHeadbobContext.kHC_Zooming is slot 2; this counter follows the real Hands zoom state. */
    if (touch_gamepad_present && zoom_states && zoom_states <= 65524 &&
        touch_member(touch_live_player, zoom_states + 8) && (int32_t)MEM32(touch_live_player + zoom_states + 8) > 0)
        out[1] |= 1u << 10;
    static uint32_t pawn_class, holdable, pending_holdable, active_ability, last_weapon, abilities, held_class, held_melee;
    static uint32_t zoom_offset, zoom_mask;
    static uint32_t hands_field, crouch_field, hands_class, weapon_mode_field;
    static uint32_t weapon_art_class, ability_art_class;
    static unsigned weapon_art, ability_art;
    uint32_t cls = touch_class(pawn);
    if (cls != pawn_class) {
        pawn_class = cls;
        holdable = touch_typed_property(cls, "ActiveHoldable", "ObjectProperty");
        pending_holdable = touch_typed_property(cls, "PendingHoldable", "ObjectProperty");
        active_ability = touch_typed_property(cls, "ActiveAbility", "ClassProperty");
        last_weapon = touch_typed_property(cls, "LastWeapon", "ObjectProperty");
        abilities = touch_typed_property(cls, "AvailableAbilities", "ArrayProperty");
        hands_field = touch_property(cls, "Hands");
        crouch_field = touch_typed_field(cls, "bIsCrouched", "BoolProperty");
        held_class = held_melee = 0;
    }
    if (!holdable || !last_weapon || !abilities || !touch_member(pawn, holdable) ||
        !touch_member(pawn, last_weapon) || !touch_member(pawn, abilities + 4)) return 1;
    uint32_t hand = MEM32(pawn + holdable), weapon = MEM32(pawn + last_weapon), plasmids = MEM32(pawn + abilities + 4);
    uint32_t hand_class = touch_class(hand);
    if (hand_class != held_class) {
        held_class = hand_class;
        char *name = hand_class ? touch_object_name(hand_class) : NULL;
        held_melee = name && !strcmp(name, "Wrench");
        free(name);
        zoom_offset = zoom_mask = 0;
        uint32_t field = touch_typed_field(hand_class, "CanBeZoomed", "BoolProperty");
        if (field && touch_member(field, 108) && touch_member(field, 148)) {
            uint32_t offset = MEM32(field + 108), mask = MEM32(field + 148);
            if (offset >= 48 && offset <= 65532 && !(offset & 3u) && mask && !(mask & (mask - 1))) {
                zoom_offset = offset;
                zoom_mask = mask;
            }
        }
    }
    if (touch_gamepad_present && zoom_mask && touch_member(hand, zoom_offset) && (MEM32(hand + zoom_offset) & zoom_mask))
        out[1] |= 1u << 11;
    uint32_t ability = active_ability && touch_member(pawn, active_ability) ? MEM32(pawn + active_ability) : 0;
    uint32_t pending = pending_holdable && touch_member(pawn, pending_holdable) ? MEM32(pawn + pending_holdable) : 0;
    out[0] |= HANDS_VALID | ((hand || ability) ? HANDS_HOLDING : 0) | (weapon ? HANDS_WEAPON : 0) |
             (plasmids && plasmids < 256 ? HANDS_PLASMID : 0) | (hand && held_melee ? HANDS_MELEE : 0) |
             (hand && pending && hand != pending ? HANDS_SWITCHING : 0);
    uint32_t weapon_cls = touch_class(weapon ? weapon : hand);
    if (weapon_cls != weapon_art_class) {
        weapon_art_class = weapon_cls;
        weapon_art = touch_ios_weapon(weapon_cls);
    }
    if (ability != ability_art_class) {
        ability_art_class = ability;
        ability_art = touch_ios_ability(ability);
    }
    out[0] |= weapon_art << 23;
    out[1] |= ability_art << 16;
    uint32_t hands = hands_field && touch_member(pawn, hands_field) ? MEM32(pawn + hands_field) : 0;
    uint32_t hands_cls = touch_class(hands);
    if (hands_cls != hands_class) {
        hands_class = hands_cls;
        // Original Hands.InWeaponsMode() returns bIsInWeaponMode.
        weapon_mode_field = touch_typed_field(hands_cls, "bIsInWeaponMode", "BoolProperty");
    }
    if (ability && touch_read_bool(hands, weapon_mode_field) == 0) out[1] |= 1u << 28;
    if (touch_read_bool(pawn, crouch_field) == 1) out[1] |= 1u << 29;
    return 1;
}

int void_touch_weapon_request(uint32_t request)
{
    unsigned phase = request >> 8, slot = request & 255u;
    if (!void_machine_is_owned() || !menu_picker || !menu_scene || phase < 4 || phase > 7 || slot > 31) return -1;
    extern void void_bioshock_movie_call(uint32_t, const char *, const char *, const char *);
    if (phase <= 5) {
        char step[16];
        snprintf(step, sizeof step, "%d", (int)slot - 16);
        void_bioshock_movie_call(menu_scene, "PortTouchSelect", phase == 4 ? "0" : "1", step);
    } else void_bioshock_movie_call(menu_scene, phase == 6 ? "PortTouchCommit" : "LBReleased", NULL, NULL);
    return 1;
}

uint32_t void_touch_weapon_preview(void)
{
    return 0;
}

int void_touch_steer(float out[3], uint32_t *flags)
{
    (void)out;
    (void)flags;
    return 0;
}

static uint32_t scene_client;

void void_bioshock_note_scene(uint32_t scene)
{
    if (scene >= 0x10000u && void_vm_readable(scene + 268, 4) && MEM32(scene + 268))
        __atomic_store_n(&scene_client, scene, __ATOMIC_RELEASE);
}

static uint32_t touch_ui_scene(void)
{
    uint32_t scene = menu_scene ? menu_scene : __atomic_load_n(&scene_client, __ATOMIC_ACQUIRE);
    return touch_member(scene, 268) && MEM32(scene + 268) ? scene : 0;
}

uint32_t void_bioshock_touch_movie(void)
{
    return void_machine_is_owned() ? touch_ui_scene() : 0;
}

int void_touch_ui_position(int x, int y)
{
    if (!void_machine_is_owned()) return 0;
    uint32_t scene = touch_ui_scene();
    if (!scene) return 0;
    uint32_t vtable = MEM32(scene);
    if (!touch_member(vtable, 228) || MEM32(vtable + 228) != 0x10A65A60u) return 0;
    uint32_t width = MEM32(scene + 104), height = MEM32(scene + 108);
    if (!width || width > 16384 || !height || height > 16384 ||
        x < 0 || y < 0 || x > (int)width || y > (int)height) return 0;
    int old_x = (int)MEM32(scene + 240), old_y = (int)MEM32(scene + 244);
    if (old_x < 0 || old_y < 0 || old_x > (int)width || old_y > (int)height) return 0;
    int dx = x - old_x, dy = old_y - y;
    if (!dx && !dy) return 1;
    uint32_t key = void_galloc(4);
    if (!key) return 0;
    MEM8(key + 1) = 4; /* Original EInputAction IST_Axis. */
    uint32_t args[3] = {key, key + 1, 0};
    void_regs saved;
    void_regs_save(&saved);
#ifdef RECOMP_X87_WINDOW
    double window[RECOMP_X87_SLOTS];
    int64_t integers[RECOMP_X87_SLOTS];
    int sp = g_fp_sp;
    memcpy(window, g_st_win, sizeof window); memcpy(integers, g_st_i64_win, sizeof integers);
#endif
    /* Feed movie pixels at its original axis handler, after GUI mouse gain. */
    if (dx) {
        float delta = (float)dx; MEM8(key) = 0xE4; memcpy(&args[2], &delta, 4);
        void_call_guest_this(0x10A65A60u, scene, 3, args);
    }
    if (dy && scene == touch_ui_scene()) {
        float delta = (float)dy; MEM8(key) = 0xE5; memcpy(&args[2], &delta, 4);
        void_call_guest_this(0x10A65A60u, scene, 3, args);
    }
#ifdef RECOMP_X87_WINDOW
    g_fp_sp = sp;
    memcpy(g_st_win, window, sizeof window); memcpy(g_st_i64_win, integers, sizeof integers);
#endif
    void_regs_load(&saved);
    void_gfree(key);
    if (scene == touch_ui_scene()) touch_list_follow(scene, y);
    return scene == touch_ui_scene();
}

int void_touch_ui_mouse(uint32_t out[2])
{
    uint32_t scene = touch_ui_scene();
    if (!scene) return 0;
    uint32_t root = MEM32(scene + 268);
    float scale[2]; memcpy(scale, GPTR(scene + 248), sizeof scale);
    if (!touch_member(root, 60) || !isfinite(scale[0]) || !isfinite(scale[1]) ||
        scale[0] < 1.0f / 16384 || scale[1] < 1.0f / 16384 || scale[0] > 64 || scale[1] > 64) return 0;
    for (unsigned i = 0; i < 2; ++i) {
        int source = (int)MEM32(scene + 240 + 4 * i), consumed = (int)MEM32(root + 56 + 4 * i);
        if (source < 0 || source > 16384 || consumed < 0 || consumed > 1048576) return 0;
        /* The previous Notify point completed Advance before this Notify began. */
        double point = consumed / (double)scale[i];
        if (point > 16384) return 0;
        out[i] = consumed == (int)((double)source * scale[i]) ? (uint32_t)source : (uint32_t)point;
    }
    return 1;
}

int void_touch_ui_button_state(void)
{
    if (!void_machine_is_owned()) return -1;
    uint32_t scene = touch_ui_scene();
    if (!scene) return -1;
    uint32_t root = MEM32(scene + 268);
    if (!touch_member(root, 0) || !touch_member(root, 112)) return -1;
    uint32_t vtable = MEM32(root);
    if (!touch_member(vtable, 76) || MEM32(vtable + 76) != 0x10BF6330u) return -1;
    /* Original Advance/10C123C0 updates this only after delivering the edge. */
    unsigned down = MEM8(root + 112);
    return down <= 1 ? (int)down : -1;
}

static float touch_movie_number(uint32_t root, const char *path)
{
    uint32_t name = void_gstrdup(path);
    if (!name) return NAN;
    uint32_t args[] = {name};
    uint32_t text = void_call_guest_this(0x10BF6690u, root, 1, args);
    void_gfree(name);
    char value[64];
    for (unsigned i = 0; i < sizeof value; ++i) {
        if (text < 0x10000u || text > UINT32_MAX - i || !void_vm_readable(text + i, 1)) return NAN;
        value[i] = (char)MEM8(text + i);
        if (!value[i]) {
            char *end;
            float number = strtof(value, &end);
            return end != value && !*end ? number : NAN;
        }
    }
    return NAN;
}

static int touch_slider_contains(float x, float y, const float bounds[4])
{
    float width = bounds[2] - bounds[0], height = bounds[3] - bounds[1];
    return isfinite(x) && isfinite(y) && isfinite(width) && isfinite(height) &&
        width > 0 && width <= 16384 && height > 0 && height <= 16384 &&
        x >= bounds[0] && x <= bounds[2] && y >= bounds[1] && y <= bounds[3];
}

static int touch_settings_slider(void)
{
    uint32_t movie = touch_ui_scene(), root = movie ? MEM32(movie + 268) : 0;
    if (!touch_member(root, 0)) return 0;
    uint32_t table = MEM32(root);
    if (!touch_member(table, 88) || MEM32(table + 88) != 0x10BF6690u) return 0;
    void_regs saved;
    void_regs_save(&saved);
#ifdef RECOMP_X87_WINDOW
    double window[RECOMP_X87_SLOTS];
    int64_t integers[RECOMP_X87_SLOTS];
    int sp = g_fp_sp;
    memcpy(window, g_st_win, sizeof window); memcpy(integers, g_st_i64_win, sizeof integers);
#endif
    static int trace = -1;
    static unsigned traces;
    if (trace < 0) {
        const char *flag = getenv("BIOSHOCK_DUMP_PROPERTIES");
        trace = flag && *flag == '1';
    }
    char clip[64] = "BrightnessScrollBar.slider_mc", path[96];
    int valid = 1, fov = 0;
    int first = 0, last = -1, hit_row = -1;
    float row = NAN, page = NAN, frame = NAN, section = NAN, modal = NAN;
    if (!menu_brightness) {
        strcpy(clip, "PortSliderBounds");
        row = touch_movie_number(root, "selectorPos"); page = touch_movie_number(root, "PortPage");
        frame = touch_movie_number(root, "_currentframe"); section = touch_movie_number(root, "PortSectionFrame");
        modal = touch_movie_number(root, "ConfirmBox._currentframe");
        fov = page == 1 && frame == section;
        valid = isfinite(frame) && frame >= 1 && frame <= 24 && frame == floorf(frame) &&
            (fov || !(isfinite(page) && page > 0 && frame == section)) && !(isfinite(modal) && modal > 1);
        if (fov) { first = 3; last = 7; }
        else if (valid) switch ((int)frame) {
            case 7: first = 8; last = 11; break;
            case 8: first = last = 3; break;
            case 9: first = 2; last = 5; break;
            case 10: first = last = 1; break;
        }
    }
    float bounds[4] = {NAN, NAN, NAN, NAN}, x = NAN, y = NAN;
    static const char *const brightness_fields[] = {"_xmouse", "_ymouse", "_width", "_height"};
    /* Pause's invisible value-measurement bar is below its independent input hitbox. */
    static const char *const hit_fields[] = {"left", "top", "right", "bottom"};
    if (menu_brightness) {
        for (unsigned i = 0; i < 4; ++i) {
            snprintf(path, sizeof path, "%s.%s", clip, brightness_fields[i]);
            bounds[i] = touch_movie_number(root, path);
        }
        x = bounds[0]; y = bounds[1]; bounds[0] = bounds[1] = 0;
    } else if (valid) {
        x = touch_movie_number(root, "_xmouse"); y = touch_movie_number(root, "_ymouse");
        /* Original rollover scripts may lag the consumed point; geometry owns classification. */
        for (int candidate = first; candidate <= last; candidate += fov ? 4 : 1) {
            if (fov) snprintf(clip, sizeof clip, "PortFovSlider%d", candidate);
            else snprintf(clip, sizeof clip, "PortSlider%d_%d", (int)frame, candidate);
            for (unsigned i = 0; i < 4; ++i) {
                snprintf(path, sizeof path, "%s_%s", clip, hit_fields[i]);
                bounds[i] = touch_movie_number(root, path);
            }
            if (touch_slider_contains(x, y, bounds)) { hit_row = candidate; break; }
        }
    }
    float confirm[6] = {NAN, NAN, NAN, NAN, NAN, NAN};
    if (trace && traces < 32 && !menu_brightness && frame == 7) {
        static const char *const fields[] = {"ConfirmBox._xmouse", "ConfirmBox._ymouse",
            "ConfirmBox.PortButtons.YesButton._xmouse", "ConfirmBox.PortButtons.YesButton._ymouse",
            "ConfirmBox.PortButtons.YesButton._width", "ConfirmBox.PortButtons.YesButton._height"};
        for (unsigned i = 0; i < 6; ++i) confirm[i] = touch_movie_number(root, fields[i]);
    }
#ifdef RECOMP_X87_WINDOW
    g_fp_sp = sp;
    memcpy(g_st_win, window, sizeof window); memcpy(g_st_i64_win, integers, sizeof integers);
#endif
    void_regs_load(&saved);
    int drag = valid && movie == touch_ui_scene() && touch_slider_contains(x, y, bounds);
    if (trace && traces++ < 32) {
        void_log("[ui-slider] movie=%08X clip=%s row=%g hit=%d page=%g frame=%g section=%g modal=%g mouse=%g,%g bounds=%g,%g..%g,%g drag=%d",
            movie, clip, (double)row, hit_row, (double)page, (double)frame, (double)section, (double)modal,
            (double)x, (double)y, (double)bounds[0], (double)bounds[1], (double)bounds[2], (double)bounds[3], drag);
        if (!menu_brightness && frame == 7)
            void_log("[ui-modal] frame=%g local=%g,%g yes=%g,%g size=%g,%g viewport=%u,%u source=%u,%u",
                (double)modal, (double)confirm[0], (double)confirm[1], (double)confirm[2], (double)confirm[3],
                (double)confirm[4], (double)confirm[5], MEM32(movie + 104), MEM32(movie + 108), MEM32(movie + 240), MEM32(movie + 244));
    }
    return drag;
}

/* M6 (wiki note 13): a vertical drag over the original Save/Load list scrolls it. The lists are frames
 * 3 (Save), 4 and 19 (Load) of pausePC.swf; each defines _root.UpArrowPressed/DownArrowPressed, which
 * move ListYOffset by one row and clamp it in UpdateScrollingList, as the arrow buttons do. The finger
 * moves the content: up by one row pitch (34 px of a 1080-line screen) shows the next save. */
#define LIST_ROW_PITCH 0.03176f
static uint32_t list_drag_scene;   /* the movie of the claimed list gesture, else 0 */
static int list_drag_anchor = -1, list_drag_rows;

static int touch_list_screen(void)
{
    uint32_t movie = touch_ui_scene(), root = movie ? MEM32(movie + 268) : 0;
    if (!touch_member(root, 0)) return 0;
    uint32_t table = MEM32(root);
    if (!touch_member(table, 88) || MEM32(table + 88) != 0x10BF6690u) return 0;
    void_regs saved;
    void_regs_save(&saved);
#ifdef RECOMP_X87_WINDOW
    double window[RECOMP_X87_SLOTS];
    int64_t integers[RECOMP_X87_SLOTS];
    int sp = g_fp_sp;
    memcpy(window, g_st_win, sizeof window); memcpy(integers, g_st_i64_win, sizeof integers);
#endif
    float frame = touch_movie_number(root, "_currentframe");
    float modal = touch_movie_number(root, "ConfirmBox._currentframe");
#ifdef RECOMP_X87_WINDOW
    g_fp_sp = sp;
    memcpy(g_st_win, window, sizeof window); memcpy(g_st_i64_win, integers, sizeof integers);
#endif
    void_regs_load(&saved);
    return (frame == 3 || frame == 4 || frame == 19) && !(isfinite(modal) && modal > 1);
}

/* Whole rows the finger moved from the anchor (positive: up, the next saves); the anchor keeps the rest,
 * so jitter at a row boundary does not step back and forth. At most 8 rows per call. */
static int touch_list_rows(int *anchor, int y, uint32_t height)
{
    int pitch = (int)lroundf((float)height * LIST_ROW_PITCH);
    if (pitch < 8) pitch = 8;
    if (*anchor < 0) { *anchor = y; return 0; }
    int rows = (*anchor - y) / pitch;
    if (rows > 8) rows = 8;
    if (rows < -8) rows = -8;
    *anchor -= rows * pitch;
    return rows;
}

static void touch_list_follow(uint32_t scene, int y)
{
    if (!list_drag_scene || list_drag_scene != scene) return;
    int rows = touch_list_rows(&list_drag_anchor, y, MEM32(scene + 108));
    for (int i = 0; i < abs(rows) && scene == touch_ui_scene(); ++i)
        void_bioshock_movie_call(scene, rows > 0 ? "DownArrowPressed" : "UpArrowPressed", NULL, NULL);
    list_drag_rows += rows;
}

int void_touch_ui_control(void)
{
    if (menu_brightness || menu_control == (1 | 4))
        return 1 | (void_machine_is_owned() && touch_settings_slider() ? 4 : 0) |
            (!menu_brightness && void_machine_is_owned() && touch_list_screen() ? 2 : 0);
    return menu_control;
}

/* The input pump's list protocol: 64 claims the list gesture under the finger, 0 asks for the ACK,
 * INT32_MIN retires the gesture; 1 means delivered. Rows follow the finger in void_touch_ui_position. */
int void_touch_ui_scroll(int steps)
{
    if (steps == 64) {
        uint32_t scene = touch_ui_scene();
        list_drag_scene = scene;
        list_drag_anchor = scene ? (int)MEM32(scene + 244) : -1;
        list_drag_rows = 0;
    } else if (steps == INT32_MIN && list_drag_scene) {
        if (list_drag_rows) {
            static unsigned logs;
            if (logs++ < 32) void_log("[ui-list] drag scrolled %d rows", list_drag_rows);
        }
        list_drag_scene = 0;
        list_drag_anchor = -1;
    }
    return 1;
}

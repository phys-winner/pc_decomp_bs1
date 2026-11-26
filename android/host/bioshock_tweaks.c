/* Optional tweaks and fixes of the original game: the "Game and fixes" page of the port's
 * Additional settings (android/flash/port_settings.as) and the anisotropy row of its graphics page.
 *
 * physicsRate   Havok fixed step. The accumulator of 10AEF580 adds the frame time and steps the
 *               world while it holds at least the float at 11159194 (.data, 1/30 s in the shipped
 *               exe), reading it on every pass, so a new value applies on the next frame. The
 *               address is the one the "Bioshock Physics Unlocker" by TurtleHx (MIT) patches in the
 *               file (signature HkUnrealCharacterPointCollector@@ + 36). 60 Hz doubles Havok's work.
 * fixScrounger  The Scrounger tonic raises ReRollContainer_Bonus. The native
 *               UInventoryManager::CanReRoll (10CB85C0) only checks that the open container
 *               (+0x48) was never touched by a script (EverInteracted, +76 bit 0) and was rolled
 *               fewer times (+72) than the bonus; it never checks that the container is empty.
 *               ReRollContainer (10CB8680 -> UContainer 10CA95E0) re-rolls all three LootSlots and
 *               discards what they held, so a re-roll (Reload/R, the touch "Search again") at an
 *               untouched container whose items were authored in the map, such as Langford's safe
 *               in Arcadia, deletes the quest item. With the fix a container that still holds an
 *               item cannot be re-rolled; an empty one can, as the script's CanUseContainer
 *               (ShockPlayer) intends. Seam at 10CB85C0 (tools/build_android.py SEAMS).
 * cutsceneFps   At more than 60 FPS the game's cutscenes are misframed (PCGamingWiki). BioShock's
 *               scripted scenes disable the player's input with NullInput. The engine also has
 *               cinematic mode (ActionCinematicEnter/Exit, Client->Viewports[0]+352) and a
 *               separate foreground Bink player.
 * mouseRaw      The original PlayerInput.SmoothMouse (MouseSmoothingMode 1) for a physical mouse
 *               too, as "BioshockMouseSensitivity" by garamond13 does in User.ini (its
 *               MouseAccelThreshold=0 is already the default of this edition's DefUser.ini). Touch
 *               look always runs without the smoothing (bioshock_touch.c).
 * anisotropy    Sampler anisotropy of the native renderer (r10 sampler creation): 0 keeps the game's
 *               samplers, 4/8/16 makes every non-comparison linear sampler anisotropic at that
 *               level ("16x anisotropic filtering" of garamond13's BioshockGraphicalUpgrade).
 *               Samplers are created once, so the change applies after a restart. */
#include "host.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

extern int void_machine_is_owned(void);
extern int void_android_port_setting(const char *key, const char *value, char *out, size_t cap) __attribute__((weak));
extern int void_bioshock_scripted_scene(void) __attribute__((weak));

#define PHYSICS_STEP_VA 0x11159194u
#define PHYSICS_STEP_ORIGINAL 0x3D088889u   /* 1/30 s, the shipped value */

static int physics_rate = 30, fix_scrounger = 1, cutscene_cap = 1, mouse_raw;
static int player_fov = 75, weapon_fov = 60;
static int anisotropy = -1;
static int loaded, physics_disabled, cinematic;
static uint32_t physics_written = PHYSICS_STEP_ORIGINAL;

int void_bioshock_anisotropy_level(void);

static int port_int(const char *key, int fallback)
{
    char out[64];
    if (!void_android_port_setting || void_android_port_setting(key, NULL, out, sizeof out)) return fallback;
    char *end;
    double value = strtod(out, &end);
    return end != out && isfinite(value) ? (int)value : fallback;
}

static int readable(uint32_t address, uint32_t size)
{
    return address >= 0x10000u && address <= UINT32_MAX - size && void_vm_readable(address, size);
}

/* Applies one normalized value; unknown keys are ignored. */
void void_bioshock_tweak(const char *key, const char *value)
{
    int v = atoi(value);
    if (!strcmp(key, "physicsRate")) __atomic_store_n(&physics_rate, v == 60 ? 60 : 30, __ATOMIC_RELEASE);
    else if (!strcmp(key, "fixScrounger")) __atomic_store_n(&fix_scrounger, v != 0, __ATOMIC_RELEASE);
    else if (!strcmp(key, "cutsceneFps")) __atomic_store_n(&cutscene_cap, v != 0, __ATOMIC_RELEASE);
    else if (!strcmp(key, "mouseRaw")) __atomic_store_n(&mouse_raw, v != 0, __ATOMIC_RELEASE);
    else if (!strcmp(key, "playerFov") && v >= 55 && v <= 120) player_fov = v;
    else if (!strcmp(key, "weaponFov") && v >= 40 && v <= 100) weapon_fov = v;
    else return;
    void_log("[tweaks] %s=%d", key, v);
}

static void load(void)
{
    if (loaded || !void_android_port_setting) return;
    loaded = 1;
    physics_rate = port_int("physicsRate", 30) == 60 ? 60 : 30;
    fix_scrounger = port_int("fixScrounger", 1) != 0;
    cutscene_cap = port_int("cutsceneFps", 1) != 0;
    mouse_raw = port_int("mouseRaw", 0) != 0;
    player_fov = port_int("playerFov", 75);
    weapon_fov = port_int("weaponFov", 60);
    void_log("[tweaks] physics %d Hz, Scrounger fix %d, cutscene cap %d, raw mouse %d, anisotropy %d",
        physics_rate, fix_scrounger, cutscene_cap, mouse_raw, void_bioshock_anisotropy_level());
}

static void apply_physics(void)
{
    if (physics_disabled || !void_vm_writable(PHYSICS_STEP_VA, 4)) return;
    uint32_t current = MEM32(PHYSICS_STEP_VA);
    if (current != physics_written) {
        /* Something else owns the value (another edition or a patched exe): leave it alone. */
        physics_disabled = 1;
        void_log("[tweaks] physics step %08X is not the expected %08X; rate setting disabled", current, physics_written);
        return;
    }
    int rate = __atomic_load_n(&physics_rate, __ATOMIC_ACQUIRE);
    float step = 1.0f / (float)rate;
    uint32_t wanted;
    memcpy(&wanted, &step, 4);
    if (rate == 30) wanted = PHYSICS_STEP_ORIGINAL;
    if (wanted == current) return;
    MEM32(PHYSICS_STEP_VA) = wanted;
    physics_written = wanted;
    void_log("[tweaks] Havok step %08X -> %08X (%d Hz)", current, wanted, rate);
}

static int cinematic_mode(void)
{
    uint32_t engine = MEM32(0x111BE314u);
    if (!readable(engine + 68, 4)) return 0;
    uint32_t client = MEM32(engine + 68);
    if (!readable(client + 60, 8) || (int32_t)MEM32(client + 64) < 1) return 0;
    uint32_t viewports = MEM32(client + 60);
    if (!readable(viewports, 4)) return 0;
    uint32_t viewport = MEM32(viewports);
    return readable(viewport + 352, 4) && MEM32(viewport + 352) != 0;
}

/* Every input pump with the machine owned (bioshock_settings.c). */
void void_bioshock_tweaks_poll(void)
{
    if (!void_machine_is_owned()) return;
    load();
    apply_physics();
    int engine_mode = cinematic_mode();
    /* 10A74D50 publishes the fullscreen FBinkMovie, and 10A74680 clears it on stop. */
    int movie = readable(0x111C1438u, 4) && MEM32(0x111C1438u) != 0;
    int scene = void_bioshock_scripted_scene && void_bioshock_scripted_scene() > 0;
    int now = engine_mode || movie || scene;
    if (now != cinematic) void_log("[tweaks] cutscene %s (cinematic mode %d, fullscreen movie %d, scripted input locked %d)",
        now ? "began" : "ended", engine_mode, movie, scene);
    __atomic_store_n(&cinematic, now, __ATOMIC_RELEASE);
}

int void_bioshock_is_cinematic(void)
{
    return __atomic_load_n(&cinematic, __ATOMIC_ACQUIRE);
}

int void_bioshock_fov(int foreground)
{
    return foreground ? weapon_fov : player_fov;
}

/* The native frame limiter's cap (r10_present.c): 60 FPS in cutscenes, otherwise none. */
float void_bioshock_frame_cap(void)
{
    return __atomic_load_n(&cutscene_cap, __ATOMIC_ACQUIRE) && void_bioshock_is_cinematic() ? 60.0f : 0.0f;
}

/* QA seam at 109142EA, after the main loop stores GDeltaTime and before both engine ticks.
 * Keep the simulation step independent of present pacing and wall-clock measurements. */
void void_bioshock_qa_frame_delta(void)
{
    static int fixed = -1;
    if (fixed < 0) {
        const char *value = getenv("BIOSHOCK_QA_FIXED_DT");
        fixed = value && !strcmp(value, "1");
        if (fixed) void_log("[tweaks] QA simulation step fixed at 22 ms");
    }
    if (fixed) {
        const float delta = 0.022f;
        memcpy(GPTR(0x111C13C8u), &delta, sizeof delta);
    }
}

int void_bioshock_mouse_raw(void)
{
    return __atomic_load_n(&mouse_raw, __ATOMIC_ACQUIRE);
}

/* Read once: the renderer creates samplers with the level of the process start. */
int void_bioshock_anisotropy_level(void)
{
    int level = __atomic_load_n(&anisotropy, __ATOMIC_ACQUIRE);
    if (level < 0) {
        level = port_int("anisotropy", 0);
        if (level != 4 && level != 8 && level != 16) level = 0;
        __atomic_store_n(&anisotropy, level, __ATOMIC_RELEASE);
    }
    return level;
}

/* Seam at the entry of UInventoryManager::CanReRoll (10CB85C0, thiscall, no arguments): returns 1
 * after retiring the call with false when the open container still holds an item. */
int void_bioshock_reroll_blocked(void)
{
    if (!__atomic_load_n(&fix_scrounger, __ATOMIC_ACQUIRE)) return 0;
    uint32_t manager = g_ecx;
    if (!readable(manager + 0x48, 4)) return 0;
    uint32_t container = MEM32(manager + 0x48);
    if (!readable(container + 56, 12)) return 0;
    int items = 0;
    for (unsigned slot = 0; slot < 3; ++slot) {
        uint32_t loot = MEM32(container + 56 + 4 * slot);
        uint32_t stack = readable(loot + 60, 4) ? MEM32(loot + 60) : 0;   /* ULootSlot::GetItem (10CC5C20) */
        if (readable(stack + 76, 4) && (int32_t)MEM32(stack + 76) > 0) ++items;   /* UContainer::IsEmpty (10CA8D00) */
    }
    if (!items) return 0;
    static uint32_t logged;
    if (logged != container) {
        logged = container;
        void_log("[tweaks] Scrounger re-roll refused: container %08X holds %d item stack(s)", container, items);
    }
    g_eax = 0;
    g_esp += 4;
    return 1;
}

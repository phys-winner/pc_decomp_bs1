/* Register file, dispatch, CPUID, x87 helpers and fatal diagnostics. */
#define _GNU_SOURCE
#include "host.h"
#include "replacements/crash_marker.h"
#include <dlfcn.h>
#include <errno.h>
#include <signal.h>
#include <stdatomic.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>
#ifdef __ANDROID__
#include <android/log.h>
#endif

/* ------------------------------------------------------------ register file */
VOID_HIDDEN uint32_t g_eax, g_ecx, g_edx, g_esp, g_ebx, g_esi, g_edi, g_ebp;
#ifdef RECOMP_X87_WINDOW
VOID_HIDDEN double g_st_win[RECOMP_X87_SLOTS];
VOID_HIDDEN int64_t g_st_i64_win[RECOMP_X87_SLOTS];
VOID_HIDDEN int g_fp_sp = RECOMP_X87_SLOTS / 2;
#else
VOID_HIDDEN double g_st[8];
#endif
VOID_HIDDEN int g_fp_top;
VOID_HIDDEN uint16_t g_fpu_cw = 0x027F;
VOID_HIDDEN uint64_t g_fp_round_mask = ~0ull, g_fp_store_mask = ~0ull;   /* of 0x027F (fp_cw_masks) */
VOID_HIDDEN uint16_t g_seg_cs, g_seg_ds, g_seg_es, g_seg_fs, g_seg_gs, g_seg_ss;
VOID_HIDDEN uint64_t g_mm[8];
VOID_HIDDEN xmm_t g_xmm[8];
VOID_HIDDEN uint32_t g_mxcsr = 0x1F80;
VOID_HIDDEN uint32_t g_fs_base, g_gs_base, g_es_base;
VOID_HIDDEN ptrdiff_t g_mem_base = (ptrdiff_t)VOID_GUEST_BASE;
VOID_HIDDEN uint32_t g_cur_func;
VOID_HIDDEN uint32_t g_icall_trace[ICALL_TRACE_SIZE], g_icall_from[ICALL_TRACE_SIZE];
VOID_HIDDEN uint32_t g_icall_trace_idx, g_icall_count;
VOID_HIDDEN uint32_t g_flag_k, g_flag_a, g_flag_b, g_flag_cf;
VOID_HIDDEN int void_fpu_cmp = 1;
#ifndef RECOMP_X87_WINDOW
VOID_HIDDEN int64_t g_st_i64[8];
#endif
VOID_HIDDEN void (*recomp_yield_hook)(void);
VOID_HIDDEN void (*recomp_spin_hook)(void);
VOID_HIDDEN uint32_t g_backedges;
#define VOID_ENTER_RING 1024
VOID_HIDDEN uint32_t g_enter_trace[VOID_ENTER_RING];
VOID_HIDDEN uint32_t g_enter_idx;

int void_verbose;

VOID_HIDDEN uint32_t g_bioshock_menu_name, g_bioshock_menu_instance;

static char *void_bioshock_object_name(uint32_t object)
{
    if (object < 0x10000u || !void_vm_readable(object, 48)) return NULL;
    uint32_t table = MEM32(0x11297070u), count = MEM32(0x11297074u);
    uint32_t index = MEM32(object + 36), entry = 0;
    if (index < count && void_vm_readable(table + 4 * index, 4))
        entry = MEM32(table + 4 * index);
    return entry && void_vm_readable(entry + 16, 2) ? void_wstr_utf8(entry + 16) : NULL;
}

void void_bioshock_log_object(uint32_t object, const char *label)
{
    if (object < 0x10000u || !void_vm_readable(object, 48)) {
        void_log("[world-object] %s=%08X invalid", label, object);
        return;
    }
    uint32_t cls = MEM32(object + 44);
    char *name = void_bioshock_object_name(object), *class_name = void_bioshock_object_name(cls);
    void_log("[world-object] %s=%08X name=%s(%u) class=%08X(%s) outer=%08X vt=%08X",
        label, object, name ? name : "?", MEM32(object + 36), cls,
        class_name ? class_name : "?", MEM32(object + 24), MEM32(object));
    free(class_name);
    free(name);
}

void void_bioshock_log_package_objects(uint32_t package)
{
    uint32_t total = 0, visited = 0;
    for (uint32_t bucket = 0; bucket < 4096; ++bucket) {
        uint32_t object = MEM32(0x11287E70u + 4 * bucket);
        while (object >= 0x10000u && void_vm_readable(object, 48) && visited++ < 65536) {
            if (MEM32(object + 36) == 30277u || MEM32(object + 44) == 0x01120600u)
                void_bioshock_log_object(object, "world-candidate");
            if (MEM32(object + 24) == package) {
                if (total++ < 96) void_bioshock_log_object(object, "package-child");
            }
            object = MEM32(object + 8);
        }
    }
    void_log("[world-package-scan] package=%08X children=%u visited=%u", package, total, visited);
    uint32_t linkers = MEM32(0x1128BF34u), linker_count = MEM32(0x1128BF38u);
    for (uint32_t i = 0; i < linker_count && i < 1024; ++i) {
        uint32_t linker = MEM32(linkers + 4 * i);
        if (!void_vm_readable(linker, 0x138) || MEM32(linker + 56) != package) continue;
        uint32_t exports = MEM32(linker + 0x50), table = MEM32(linker + 0x94), loaded = 0;
        for (uint32_t e = 0; e < exports && e < 4096; ++e) {
            if (!void_vm_readable(table + 52 * e + 40, 4)) break;
            uint32_t object = MEM32(table + 52 * e + 40);
            if (object < 0x10000u || !void_vm_readable(object, 48)) continue;
            if (loaded++ < 16) void_bioshock_log_object(object, "linker-export");
        }
        void_log("[world-linker] package=%08X linker=%08X exports=%u objects=%u mask=%08X/%08X load=%08X package_flags=%08X",
            package, linker, exports, loaded, MEM32(linker + 0xB0), MEM32(linker + 0xB4),
            MEM32(linker + 0x130), MEM32(linker + 0x44));
    }
}

void void_bioshock_mark_menu(uint32_t name, uint32_t instance)
{
    g_bioshock_menu_name = name;
    g_bioshock_menu_instance = instance;
    void_log("[menu] registered LaunchMainMenu name=%u+%u", name, instance);
}

static void void_bioshock_dump_functions(void)
{
    uint32_t count = MEM32(0x1128BF20), array = MEM32(0x1128BF1C), shown = 0;
    void_log("[menu] function registry count=%u array=%08X", count, array);
    for (uint32_t i = 0; i < count && i < 200000 && shown < 80; ++i) {
        if (!void_vm_readable(array + 4 * i, 4)) break;
        uint32_t object = MEM32(array + 4 * i);
        if (object < 0x10000u || !void_vm_readable(object, 0x30)) continue;
        uint32_t name = MEM32(object + 0x24);
        if (name >= 65536u) continue;
        uint32_t entry = MEM32(MEM32(0x11297070) + 4 * name);
        if (!entry || !void_vm_readable(entry + 16, 32)) continue;
        char *text = void_wstr_utf8(entry + 16);
        if (strstr(text, "Movie") || strstr(text, "Menu") || strstr(text, "Pause")) {
            void_log("[function] %08X name=%s vt=%08X native=%u func=%08X script=%u",
                object, text, MEM32(object), MEM16(object + 0xAC), MEM32(object + 0xB8), MEM32(object + 0x58));
            ++shown;
        }
        free(text);
    }
}

static void void_bioshock_trace_script(uint32_t function)
{
    static uint32_t seen[96], count;
    if (function < 0x10000u || !void_vm_readable(function, 0x30)) return;
    for (uint32_t i = 0; i < count; ++i) if (seen[i] == function) return;
    uint32_t name = MEM32(function + 0x24);
    if (name >= 65536u) return;
    uint32_t entry = MEM32(MEM32(0x11297070) + 4 * name);
    if (!entry || !void_vm_readable(entry + 16, 32)) return;
    char *text = void_wstr_utf8(entry + 16);
    if (count < sizeof seen / sizeof seen[0]) {
        seen[count] = function;
        void_log("[script] %u function=%08X name=%s owner=%08X", count++, function, text, MEM32(function + 0x2C));
    }
    if (!strcmp(text, "EnteredMainMenu")) {
        uint32_t self = g_ecx, cls = self >= 0x10000u && void_vm_readable(self, 0x40) ? MEM32(self + 0x2C) : 0;
        uint32_t cname = cls && void_vm_readable(cls, 0x30) ? MEM32(cls + 0x24) : 0xFFFFFFFFu;
        uint32_t cent = cname < 65536u ? MEM32(MEM32(0x11297070) + 4 * cname) : 0;
        char *ctext = cent && void_vm_readable(cent + 16, 32) ? void_wstr_utf8(cent + 16) : NULL;
        void_log("[menu] EnteredMainMenu this=%08X vt=%08X class=%08X class_name=%s native=%u script=%u flags=%08X fields=%08X,%08X,%08X,%08X,%08X",
            self, MEM32(self), cls, ctext ? ctext : "?", MEM16(function + 0xAC), MEM32(function + 0x58), MEM32(function + 0xA8),
            MEM32(self + 0x3C), MEM32(self + 0x40), MEM32(self + 0x48), MEM32(self + 0x4C), MEM32(self + 0x50));
        free(ctext);
        void_bioshock_dump_functions();
    }
    free(text);
}

void void_bioshock_trace_menu(uint32_t function)
{
    void_bioshock_trace_script(function);
    if (!g_bioshock_menu_name || function < 0x10000u || !void_vm_readable(function, 0x30)) return;
    if (MEM32(function + 0x24) != g_bioshock_menu_name || MEM32(function + 0x28) != g_bioshock_menu_instance) return;
    uint32_t entry = MEM32(MEM32(0x11297070) + 4 * g_bioshock_menu_name);
    if (!void_vm_readable(entry + 16, 32)) return;
    char *text = void_wstr_utf8(entry + 16);
    void_log("[menu] LaunchMainMenu invoked function=%08X name=%s", function, text);
    free(text);
}

static void bioshock_console_command(const char *requested);
int void_bioshock_take_navigation(void);

/* The indices match the in-game cheats dialog. All these commands run synchronously. */
static const char *const bioshock_cheats[] = {
    "God", "GiveHealth", "GiveBioAmmo", "IGBigBucks", "GiveAll", "GiveAllPlasmids", "Ghost", "Walk",
};
static atomic_int bioshock_cheat_pending;

int void_bioshock_request_cheat(int index)
{
    if (index < 0 || (size_t)index >= sizeof bioshock_cheats / sizeof *bioshock_cheats) return 0;
    int idle = 0;
    return atomic_compare_exchange_strong(&bioshock_cheat_pending, &idle, index + 1);
}

static void void_bioshock_run_navigation(uint32_t wparam)
{
    int cheat = atomic_exchange(&bioshock_cheat_pending, 0);
    if (!cheat && !void_bioshock_take_navigation()) return;
    void_regs saved;
    void_regs_save(&saved);
    void_log("[navigation] executing on guest thread %u wparam=%08X", void_thread_id(), wparam);
    bioshock_console_command(cheat ? bioshock_cheats[cheat - 1] : NULL);
    void_regs_load(&saved);
}

void void_bioshock_poll_main_navigation(void)
{
    if (void_thread_id() == 260) void_bioshock_run_navigation(0);
}

static void void_bioshock_tick_entry(void)
{
    static unsigned calls;
    uint32_t player = g_ecx;
    if (calls >= 40 && calls % 120) { ++calls; return; }
    uint32_t view = player >= 0x10000u && void_vm_readable(player + 268, 4) ? MEM32(player + 268) : 0;
    uint32_t count = player >= 0x10000u && void_vm_readable(player + 284, 4) ? MEM32(player + 284) : 0;
    uint32_t movies = count && void_vm_readable(player + 280, 4) ? MEM32(player + 280) : 0;
    uint32_t movie = movies && void_vm_readable(movies, 4) ? MEM32(movies) : 0;
    uint32_t root = movie >= 0x10000u && void_vm_readable(movie + 268, 4) ? MEM32(movie + 268) : 0;
    uint32_t inner = root >= 0x10000u && void_vm_readable(root + 12, 4) ? MEM32(root + 12) : 0;
    uint32_t inner_vt = inner >= 0x10000u && void_vm_readable(inner, 4) ? MEM32(inner) : 0;
    uint32_t advance = inner_vt >= 0x10000u && void_vm_readable(inner_vt + 16, 4) ? MEM32(inner_vt + 16) : 0;
    uint32_t frame = inner >= 0x10000u && void_vm_readable(inner + 164, 8) ? MEM32(inner + 164) : 0;
    uint32_t remainder = inner >= 0x10000u && void_vm_readable(inner + 168, 4) ? MEM32(inner + 168) : 0;
    uint32_t environment = inner >= 0x10000u && void_vm_readable(inner + 24, 4) ? MEM32(inner + 24) : 0;
    uint32_t playing = inner >= 0x10000u && void_vm_readable(inner + 110, 1) ? MEM8(inner + 110) : 0;
    uint32_t gate0 = player >= 0x10000u && void_vm_readable(player + 60, 4) ? MEM32(player + 60) : 0;
    uint32_t gate1 = gate0 >= 0x10000u && void_vm_readable(gate0 + 64, 4) ? MEM32(gate0 + 64) : 0;
    uint32_t gate2 = gate1 >= 0x10000u && void_vm_readable(gate1 + 224, 4) ? MEM32(gate1 + 224) : 0;
    uint32_t gate3 = gate2 >= 0x10000u && void_vm_readable(gate2 + 228, 4) ? MEM32(gate2 + 228) : 0;
    uint32_t gate4 = gate3 >= 0x10000u && void_vm_readable(gate3 + 84, 4) ? MEM32(gate3 + 84) : 0;
    uint32_t gate4vt = gate4 >= 0x10000u && void_vm_readable(gate4, 4) ? MEM32(gate4) : 0;
    uint32_t gate3vt = gate3 >= 0x10000u && void_vm_readable(gate3, 4) ? MEM32(gate3) : 0;
    uint32_t platform = MEM32(0x111BE308);
    void_log("[flash-tick-gate] chain=%08X/%08X/%08X/%08X/%08X methods=%08X/%08X counters=%u/%u platform=%08X flag4=%u",
        gate0, gate1, gate2, gate3, gate4,
        gate4vt >= 0x10000u && void_vm_readable(gate4vt + 332, 4) ? MEM32(gate4vt + 332) : 0,
        gate3vt >= 0x10000u && void_vm_readable(gate3vt + 380, 4) ? MEM32(gate3vt + 380) : 0,
        gate4 >= 0x10000u && void_vm_readable(gate4 + 272, 8) ? MEM32(gate4 + 272) : 0,
        gate4 >= 0x10000u && void_vm_readable(gate4 + 272, 8) ? MEM32(gate4 + 276) : 0,
        platform, platform >= 0x10000u && void_vm_readable(platform + 4, 4) ? MEM32(platform + 4) : 0);
    void_log("[flash-tick-entry] n=%u player=%08X flags68=%02X count=%u movie=%08X view=%08X play117=%u root=%08X root_vt=%08X time=%08X inner=%08X advance=%08X frame=%u/%u remainder=%08X environment=%08X",
        calls++, player, player >= 0x10000u && void_vm_readable(player + 68, 1) ? MEM8(player + 68) : 0,
        count, movie, view, root >= 0x10000u && void_vm_readable(root + 117, 1) ? MEM8(root + 117) : 0,
        root, root >= 0x10000u && void_vm_readable(root, 4) ? MEM32(root) : 0,
        root >= 0x10000u && void_vm_readable(root + 40, 4) ? MEM32(root + 40) : 0, inner, advance,
        frame, playing, remainder, environment);
}

static int save_trace_active;

void void_bioshock_save_entry(uint32_t va)
{
    if (va == 0x10CE2670u || va == 0x10E4A8D1u || va == 0x10E58014u ||
        va == 0x10E4AC55u || va == 0x10E4AA9Fu ||
        va == 0x10E57FAEu || va == 0x10E57FB6u) {
        void_log("[fmod-step] fn=%08X result=%08X", va, g_eax);
        return;
    }
    if (va == 0x10E4AC2Bu) {
        if (g_cur_func == va)
            void_log("[fmod-init-enter] system=%08X channels=%u flags=%08X extra=%08X",
                MEM32(g_esp + 4), MEM32(g_esp + 8), MEM32(g_esp + 12), MEM32(g_esp + 16));
        else
            void_log("[fmod-init-return] caller=%08X result=%08X", g_cur_func, g_eax);
        return;
    }
    if (va == 0x10CE7F35u || va == 0x10CE8008u || va == 0x10CE802Au) {
        void_log("[fmod-config] stage=%08X this=%08X result=%08X", va, g_esi, g_eax);
        return;
    }
    if (va == 0x10CE8069u) {
        void_log("[fmod-subsystem-init] failed result=%08X", g_eax);
        extern VOID_HIDDEN uint32_t g_enter_trace[1024], g_enter_idx;
        for (unsigned i = 0; i < 256; ++i)
            void_log("[fmod-ring] %03u %08X", i, g_enter_trace[(g_enter_idx - 256 + i) & 1023]);
        return;
    }
    if (va == 0x10E974BFu) {
        void_log("[openal-stage] this=%08X drivers=%u device=%08X context=%08X",
            g_esi, MEM32(g_esi + 0x334), MEM32(g_esi + 0x32C), MEM32(g_esi + 0x330));
        return;
    }
    if (va == 0x10EB818Cu || va == 0x10EB81ABu || va == 0x10E96071u ||
        va == 0x10E96078u || va == 0x10E962C3u || va == 0x10E974A9u) {
        void_log("[openal-return] stage=%08X eax=%08X al=%02X edi=%08X", va, g_eax, LO8(g_eax), g_edi);
        return;
    }
    if (va == 0x10E963A0u || va == 0x10E96445u || va == 0x10E96487u) {
        void_log("[openal-caps] stage=%08X this=%08X device=%08X context=%08X eax=%08X",
            va, g_esi, g_edi, g_ebp, g_eax);
        return;
    }
    if (va == 0x1090A800u && !MEM32(g_esp + 16)) {
        uint32_t name = MEM32(g_esp + 12);
        char *text = name && void_vm_readable(name, 2) ? void_wstr_utf8(name) : NULL;
        void_log("[package-load] a1=%08X a2=%08X name=%s", MEM32(g_esp + 4), MEM32(g_esp + 8), text ? text : "");
        free(text);
        return;
    }
#ifdef RECOMP_TRACE
    if (va == 0x1094D7B0u && !MEM32(g_esp + 8) && !MEM32(g_esp + 16)) {
        uint32_t object = MEM32(g_esp + 4);
        uint32_t index = object && void_vm_readable(object + 36, 4) ? MEM32(object + 36) : 0;
        uint32_t entry = index ? MEM32(MEM32(0x11297070) + 4 * index) : 0;
        char *name = entry && void_vm_readable(entry + 16, 2) ? void_wstr_utf8(entry + 16) : NULL;
        uint32_t requested = MEM32(g_esp + 12);
        char *path = requested && void_vm_readable(requested, 2) ? void_wstr_utf8(requested) : NULL;
        void_log("[package-caller] a1=%08X a2=%08X name=%s requested=%s", object, MEM32(g_esp + 8), name ? name : "", path ? path : "");
        free(path);
        free(name);
        return;
    }
#endif
#ifdef RECOMP_TRACE
    if (va == 0x1094D1C0u) {
        uint32_t name = MEM32(g_esp + 8);
        char *text = name && void_vm_readable(name, 2) ? void_wstr_utf8(name) : NULL;
        void_log("[package-resolve] package=%08X path=%s", MEM32(g_esp + 4), text ? text : "");
        free(text);
        if (!name) {
            extern VOID_HIDDEN uint32_t g_enter_trace[1024], g_enter_idx;
            for (unsigned i = 0; i < 1024; ++i) {
                if (g_enter_trace[(g_enter_idx - 1 - i) & 1023] != 0x1094D7B0u) continue;
                for (unsigned j = 0; j < 16; ++j)
                    void_log("[package-stack] %08X", g_enter_trace[(g_enter_idx - 1 - i - j) & 1023]);
                break;
            }
        }
        return;
    }
#endif
    if (va == 0x10952E00u || va == 0x10978D10u || va == 0x10EC9E50u || va == 0x10953100u) {
        static unsigned calls;
        if (calls++ < 64) {
            uint32_t path = MEM32(g_esp + 4);
            char *text = path >= 0x10000u && void_vm_readable(path, 2) ? void_wstr_utf8(path) : NULL;
            void_log("[stream-init] fn=%08X this=%08X path=%s key=%08X chunk=%08X start=%08X end=%08X",
                va, g_ecx, text ? text : "?", va == 0x10EC9E50u ? 0 : MEM32(g_esp + 12),
                MEM32(g_esp + 20), MEM32(g_esp + 24), MEM32(g_esp + 28));
            free(text);
        }
        return;
    }
    if (va == 0x109529F0u) {
        static unsigned calls;
        if (save_trace_active && calls++ < 32) void_log("[stream-compare] node=%08X stored14=%08X key=%08X stored2C=%08X start=%08X stored30=%08X end=%08X",
            g_ecx, MEM32(g_ecx + 20), MEM32(g_esp + 12), MEM32(g_ecx + 44), MEM32(g_esp + 20), MEM32(g_ecx + 48), MEM32(g_esp + 24));
        return;
    }
    if (va == 0x10952AB0u) {
        static unsigned calls;
        if (save_trace_active && calls++ < 8) {
            uint32_t level = g_ecx, path = MEM32(g_esp + 4), count = MEM32(level + 20);
            char *text = path >= 0x10000u && void_vm_readable(path, 2) ? void_wstr_utf8(path) : NULL;
            void_log("[stream-request] level=%08X path=%s arg4=%08X arg8=%08X argC=%08X arg10=%08X arg14=%08X count=%u",
                level, text ? text : "?", MEM32(g_esp + 8), MEM32(g_esp + 12), MEM32(g_esp + 16),
                MEM32(g_esp + 20), MEM32(g_esp + 24), count);
            free(text);
            if (count && count < 64) {
                uint32_t node = MEM32(MEM32(level + 16));
                if (void_vm_readable(node, 52)) void_log("[stream-candidate] node=%08X name4=%08X nameC=%08X alt8=%08X values14/2C/30=%08X/%08X/%08X",
                    node, MEM32(node + 4), MEM32(node + 12), MEM32(node + 8), MEM32(node + 20), MEM32(node + 44), MEM32(node + 48));
            }
        }
        return;
    }
    if (va == 0x10C65F30u) {
        static unsigned calls;
        if (calls++ < 64) void_log("[save-thunk-entry] this=%08X archive=%08X", g_ecx, MEM32(g_esp + 4));
        return;
    }
    if (va == 0x10C61CD0u) {
        static unsigned calls;
        uint32_t source = MEM32(g_esp + 4);
        char *text = source >= 0x10000u && void_vm_readable(source, 2) ? void_wstr_utf8(source) : NULL;
        if (calls++ < 96) void_log("[save-event-entry] this=%08X event=%s args=%08X", g_ecx, text ? text : "?", MEM32(g_esp + 12));
        free(text);
        return;
    }
    if (va == 0x10ACDD00u) save_trace_active = 1;
    if (va == 0x10ACA050u || va == 0x10AC6AF0u || va == 0x10ACB4D0u || va == 0x10AC9850u ||
        va == 0x109ABE00u || va == 0x10912BF0u || va == 0x10913760u) {
        static unsigned stage_calls;
        if (save_trace_active && stage_calls++ < 192) void_log("[save-stage-entry] fn=%08X this=%08X arg0=%08X arg1=%08X globals=%08X/%08X/%08X",
            va, g_ecx, MEM32(g_esp + 4), MEM32(g_esp + 8), MEM32(0x112BD7C8), MEM32(0x1128F028), MEM32(0x112A411C));
        return;
    }
    static unsigned calls;
    uint32_t offset = va == 0x1094E090u ? 20 : va == 0x10AC8510u ? 16 : 8;
    uint32_t source = MEM32(g_esp + offset);
    char *text = source >= 0x10000u && void_vm_readable(source, 2) ? void_wstr_utf8(source) : NULL;
    if (calls++ < 96) void_log("[save-direct-entry] fn=%08X source=%s", va, text ? text : "?");
    free(text);
}

int bioshock_sample_watch;

void void_bioshock_audio_return(uint32_t caller, uint32_t value)
{
    static unsigned calls;
    if (caller == 0x109EAB20u && calls++ < 32)
        void_log("[audio-load-return] value=%08X", value);
    if (caller == 0x10CE7D30u) {
        static unsigned nested;
        if (nested++ < 128) void_log("[audio-nested] value=%08X", value);
    }
    if (caller == 0x109EAB20u && value >= 0x02000000u && value < 0x10000000u && void_vm_readable(value, 4)) {
        uint32_t vt = MEM32(value);
        if (void_vm_readable(vt + 0xE4, 4))
            void_log("[audio-object] object=%08X vt=%08X init=%08X", value, vt, MEM32(vt + 0xE4));
    }
}

VOID_HIDDEN uint32_t bioshock_flash_trace;

void void_bioshock_debug_flash_method(uint32_t name, uint32_t target)
{
    static unsigned calls;
    if (!bioshock_flash_trace || calls++ >= 128) return;
    char *text = NULL;
    if (name >= 0x10000u && void_vm_readable(name, 16)) {
        uint32_t address = MEM8(name) == 0xFFu ? MEM32(name + 12) : name + 1;
        if (address >= 0x10000u && void_vm_readable(address, 2)) text = void_astr_utf8(address);
    }
    if (name == UINT32_MAX) void_log("[flash-getvariable] name=%s", text ? text : "?");
    else void_log("[flash-callmethod] name=%s target=%08X", text ? text : "?", target);
    free(text);
}

void void_bioshock_debug_flash_opcode(uint32_t pc, uint32_t opcode, uint32_t code)
{
    static unsigned calls;
    if (!bioshock_flash_trace || calls++ >= 384) return;
    uint32_t data = code >= 0x10000u && void_vm_readable(code + pc, 8) ? MEM32(code + pc) : 0;
    void_log("[flash-op] pc=%08X op=%02X data=%08X code=%08X", pc, opcode, data, code);
    if (code >= 0x10000u && (opcode == 0x81u || opcode == 0x83u || opcode == 0x9Bu) && void_vm_readable(code + pc + 3, 2)) {
        if (opcode == 0x81u) void_log("[flash-gotoframe] frame=%u", MEM16(code + pc + 3));
        else {
            char *text = void_astr_utf8(code + pc + 3);
            if (text) {
                void_log(opcode == 0x83u ? "[flash-geturl] %s" : "[flash-function] %s", text);
                free(text);
            }
        }
    }
}

void void_bioshock_debug_flash_value(uint32_t address)
{
    static unsigned calls;
    if (!bioshock_flash_trace || calls++ >= 128 || !void_vm_readable(address, 28)) return;
    double number;
    memcpy(&number, (const void *)ADDR(address + 0x14), sizeof number);
    void_log("[flash-value] type=%u raw=%08X number=%.9g", MEM32(address), MEM32(address + 0x14), number);
}

void void_bioshock_debug_entry(uint32_t va)
{
    static unsigned action_logs[3];
    if (va == 0x10967870u && MEM32(g_esp + 4) == 0x01F8CE80u)
        void_log("[navigation] process gates=%08X/%08X function_flags=%08X native=%u params=%u return=%u",
            MEM32(0x111C0340u), MEM32(0x111C0344u), MEM32(0x01F8CF28u),
            MEM16(0x01F8CF2Cu), MEM16(0x01F8CF32u), MEM16(0x01F8CF34u));
    if (va == MEM32(0x01F8CF38u))
        void_log("[navigation] native console thunk on guest thread %u", void_thread_id());
    if (va == 0x10C372F0u)
        void_log("[navigation] console dispatch on guest thread %u", void_thread_id());
    if (va == 0x10AD1490u) {
        uint32_t source = MEM32(g_esp + 4);
        char *text = source >= 0x10000u && void_vm_readable(source, 2) ? void_wstr_utf8(source) : NULL;
        void_log("[navigation] engine exec on guest thread %u: %s", void_thread_id(), text ? text : "?");
        free(text);
    }
    if (va == 0x10A73C70u) { void_bioshock_tick_entry(); return; }
    if (va == 0x10AC6C50u) {
        static unsigned calls;
        if (calls++ < 8) void_log("[flash-engine-gate-entry] this=%08X counters=%08X/%08X",
            g_ecx, MEM32(g_ecx + 272), MEM32(g_ecx + 276));
        return;
    }
    if (va == 0x10B10B50u) {
        static unsigned calls;
        uint32_t platform = MEM32(0x111BE308);
        if (calls++ < 8) void_log("[flash-level-gate-entry] this=%08X platform=%08X flag4=%u",
            g_ecx, platform, void_vm_readable(platform + 4, 4) ? MEM32(platform + 4) : 0);
        return;
    }
    if (va == 0x10BF6330u) {
        static uint32_t last[3] = { UINT32_MAX, UINT32_MAX, UINT32_MAX };
        static unsigned calls;
        uint32_t mouse[3] = { MEM32(g_esp + 4), MEM32(g_esp + 8), MEM32(g_esp + 12) };
        unsigned n = calls++;
        if (!(n % 120) || memcmp(mouse, last, sizeof mouse)) {
            memcpy(last, mouse, sizeof mouse);
            void_log("[flash-notify-entry] n=%u root=%08X x=%d y=%d buttons=%u", n, g_ecx,
                (int)mouse[0], (int)mouse[1], mouse[2]);
        }
        return;
    }
    if (va == 0x10C12BD0u) {
        static unsigned calls;
        if (MEM8(g_esp + 4) == 1) {
            bioshock_flash_trace = 1;
            memset(action_logs, 0, sizeof action_logs);
        }
        if (calls++ < 64) void_log("[flash-button-entry] this=%08X event=%u", g_ecx, MEM8(g_esp + 4));
        return;
    }
    if (va == 0x10BFF7C0u) {
        if (bioshock_flash_trace && action_logs[0]++ < 64)
            void_log("[flash-action-queue] view=%08X action=%08X count=%u", g_ecx, MEM32(g_esp + 4), MEM32(g_ecx + 0x98));
        return;
    }
    if (va == 0x10BFF2F0u) {
        if (bioshock_flash_trace && g_ecx >= 0x10000u && void_vm_readable(g_ecx + 0x94, 12) && MEM32(g_ecx + 0x98) && action_logs[1]++ < 64)
            void_log("[flash-action-drain] view=%08X actions=%08X count=%u", g_ecx, MEM32(g_ecx + 0x94), MEM32(g_ecx + 0x98));
        return;
    }
    if (va == 0x10C0DE60u) {
        if (bioshock_flash_trace && action_logs[2]++ < 64)
            void_log("[flash-action-execute] action=%08X receiver=%08X header=%08X", g_ecx, MEM32(g_esp + 4), MEM32(g_ecx));
        return;
    }
    if (va == 0x10E868C3u || va == 0x10EA6529u) {
        static unsigned calls[2];
        unsigned slot = va == 0x10EA6529u;
        if (calls[slot]++ < 32) {
            if (!slot) {
                uint32_t format = MEM32(g_ecx + 0x138), parent = MEM32(g_ecx + 0x1C);
                void_log("[wav-read] this=%08X format=%04X channels=%u block=%u samples=%u source_bytes=%u type=%u out=%08X/%08X",
                    g_ecx, MEM16(format), MEM16(format + 2), MEM16(format + 12), MEM16(format + 18),
                    MEM32(g_ecx + 0xAC), MEM32(parent + 0x100), MEM32(g_esp + 4), MEM32(g_esp + 8));
            } else {
                uint32_t source = MEM32(g_esp + 4);
                void_log("[ima-decode] source=%08X header=%08X/%08X output=%08X blocks=%u stride=%u samples=%u channels=%u",
                    source, MEM32(source), MEM32(source + 4), MEM32(g_esp + 8), MEM32(g_esp + 12),
                    MEM32(g_esp + 16), MEM32(g_esp + 20), MEM32(g_esp + 24));
            }
        }
        return;
    }
    if (va == 0x10A655F0u) {
        static unsigned calls;
        if (calls++ < 16)
            void_log("[flash-can-tick-entry] movie=%08X owner=%08X", g_ecx,
                void_vm_readable(g_ecx + 268, 4) ? MEM32(g_ecx + 268) : 0);
        return;
    }
    if (va == 0x10A65DA0u) {
        static uint32_t last[4] = { UINT32_MAX, UINT32_MAX, UINT32_MAX, UINT32_MAX };
        static unsigned calls;
        uint32_t state[4] = { MEM32(g_ecx + 240), MEM32(g_ecx + 244), MEM32(g_ecx + 256), MEM32(g_esp + 8) };
        unsigned n = calls++;
        if (n < 16 || !(n % 120) || memcmp(state, last, sizeof state)) {
            memcpy(last, state, sizeof state);
            void_log("[flash-scene-tick-entry] n=%u this=%08X x=%d y=%d buttons=%u notify=%u root=%08X scale=%08X/%08X", n,
                g_ecx, (int)state[0], (int)state[1], state[2], state[3], MEM32(g_ecx + 268),
                MEM32(g_ecx + 248), MEM32(g_ecx + 252));
        }
        return;
    }
    if (va == 0x10C0F2B0u) {
        uint32_t format = MEM32(g_esp + 4);
        char *message = format >= 0x10000u && void_vm_readable(format, 2) ? void_astr_utf8(format) : NULL;
        static unsigned script_messages;
        if (message && bioshock_flash_trace && script_messages++ < 64)
            void_log("[flash-script-message] caller=%08X text=%s", MEM32(g_esp), message);
        if (message && strstr(message, "import error")) {
            uint32_t resource = MEM32(g_esp + 8), movie = MEM32(g_esp + 12);
            char *resource_text = resource >= 0x10000u && void_vm_readable(resource, 2) ? void_astr_utf8(resource) : NULL;
            char *movie_text = movie >= 0x10000u && void_vm_readable(movie, 2) ? void_astr_utf8(movie) : NULL;
            void_log("[flash-import-error] caller=%08X resource=%s movie=%s format=%s", MEM32(g_esp),
                resource_text ? resource_text : "?", movie_text ? movie_text : "?", message);
            free(resource_text);
            free(movie_text);
        }
        free(message);
        return;
    }
    static unsigned imports;
    if (va != 0x10BFA7A0u || imports >= 80) return;
    ++imports;
    uint32_t resource = MEM32(g_esp + 4);
    char *text = resource >= 0x10000u && void_vm_readable(resource, 2) ? void_astr_utf8(resource) : NULL;
    void_log("[flash-import] this=%08X resource=%s exports=%d out=%08X", g_ecx, text ? text : "?",
        g_ecx >= 0x10000u && void_vm_readable(g_ecx + 60, 4) ? (int)MEM32(g_ecx + 60) : -1, MEM32(g_esp + 8));
    free(text);
}

void void_regs_save(void_regs *r)
{
    r->eax = g_eax; r->ecx = g_ecx; r->edx = g_edx; r->ebx = g_ebx;
    r->esp = g_esp; r->ebp = g_ebp; r->esi = g_esi; r->edi = g_edi;
    r->fs = g_fs_base; r->cur = g_cur_func;
    r->flag_k = g_flag_k; r->flag_a = g_flag_a; r->flag_b = g_flag_b; r->flag_cf = g_flag_cf;
    memcpy(r->st, g_st, sizeof g_st);
    memcpy(r->st_i64, g_st_i64, sizeof g_st_i64);
    r->fp_top = g_fp_top; r->fpu_cmp = void_fpu_cmp; r->fpu_cw = g_fpu_cw;
    r->fp_round_mask = g_fp_round_mask; r->fp_store_mask = g_fp_store_mask;
    memcpy(r->mm, g_mm, sizeof g_mm);
    memcpy(r->xmm, g_xmm, sizeof g_xmm);
    r->mxcsr = g_mxcsr;
    r->eh_frames = g_eh_frames; r->eh_obj = g_eh_obj; r->eh_info = g_eh_info;
    r->backedges = g_backedges;
}

void void_regs_load(const void_regs *r)
{
    g_eax = r->eax; g_ecx = r->ecx; g_edx = r->edx; g_ebx = r->ebx;
    g_esp = r->esp; g_ebp = r->ebp; g_esi = r->esi; g_edi = r->edi;
    g_fs_base = r->fs; g_cur_func = r->cur;
    g_flag_k = r->flag_k; g_flag_a = r->flag_a; g_flag_b = r->flag_b; g_flag_cf = r->flag_cf;
    memcpy(g_st, r->st, sizeof g_st);
    memcpy(g_st_i64, r->st_i64, sizeof g_st_i64);
    g_fp_top = r->fp_top; void_fpu_cmp = r->fpu_cmp; g_fpu_cw = r->fpu_cw;
    g_fp_round_mask = r->fp_round_mask; g_fp_store_mask = r->fp_store_mask;
    memcpy(g_mm, r->mm, sizeof g_mm);
    memcpy(g_xmm, r->xmm, sizeof g_xmm);
    g_mxcsr = r->mxcsr;
    g_eh_frames = (recomp_ehframe_t *)r->eh_frames; g_eh_obj = r->eh_obj; g_eh_info = r->eh_info;
    g_backedges = r->backedges;
}

/* --------------------------------------------------------------- logging */
static pthread_mutex_t log_lock = PTHREAD_MUTEX_INITIALIZER;
static FILE *log_file;
/* A session's log file stops growing at LOG_FILE_LIMIT bytes; logcat keeps every line (wiki note 13, L3a).
 * The main menu's Bink background logs every frame (about 9 MB an hour), and SessionLog rotates the file
 * only at a start. A fatal error or signal dump (up to LOG_CRASH_RESERVE more) is still written. */
#define LOG_FILE_LIMIT (48u << 20)
#define LOG_CRASH_RESERVE (4u << 20)
static uint64_t log_file_bytes;
static int log_file_capped;
static volatile int log_crash_mode;

/* The session log is on the shared storage (FUSE): a write there usually takes a fraction of a millisecond
 * but can stall for 100+ ms while the media provider is busy, and void_log runs on guest threads that hold the
 * machine (a 172 ms hold inside WaitForSingleObject's log line on the POCO, 2026-11-25). Lines now go into a
 * ring that a writer thread empties every 20 ms; logcat still gets each line at once. A fatal error writes
 * through (crash mode), void_log_flush drains, and a host signal writes what is still queued before its own
 * line. A full ring is written by the producer itself. */
#define LOG_RING_SIZE (1u << 20)
static char log_ring[LOG_RING_SIZE];
static uint64_t log_ring_head;        /* produced; log_lock */
static uint64_t log_ring_tail;        /* written; log_drain_lock (atomic reads elsewhere) */
static pthread_mutex_t log_drain_lock = PTHREAD_MUTEX_INITIALIZER;
static int log_fd = -1;
static pthread_once_t log_writer_once = PTHREAD_ONCE_INIT;

static void log_write_all(int fd, const char *data, size_t length)
{
    while (length) {
        ssize_t n = write(fd, data, length);
        if (n < 0) { if (errno == EINTR) continue; return; }
        data += n;
        length -= (size_t)n;
    }
}

/* Writes every queued byte; log_drain_lock held. */
static void log_drain_locked(void)
{
    uint64_t head = __atomic_load_n(&log_ring_head, __ATOMIC_ACQUIRE), tail = log_ring_tail;
    int fd = __atomic_load_n(&log_fd, __ATOMIC_ACQUIRE);
    while (tail < head) {
        size_t at = (size_t)(tail % LOG_RING_SIZE), n = (size_t)(head - tail);
        if (n > LOG_RING_SIZE - at) n = LOG_RING_SIZE - at;
        if (fd >= 0) log_write_all(fd, log_ring + at, n);
        tail += n;
    }
    __atomic_store_n(&log_ring_tail, tail, __ATOMIC_RELEASE);
}

static void log_drain(void)
{
    pthread_mutex_lock(&log_drain_lock);
    log_drain_locked();
    pthread_mutex_unlock(&log_drain_lock);
}

static void *log_writer(void *unused)
{
    (void)unused;
    extern int void_native_worker_affinity_apply(void);
    (void)void_native_worker_affinity_apply();   /* not the guest core it was created from */
    for (;;) {
        struct timespec delay = { 0, 20000000 };
        nanosleep(&delay, NULL);
        if (__atomic_load_n(&log_ring_tail, __ATOMIC_ACQUIRE) != __atomic_load_n(&log_ring_head, __ATOMIC_ACQUIRE))
            log_drain();
    }
    return NULL;
}

static void log_writer_start(void)
{
    pthread_t thread;
    if (!pthread_create(&thread, NULL, log_writer, NULL)) pthread_detach(thread);
}

/* A host signal: what is still queued goes out first. No locks, and the tail stays the writer's: storing it here
 * could move it back behind bytes the producer already reused (Grok's review, 2026-11-25). A line may appear twice
 * if the writer thread was writing the same bytes. */
static void log_drain_from_signal(int fd)
{
    uint64_t head = __atomic_load_n(&log_ring_head, __ATOMIC_ACQUIRE), tail = __atomic_load_n(&log_ring_tail, __ATOMIC_ACQUIRE);
    if (fd < 0 || head - tail > LOG_RING_SIZE) return;
    while (tail < head) {
        size_t at = (size_t)(tail % LOG_RING_SIZE), n = (size_t)(head - tail);
        if (n > LOG_RING_SIZE - at) n = LOG_RING_SIZE - at;
        log_write_all(fd, log_ring + at, n);
        tail += n;
    }
}

/* log_lock held: queue (or, with the ring full or in crash mode, write) one line and its newline. */
static void log_file_line(const char *line, size_t length)
{
    if (log_fd < 0) return;
    if (!log_crash_mode && length + 1 <= LOG_RING_SIZE) {
        pthread_once(&log_writer_once, log_writer_start);
        if (log_ring_head + length + 1 - __atomic_load_n(&log_ring_tail, __ATOMIC_ACQUIRE) > LOG_RING_SIZE) log_drain();
        for (size_t i = 0; i <= length; ++i) log_ring[(log_ring_head + i) % LOG_RING_SIZE] = i < length ? line[i] : '\n';
        __atomic_store_n(&log_ring_head, log_ring_head + length + 1, __ATOMIC_RELEASE);
        return;
    }
    log_drain();
    log_write_all(log_fd, line, length);
    log_write_all(log_fd, "\n", 1);
}

void void_log_open(const char *path)
{
    pthread_mutex_lock(&log_lock);
    log_drain();   /* the previous file's queued lines, before its descriptor closes */
    __atomic_store_n(&log_fd, -1, __ATOMIC_RELEASE);
    if (log_file) fclose(log_file);
#ifdef UT3_ANDROID_APP
    log_file = path ? fopen(path, "a") : NULL;
#else
    log_file = path ? fopen(path, "w") : NULL;
#endif
    if (log_file) setvbuf(log_file, NULL, _IOLBF, 0);
    log_file_bytes = 0;
    log_file_capped = 0;
    if (log_file && !fseek(log_file, 0, SEEK_END)) {
        long size = ftell(log_file);   /* the Java session header is already there */
        if (size > 0) log_file_bytes = (uint64_t)size;
    }
    __atomic_store_n(&log_fd, log_file ? fileno(log_file) : -1, __ATOMIC_RELEASE);
    pthread_mutex_unlock(&log_lock);
}

static void log_va(const char *fmt, va_list ap)
{
    char line[2048];
    vsnprintf(line, sizeof line, fmt, ap);
    pthread_mutex_lock(&log_lock);
#ifndef __ANDROID__
    fprintf(stderr, "%s\n", line);   /* an Android app's stderr goes nowhere: one system call per line saved */
#endif
    if (log_file) {
        size_t length = strlen(line) + 1;
        if (!log_file_capped && log_file_bytes + length > LOG_FILE_LIMIT) {
            log_file_capped = 1;
            char note[160];
            int n = snprintf(note, sizeof note, "[log] this file reached %u MiB: later lines go only to logcat (tag BioShock), except a crash dump",
                             (unsigned)(LOG_FILE_LIMIT >> 20));
            log_file_line(note, (size_t)n);
            log_file_bytes += (uint64_t)n + 1;
        }
        if (!log_file_capped || (log_crash_mode && log_file_bytes < (uint64_t)LOG_FILE_LIMIT + LOG_CRASH_RESERVE)) {
            log_file_line(line, length - 1);
            log_file_bytes += length;
        }
    }
#ifdef __ANDROID__
    __android_log_write(ANDROID_LOG_INFO, "BioShock", line);
#endif
    pthread_mutex_unlock(&log_lock);
}

void void_log(const char *fmt, ...)
{
#ifndef RECOMP_TRACE
    static const char *const diagnostic_noise[] = { "[loadmap-null]", "[package-caller]", "[machine-wait]" };
    for (size_t i = 0; i < sizeof diagnostic_noise / sizeof diagnostic_noise[0]; ++i)
        if (!strncmp(fmt, diagnostic_noise[i], strlen(diagnostic_noise[i]))) return;
#endif
    va_list ap;
    va_start(ap, fmt);
    log_va(fmt, ap);
    va_end(ap);
}

void void_log_flush(void)
{
    pthread_mutex_lock(&log_lock);
    fflush(stderr);
    log_drain();
    pthread_mutex_unlock(&log_lock);
}

/* ------------------------------------------------------------- dispatch */
typedef struct { uint32_t va; void_fn fn; } slot_t;
static slot_t *table;
static uint32_t table_mask, table_used;
static uint32_t image_lo[64], image_hi[64];
static int image_count;

static inline uint32_t hash_va(uint32_t va) { return (va * 0x9E3779B1u) >> 7; }

static void table_grow(void)
{
    uint32_t capacity = table ? (table_mask + 1) * 2 : 1u << 18;
    slot_t *old = table;
    uint32_t old_capacity = table ? table_mask + 1 : 0;
    table = (slot_t *)calloc(capacity, sizeof *table);
    if (!table) void_fatal("dispatch table allocation failed");
    table_mask = capacity - 1;
    table_used = 0;
    for (uint32_t i = 0; i < old_capacity; ++i)
        if (old[i].va) void_dispatch_add(old[i].va, old[i].fn);
    free(old);
}

/* A direct-mapped cache in front of the table for void_resolve. Indirect
 * calls (C++ virtual calls above all) hit the same few thousand targets over
 * and over, and the table (megabytes) misses the data cache on nearly every
 * probe. Only the thread holding the machine resolves, so plain stores do. */
#define RESOLVE_CACHE_BITS 14
/* `diagnostic` marks the few guest targets whose calls still pass through
 * resolve_diagnostics (logging and the scene note); every other hit returns at once. */
static struct { uint32_t va, diagnostic; void_fn fn; } resolve_cache[1u << RESOLVE_CACHE_BITS];
static inline uint32_t resolve_slot(uint32_t va) { return (va * 0x9E3779B1u) >> (32 - RESOLVE_CACHE_BITS); }

static void icache_flush(void);

void void_dispatch_add(uint32_t va, void_fn fn)
{
    if (!va) void_fatal("dispatch: VA 0 cannot be registered");
    resolve_cache[resolve_slot(va)].va = 0;   /* a re-registered VA must not keep its old target */
    if (!table || (table_used + 1) * 2 > table_mask + 1) table_grow();
    for (uint32_t i = hash_va(va) & table_mask;; i = (i + 1) & table_mask) {
        if (table[i].va == va) {
            if (table[i].fn != fn) icache_flush();
            table[i].fn = fn; return;
        }
        if (!table[i].va) { table[i].va = va; table[i].fn = fn; ++table_used; return; }
    }
}

void_fn void_dispatch_find(uint32_t va)
{
    if (!va || !table) return NULL;
    for (uint32_t i = hash_va(va) & table_mask;; i = (i + 1) & table_mask) {
        if (table[i].va == va) return table[i].fn;
        if (!table[i].va) return NULL;
    }
}

void void_dispatch_image(uint32_t lo, uint32_t hi)
{
    if (image_count < 64) { image_lo[image_count] = lo; image_hi[image_count] = hi; ++image_count; }
}

static int in_image(uint32_t va)
{
    for (int i = 0; i < image_count; ++i) if (va >= image_lo[i] && va < image_hi[i]) return 1;
    return 0;
}

void void_dispatch_init(void)
{
    for (uint32_t i = 0; i < recomp_dispatch_count; ++i) {
        if (i && recomp_dispatch_table[i].address <= recomp_dispatch_table[i - 1].address)
            void_fatal("dispatch table not strictly sorted at %u", i);
        void_dispatch_add(recomp_dispatch_table[i].address, recomp_dispatch_table[i].func);
    }
    void_log("[void] dispatch: %u lifted entries", recomp_dispatch_count);
}

/* Synthetic VAs. Each thunk owns 16 bytes of a reserved, inaccessible range,
 * so a stray data access through one faults instead of reading garbage. */
#define MAX_THUNKS ((GUEST_THUNK_LIMIT - GUEST_THUNK_BASE) / 16u)
static const char **thunk_names;
static uint32_t thunk_count;

uint32_t void_thunk(void_fn fn, const char *name)
{
    if (!thunk_names) thunk_names = (const char **)calloc(MAX_THUNKS, sizeof *thunk_names);
    if (thunk_count >= MAX_THUNKS) void_fatal("thunk space exhausted");
    uint32_t va = GUEST_THUNK_BASE + 16u * thunk_count;
    thunk_names[thunk_count++] = name;
    void_dispatch_add(va, fn);
    return va;
}

const char *void_thunk_name(uint32_t va)
{
    if (va < GUEST_THUNK_BASE || va >= GUEST_THUNK_BASE + 16u * thunk_count || (va & 15u)) return NULL;
    return thunk_names[(va - GUEST_THUNK_BASE) / 16u];
}

/* x86 recomp_lookup*: the host code ported from Windows asks by kind. */
recomp_func_t recomp_lookup(uint32_t va)
{
    return in_image(va) ? (recomp_func_t)void_dispatch_find(va) : NULL;
}

recomp_func_t recomp_lookup_import(uint32_t va)
{
    return void_thunk_name(va) ? (recomp_func_t)void_dispatch_find(va) : NULL;
}

recomp_func_t recomp_lookup_manual(uint32_t va)
{
    (void)va;
    return NULL;
}

/* VOID_COUNT_THUNKS=1: count guest calls into each host thunk (Win32 and COM
 * methods all go through void_resolve) and log the busiest per frame. */
static int count_thunks = -1;
static uint32_t *thunk_calls;

void void_thunk_stats(uint32_t frames)
{
    if (count_thunks <= 0 || !thunk_calls || !frames) return;
    for (int rank = 0; rank < 12; ++rank) {
        uint32_t best = 0, best_i = 0;
        for (uint32_t i = 0; i < thunk_count; ++i) if (thunk_calls[i] > best) { best = thunk_calls[i]; best_i = i; }
        if (!best) break;
        void_log("[thunks] %8.1f/frame %s", (double)best / frames, thunk_names[best_i]);
        thunk_calls[best_i] = 0;
    }
    memset(thunk_calls, 0, thunk_count * sizeof *thunk_calls);
}

/* VOID_TRACE_CALLS=va[,va...] (hex, up to 16): indirect calls to these guest
 * functions log the object (ECX), the calling function and the EAX returned,
 * whenever the result for that object changes. Only the machine holder runs
 * guest code, so the bookkeeping needs no lock. */
#define TRACE_MAX 16
#define TRACE_OBJECTS 32
static int trace_n = -1;
static uint32_t trace_va[TRACE_MAX];
static void_fn trace_real[TRACE_MAX];
static struct { uint32_t ecx, eax, calls; } trace_obj[TRACE_MAX][TRACE_OBJECTS];

static void trace_call(int k)
{
    uint32_t ecx = g_ecx, caller = g_cur_func, entry_esp = g_esp;
    if (trace_va[k] == 0x1800CA20u) {
        static unsigned pauses;
        uint32_t bink = MEM32(entry_esp + 4);
        if (pauses++ < 32 && bink >= 0x10000u && void_vm_readable(bink, 900))
            void_log("[bink-pause] begin tid=%u bink=%08X pause=%u mutex=%08X paused=%u", void_thread_id(), bink,
                MEM32(entry_esp + 8), MEM32(bink + 0x344), MEM32(bink + 0xDC));
    }
    if (trace_va[k] == 0x1800C800u || trace_va[k] == 0x1800BAA0u) {
        static unsigned calls[2];
        uint32_t slot = trace_va[k] == 0x1800BAA0u;
        uint32_t n = calls[slot]++;
        uint32_t bink = MEM32(entry_esp + 4);
        if (n < 32 && bink >= 0x10000u && void_vm_readable(bink, 900))
            void_log("[bink-enter] fn=%08X n=%u bink=%08X played=%u decoded=%u last=%u flags=%08X threads=%u io=%u paused=%u timer=%u",
                trace_va[k], n, bink, MEM32(bink + 0xC), MEM32(bink + 0x258), MEM32(bink + 0x26C),
                MEM32(bink + 0x1C), MEM32(bink + 0x2E8), MEM32(bink + 0x288), MEM32(bink + 0xDC), MEM32(bink + 0x268));
    }
    if (trace_va[k] == 0x10BF7A30u || trace_va[k] == 0x10BF6570u || trace_va[k] == 0x10A73DB0u) {
        static unsigned flash_calls[3];
        uint32_t slot = trace_va[k] == 0x10BF6570u ? 1 : trace_va[k] == 0x10A73DB0u ? 2 : 0;
        uint32_t n = flash_calls[slot]++;
        if (n < 40) {
            uint32_t count = ecx >= 0x10000u && void_vm_readable(ecx + 284, 4) ? MEM32(ecx + 284) : 0;
            uint32_t movie = count && void_vm_readable(MEM32(ecx + 280), 4) ? MEM32(MEM32(ecx + 280)) : 0;
            void_log("[flash] fn=%08X n=%u this=%08X arg=%08X flags68=%02X count=%u movie=%08X visible256=%02X",
                trace_va[k], n, ecx, MEM32(entry_esp + 8),
                ecx >= 0x10000u && void_vm_readable(ecx + 68, 1) ? MEM8(ecx + 68) : 0, count, movie,
                movie >= 0x10000u && void_vm_readable(movie + 256, 1) ? MEM8(movie + 256) : 0);
        }
    }
    if (trace_va[k] == 0x10A78B50u || trace_va[k] == 0x10A73C70u || trace_va[k] == 0x10A65DA0u) {
        static unsigned ticks[3];
        uint32_t slot = trace_va[k] == 0x10A73C70u ? 1 : trace_va[k] == 0x10A65DA0u ? 2 : 0;
        uint32_t n = ticks[slot]++;
        if (n < 40) {
            uint32_t view = ecx >= 0x10000u && void_vm_readable(ecx + 268, 4) ? MEM32(ecx + 268) : 0;
            void_log("[flash-tick] fn=%08X n=%u this=%08X arg=%08X gate=%u/%u view=%08X play117=%u flags68=%02X",
                trace_va[k], n, ecx, MEM32(entry_esp + 8), MEM32(0x11129754), MEM32(0x111299E8), view,
                view >= 0x10000u && void_vm_readable(view + 117, 1) ? MEM8(view + 117) : 0,
                ecx >= 0x10000u && void_vm_readable(ecx + 68, 1) ? MEM8(ecx + 68) : 0);
        }
    }
    if (trace_va[k] == 0x10BFA7A0u) {
        static unsigned imports;
        if (imports++ < 80) {
            uint32_t resource = MEM32(entry_esp + 4);
            char *text = resource >= 0x10000u && void_vm_readable(resource, 2) ? void_astr_utf8(resource) : NULL;
            void_log("[flash-import] this=%08X resource=%s exports=%d out=%08X", ecx, text ? text : "?",
                ecx >= 0x10000u && void_vm_readable(ecx + 60, 4) ? (int)MEM32(ecx + 60) : -1, MEM32(entry_esp + 8));
            free(text);
        }
    }
    if (trace_va[k] == 0x10A65370u) {
        static unsigned commands;
        if (commands++ < 256) {
            uint32_t command = MEM32(entry_esp + 8), args = MEM32(entry_esp + 12);
            char *command_text = command >= 0x10000u && void_vm_readable(command, 2) ? void_astr_utf8(command) : NULL;
            char *args_text = args >= 0x10000u && void_vm_readable(args, 2) ? void_astr_utf8(args) : NULL;
            void_log("[fscommand] command=%s args=%s context=%08X", command_text ? command_text : "?",
                args_text ? args_text : "?", MEM32(entry_esp + 16));
            free(command_text);
            free(args_text);
        }
    }
    if (trace_va[k] == 0x10A66CD0u) {
        static unsigned invocations;
        if (invocations++ < 64) {
            uint32_t name = MEM32(entry_esp + 8);
            char *text = name >= 0x10000u && void_vm_readable(name, 2) ? void_astr_utf8(name) : NULL;
            void_log("[movie-invoke] movie=%08X name=%s arg3=%08X arg4=%08X", MEM32(entry_esp + 4),
                text ? text : "?", MEM32(entry_esp + 12), MEM32(entry_esp + 16));
            free(text);
        }
    }
    if (trace_va[k] == 0x10CE87F0u) {
        static unsigned streams;
        if (streams++ < 64) {
            uint32_t bank = MEM32(entry_esp + 12), count = bank >= 0x10000u && void_vm_readable(bank + 4, 4) ? MEM32(bank + 4) : 0;
            uint32_t address = count ? MEM32(bank) : 0;
            char *name = address >= 0x10000u && void_vm_readable(address, 2) ? void_wstr_utf8(address) : NULL;
            char *language = void_wstr_utf8(0x1112F8D8u);
            void_log("[audio-stream] source=%08X request=%08X bank=%s language=%s", ecx, MEM32(entry_esp + 8),
                name ? name : "", language ? language : "?");
            if (name && strstr(name, "vo_S_1_At_PickUpRadioTwo"))
                bioshock_sample_watch = 1;
            free(name);
            free(language);
        }
    }
    if (trace_va[k] == 0x10A65A60u) {
        static unsigned key_events;
        uint32_t key = MEM32(entry_esp + 4), type = MEM32(entry_esp + 8);
        if (key_events++ < 40)
            void_log("[flash-key] this=%08X key_ptr=%08X key=%u type_ptr=%08X type=%u", ecx, key,
                key >= 0x10000u && void_vm_readable(key, 1) ? MEM8(key) : 0, type,
                type >= 0x10000u && void_vm_readable(type, 1) ? MEM8(type) : 0);
    }
    if (trace_va[k] == 0x10A65DA0u && void_vm_readable(ecx + 268, 4)) {
        static uint32_t last_tick[4] = { UINT32_MAX, UINT32_MAX, UINT32_MAX, UINT32_MAX };
        static unsigned tick_calls;
        uint32_t tick[4] = { MEM32(ecx + 240), MEM32(ecx + 244), MEM32(ecx + 256), MEM32(entry_esp + 8) };
        unsigned n = tick_calls++;
        if (n < 12 || memcmp(tick, last_tick, sizeof tick)) {
            memcpy(last_tick, tick, sizeof tick);
            void_log("[flash-scene-tick] this=%08X x=%d y=%d buttons=%u notify=%u root=%08X scale=%08X/%08X",
                ecx, (int)tick[0], (int)tick[1], tick[2], tick[3], MEM32(ecx + 268), MEM32(ecx + 248), MEM32(ecx + 252));
        }
    }
    if (trace_va[k] == 0x10BF6330u) {
        static uint32_t last_notify[3] = { UINT32_MAX, UINT32_MAX, UINT32_MAX };
        uint32_t notify[3] = { MEM32(entry_esp + 4), MEM32(entry_esp + 8), MEM32(entry_esp + 12) };
        if (memcmp(notify, last_notify, sizeof notify)) {
            memcpy(last_notify, notify, sizeof notify);
            void_log("[flash-notify-mouse] root=%08X x=%d y=%d buttons=%u", ecx,
                (int)notify[0], (int)notify[1], notify[2]);
        }
    }
    if (trace_va[k] == 0x10A655F0u) {
        static unsigned tick_gate_calls;
        if (tick_gate_calls++ < 16)
            void_log("[flash-can-tick] movie=%08X owner=%08X", ecx,
                void_vm_readable(ecx + 268, 4) ? MEM32(ecx + 268) : 0);
    }
    if (trace_va[k] == 0x10C12BD0u) {
        static unsigned button_events;
        if (button_events++ < 48)
            void_log("[flash-button] this=%08X id=%u state=%u", ecx, MEM8(entry_esp + 4),
                void_vm_readable(ecx + 152, 4) ? MEM32(ecx + 152) : 0);
    }
    if (trace_va[k] == 0x10D9EC40u || trace_va[k] == 0x10DA2DD0u || trace_va[k] == 0x10D82AD0u) {
        static unsigned input_calls[3];
        uint32_t slot = trace_va[k] == 0x10DA2DD0u ? 1 : trace_va[k] == 0x10D82AD0u ? 2 : 0;
        if (input_calls[slot]++ < 40) {
            if (!slot) void_log("[input] forward this=%08X key=%u type=%u", ecx, MEM32(entry_esp + 4), MEM32(entry_esp + 8));
            else if (slot == 1) void_log("[input] viewport this=%08X msg=%04X wparam=%08X", ecx, MEM32(entry_esp + 8), MEM32(entry_esp + 12));
            else void_log("[input] wndproc hwnd=%08X msg=%04X count=%u objects=%08X disabled=%u",
                MEM32(entry_esp + 4), MEM32(entry_esp + 8), MEM32(0x112C7F90), MEM32(0x112C7F8C), MEM32(0x111C0384));
        }
    }
    if (trace_va[k] == 0x10AC9B10u || trace_va[k] == 0x109725F0u || trace_va[k] == 0x1094E090u ||
        trace_va[k] == 0x10ACDD00u || trace_va[k] == 0x10AC8510u) {
        uint32_t offset = trace_va[k] == 0x1094E090u ? 20 : trace_va[k] == 0x10AC8510u ? 16 : 8;
        uint32_t source = MEM32(entry_esp + offset);
        if (source >= 0x10000u && void_vm_readable(source, 2)) {
            char *text = void_wstr_utf8(source);
            void_log("[save-path] fn=%08X source=%s", trace_va[k], text ? text : "");
            free(text);
        } else {
            void_log("[save-path] fn=%08X source=%08X", trace_va[k], source);
        }
    }
    if (trace_va[k] == 0x1092A930u) {
        void_log("[save-check] arg0=%08X arg1=%08X", MEM32(entry_esp + 4), MEM32(entry_esp + 8));
    }
    if (trace_va[k] == 0x10CE7D30u)
        void_log("[audio-init-enter] this=%08X args=%08X/%08X/%08X", ecx,
            MEM32(entry_esp + 4), MEM32(entry_esp + 8), MEM32(entry_esp + 12));
    trace_real[k]();
    uint32_t eax = g_eax;
    if (trace_va[k] == 0x10A655F0u) {
        static unsigned tick_gate_results;
        if (tick_gate_results++ < 16)
            void_log("[flash-can-tick-result] movie=%08X result=%u", ecx, eax & 0xFFu);
    }
    int i = 0;
    while (i < TRACE_OBJECTS - 1 && trace_obj[k][i].ecx && trace_obj[k][i].ecx != ecx) ++i;
    if (trace_obj[k][i].ecx != ecx || trace_obj[k][i].eax != eax || !trace_obj[k][i].calls) {
        char peek[80] = "";
        if (eax >= 0x10000u && void_vm_readable(eax, 32)) {
            /* What the result points at: printable bytes, '.' for the rest (UTF-16 shows as "n.a.m.e."). */
            size_t n = (size_t)snprintf(peek, sizeof peek, " [");
            for (uint32_t b = 0; b < 32 && n < sizeof peek - 2; ++b) {
                uint8_t c = MEM8(eax + b);
                peek[n++] = (char)(c >= 32 && c < 127 ? c : '.');
            }
            peek[n++] = ']';
            peek[n] = 0;
        }
        int trace_result = 1;
        if (trace_va[k] == 0x1800C800u || trace_va[k] == 0x1800BAA0u) {
            static unsigned result_calls[2];
            unsigned slot = trace_va[k] == 0x1800BAA0u;
            trace_result = ++result_calls[slot] % 120 == 0;
        }
        if (trace_result) void_log("[trace] %08X(ecx=%08X) from %08X -> %08X after %u calls%s", trace_va[k], ecx, caller, eax,
            trace_obj[k][i].calls, peek);
        if (trace_va[k] == 0x10BAC4D0 || trace_va[k] == 0x10AF0200) {
            static uint32_t loop_counts[2];
            uint32_t slot = trace_va[k] == 0x10AF0200;
            if (loop_counts[slot]++ < 32) void_log("[movie-loop] %08X this=%08X ret=%08X", trace_va[k], ecx, eax);
        }
        if (trace_va[k] == 0x1800CA20 || trace_va[k] == 0x1800C800 || trace_va[k] == 0x1800C140 || trace_va[k] == 0x1800C060) {
            static uint32_t counts[5];
            uint32_t slot = trace_va[k] == 0x1800CA20 ? 0 : trace_va[k] == 0x1800C800 ? 1 : trace_va[k] == 0x1800C140 ? 2 : 3;
            uint32_t n = counts[slot]++;
            uint32_t bink = MEM32(entry_esp + 4);
            if ((n < 16 || n % 120 == 0) && bink >= 0x10000u && void_vm_readable(bink, 900)) {
                uint32_t audio_ptr = MEM32(bink + 0x2F0);
                void_log("[bink] %08X ret=%08X handle=%08X played=%u decoded=%u last=%u threads=%u paused=%u fps=%u/%u audio=%u flags=%08X timer=%u",
                    trace_va[k], eax, bink, MEM32(bink + 0xC), MEM32(bink + 0x258), MEM32(bink + 0x26C),
                    MEM32(bink + 0x2E8), MEM32(bink + 0xDC), MEM32(bink + 0x14), MEM32(bink + 0x18),
                    void_vm_readable(audio_ptr + 0x6C, 4) ? MEM32(audio_ptr + 0x6C) : 0, MEM32(bink + 0x1C), MEM32(bink + 0x268));
            }
        }
        if (trace_va[k] == 0x10A74A50 && eax && void_vm_readable(eax, 0x110)) {
            uint32_t name = MEM32(eax + 0x24), entry = name < 65536u ? MEM32(MEM32(0x11297070) + 4 * name) : 0;
            char *text = entry && void_vm_readable(entry + 16, 32) ? void_wstr_utf8(entry + 16) : NULL;
            uint32_t root = MEM32(eax + 0x10C), path = MEM32(eax + 0x44), alt = MEM32(eax + 0x38);
            char *path_text = path && void_vm_readable(path, 4) ? void_wstr_utf8(path) : NULL;
            char *alt_text = alt && void_vm_readable(alt, 4) ? void_wstr_utf8(alt) : NULL;
            uint32_t inner = root && void_vm_readable(root, 16) ? MEM32(root + 0xC) : 0;
            uint32_t advance = inner && void_vm_readable(inner, 4) && void_vm_readable(MEM32(inner) + 40, 4) ? MEM32(MEM32(inner) + 40) : 0;
            void_log("[movie] object=%08X vt=%08X class=%08X name=%s path=%s alt=%s def=%08X root=%08X root_vt=%08X play117=%u inner=%08X advance=%08X hidden40=%08X state90=%08X state100=%08X stateB8=%08X",
                eax, MEM32(eax), MEM32(eax + 0x2C), text ? text : "?", path_text ? path_text : "?", alt_text ? alt_text : "?",
                MEM32(eax + 0x108), root, root && void_vm_readable(root, 8) ? MEM32(root) : 0,
                root && void_vm_readable(root + 117, 1) ? MEM8(root + 117) : 0, inner, advance,
                MEM32(eax + 0x40), MEM32(eax + 0x90), MEM32(eax + 0x100), MEM32(eax + 0xB8));
            free(path_text);
            free(alt_text);
            free(text);
        }
        trace_obj[k][i].ecx = ecx;
        trace_obj[k][i].eax = eax;
        trace_obj[k][i].calls = 0;
    }
    ++trace_obj[k][i].calls;
}

#define TRACE_TRAMPOLINE(k) static void trace_trampoline##k(void) { trace_call(k); }
TRACE_TRAMPOLINE(0) TRACE_TRAMPOLINE(1) TRACE_TRAMPOLINE(2) TRACE_TRAMPOLINE(3)
TRACE_TRAMPOLINE(4) TRACE_TRAMPOLINE(5) TRACE_TRAMPOLINE(6) TRACE_TRAMPOLINE(7)
TRACE_TRAMPOLINE(8) TRACE_TRAMPOLINE(9) TRACE_TRAMPOLINE(10) TRACE_TRAMPOLINE(11)
TRACE_TRAMPOLINE(12) TRACE_TRAMPOLINE(13) TRACE_TRAMPOLINE(14) TRACE_TRAMPOLINE(15)
static const void_fn trace_trampolines[TRACE_MAX] = {
    trace_trampoline0, trace_trampoline1, trace_trampoline2, trace_trampoline3,
    trace_trampoline4, trace_trampoline5, trace_trampoline6, trace_trampoline7,
    trace_trampoline8, trace_trampoline9, trace_trampoline10, trace_trampoline11,
    trace_trampoline12, trace_trampoline13, trace_trampoline14, trace_trampoline15,
};

static void_fn trace_wrap(uint32_t va, void_fn fn)
{
    if (trace_n < 0) {
        trace_n = 0;
        const char *env = getenv("VOID_TRACE_CALLS");
        for (char *end; env && *env && trace_n < TRACE_MAX; env = *end ? end + 1 : end) {
            uint32_t v = (uint32_t)strtoul(env, &end, 16);
            if (end == env) break;
            trace_va[trace_n++] = v;
        }
    }
    for (int k = 0; k < trace_n; ++k) {
        if (trace_va[k] != va) continue;
        trace_real[k] = fn;
        return trace_trampolines[k];
    }
    return fn;
}

/* Guest targets with call-time diagnostics or the scene note in void_resolve. */
static inline int resolve_diagnostic_va(uint32_t va)
{
    return va == 0x10CDFA40u || va == 0x10CE7D30u || va == 0x10014FB0u || va == 0x109EA620u ||
           va == 0x10BA8140u || va == 0x10C12BD0u || va == 0x10BFF7C0u || va == 0x10BF6330u ||
           va == 0x10AC6C50u || va == 0x10C123C0u;
}

static __attribute__((noinline)) recomp_func_t resolve_slow(uint32_t va)
{
    void_module_prepare_call(va);
    if (__builtin_expect(count_thunks != 0, 0)) {
        if (count_thunks < 0) {
            const char *env = getenv("VOID_COUNT_THUNKS");
            count_thunks = env && *env == '1';
            if (count_thunks) thunk_calls = (uint32_t *)calloc(MAX_THUNKS, sizeof *thunk_calls);
        }
        if (count_thunks && va >= GUEST_THUNK_BASE && va < GUEST_THUNK_BASE + 16u * thunk_count)
            ++thunk_calls[(va - GUEST_THUNK_BASE) >> 4];
    }
    uint32_t slot = resolve_slot(va);
    if (__builtin_expect(resolve_cache[slot].va == va && va, 1)) return (recomp_func_t)resolve_cache[slot].fn;
    void_fn fn = void_dispatch_find(va);
    if (__builtin_expect(fn != NULL, 1)) {
        if (__builtin_expect(trace_n != 0, 0)) fn = trace_wrap(va, fn);
        resolve_cache[slot].fn = fn;
        resolve_cache[slot].diagnostic = resolve_diagnostic_va(va);
        resolve_cache[slot].va = va;
        return (recomp_func_t)fn;
    }
    if (va == RECOMP_RETADDR) void_lift_trap(va, "return-address marker used as a call target");
    if (va < 0x10000u) void_lift_trap(va, "null or low guest control-flow target");
    if (in_image(va)) void_lift_trap(va, "unresolved guest control-flow target (not lifted)");
    void_lift_trap(va, "control-flow target outside every lifted image and thunk");
    return NULL;
}

/* appInit's engine CPU count (0x109522B1, tools/lift_game.py). The rendering
 * thread, threaded resource updates, one more threaded update and the
 * HavokNumThreads clamp derive from it. The guest machine runs one guest
 * thread at a time, so the engine uses its original single-core
 * configuration (the 2007 minimum-spec path): on the RMX3851 the same build
 * ran 45.9 instead of 43.5 FPS and lost a recurring 170 ms frame
 * (perf_optimization_295.md). GetSystemInfo stays unchanged for FMOD, Bink and
 * the CRT. QA A/B: BIOSHOCK_ENGINE_SINGLE_CORE=0 keeps the multi-core path. */
uint32_t void_bioshock_engine_cpu_count(uint32_t processors)
{
    const char *single = getenv("BIOSHOCK_ENGINE_SINGLE_CORE");
    uint32_t engine = single && *single == '0' ? processors : 1u;
    void_log("[engine] CPU count %u (system %u)", engine, processors);
    return engine;
}

/* Keep the common cache hit free of the diagnostic/miss path's saved registers. */
extern void void_bioshock_note_scene(uint32_t scene);
void void_bioshock_memcpy_origin(uint32_t caller)
{
    void_log("[memcpy-origin] caller=%08X destination=%08X source=%08X size=%u", caller, MEM32(g_esp + 4), MEM32(g_esp + 8), MEM32(g_esp + 12));
}
static __attribute__((noinline)) void resolve_diagnostics(uint32_t va)
{
    if (va == 0x10CDFA40u || va == 0x10CE7D30u)
        void_log("[audio-init-resolve] target=%08X caller=%08X this=%08X", va, g_cur_func, g_ecx);
    if (va == 0x10014FB0u && !MEM32(g_esp + 4) && MEM32(g_esp + 12)) {
        void_log("[memcpy-null] source=%08X size=%u caller=%08X return=%08X", MEM32(g_esp + 8), MEM32(g_esp + 12), g_cur_func, MEM32(g_esp));
        uint32_t frame = g_ebp;
        for (unsigned depth = 0; depth < 16 && void_vm_readable(frame, 8); ++depth) {
            uint32_t next = MEM32(frame), caller = MEM32(frame + 4);
            void_log("[guest-frame] depth=%u ebp=%08X next=%08X caller=%08X", depth, frame, next, caller);
            if (next <= frame || next - frame > 0x100000u) break;
            frame = next;
        }
        for (uint32_t offset = 0; offset < 128 && void_vm_readable(g_esp + offset, 32); offset += 32) {
            void_log("[guest-stack] +%03X %08X %08X %08X %08X %08X %08X %08X %08X", offset,
                MEM32(g_esp + offset), MEM32(g_esp + offset + 4), MEM32(g_esp + offset + 12), MEM32(g_esp + offset + 16),
                MEM32(g_esp + offset + 20), MEM32(g_esp + offset + 24), MEM32(g_esp + offset + 28), MEM32(g_esp + offset + 32));
        }
        void_dump_state("null memcpy destination");
    }
#ifndef RECOMP_TRACE
    if (va == 0x109EA620u) {
        static unsigned calls;
        uint32_t local = MEM32(g_ecx + 0x44), controller = local ? MEM32(local + 0xC8) : 0;
        if (MEM32(g_esp + 8) != 4 && calls++ < 32)
            void_log("[menu-input-key] caller=%08X esp=%08X s0=%08X s4=%08X s8=%08X sC=%08X s10=%08X s14=%08X controller=%08X flags54=%08X",
                g_cur_func, g_esp, MEM32(g_esp), MEM32(g_esp + 4), MEM32(g_esp + 8), MEM32(g_esp + 12), MEM32(g_esp + 16), MEM32(g_esp + 20),
                controller, controller ? MEM32(controller + 0x54) : 0);
    }
    if (va == 0x10BA8140u) {
        static unsigned calls;
        if (calls++ < 16) void_log("[menu-viewport-input] caller=%08X this=%08X a0=%08X a4=%08X a8=%08X type690=%08X",
            g_cur_func, g_ecx, MEM32(g_esp + 4), MEM32(g_esp + 8), MEM32(g_esp + 12), MEM32(g_ecx + 0x690));
    }
    if (va == 0x10C12BD0u) {
        static unsigned calls;
        if (MEM8(g_esp + 4) == 1) bioshock_flash_trace = 1;
        if (calls++ < 32) void_log("[menu-button] caller=%08X this=%08X event=%u",
            g_cur_func, g_ecx, MEM8(g_esp + 4));
    } else if (va == 0x10BFF7C0u) {
        static unsigned calls;
        if (calls++ < 64) void_log("[menu-action] caller=%08X view=%08X action=%08X count=%u",
            g_cur_func, g_ecx, MEM32(g_esp + 4), MEM32(g_ecx + 0x98));
    }
#endif
    if (va == 0x10BF6330u && g_cur_func == 0x10A65DA0u) void_bioshock_note_scene(g_esi);
    if (va == 0x10AC6C50u) {
        static unsigned calls;
        if (calls++ < 16) void_log("[flash-engine-gate-resolve] caller=%08X this=%08X", g_cur_func, g_ecx);
    }
    if (va == 0x10BF6330u) {
        static unsigned calls;
        if (calls++ < 16) void_log("[flash-notify-resolve] caller=%08X root=%08X", g_cur_func, g_ecx);
    }
#ifdef RECOMP_TRACE
    if (va == 0x10C123C0u) {
        static unsigned calls;
        if (calls++ < 96) {
            uint32_t state = MEM32(g_esp + 4);
            void_log("[button-state] state=%08X flags=%u,%u,%u,%u", state,
                MEM8(state + 16), MEM8(state + 17), MEM8(state + 18), MEM8(state + 19));
        }
    }
#endif
}

/* Every guest indirect call. The cache hit is checked first; only targets
 * marked `diagnostic` (and every miss of one) run resolve_diagnostics, before
 * the target is returned exactly as before. */
recomp_func_t void_resolve(uint32_t va)
{
    if (__builtin_expect(count_thunks == 0, 1)) {
        uint32_t slot = resolve_slot(va);
        if (__builtin_expect(va && resolve_cache[slot].va == va, 1)) {
            if (__builtin_expect(resolve_cache[slot].diagnostic != 0, 0)) resolve_diagnostics(va);
            return (recomp_func_t)resolve_cache[slot].fn;
        }
    }
    if (resolve_diagnostic_va(va)) resolve_diagnostics(va);
    return resolve_slow(va);
}

#include "callsite_cache.inc"

/* ---------------------------------------------------------------- traps */
static atomic_int dumping;

void void_dump_state(const char *why)
{
    void_log("[void] ===== state dump: %s =====", why ? why : "");
    void_log("[void] cur=%08X EAX=%08X ECX=%08X EDX=%08X EBX=%08X ESP=%08X EBP=%08X ESI=%08X EDI=%08X FS=%08X",
        g_cur_func, g_eax, g_ecx, g_edx, g_ebx, g_esp, g_ebp, g_esi, g_edi, g_fs_base);
    void_log("[void] x87 top=%d cw=%04X st0=%g st1=%g flags k=%u a=%08X b=%08X cf=%u",
        g_fp_top, g_fpu_cw, g_st[0], g_st[1], g_flag_k, g_flag_a, g_flag_b, g_flag_cf);
    if (void_vm_readable(g_esp, 64)) {
        void_log("[void] stack %08X: %08X %08X %08X %08X %08X %08X %08X %08X", g_esp,
            MEM32(g_esp), MEM32(g_esp + 4), MEM32(g_esp + 8), MEM32(g_esp + 12),
            MEM32(g_esp + 16), MEM32(g_esp + 20), MEM32(g_esp + 24), MEM32(g_esp + 28));
    }
    char line[1024];
    size_t used = 0;
    uint32_t end = g_enter_idx, count = end < 48 ? end : 48;
    used += (size_t)snprintf(line + used, sizeof line - used, "[void] last entries:");
    for (uint32_t i = end - count; i != end; ++i) {
        used += (size_t)snprintf(line + used, sizeof line - used, " %08X", g_enter_trace[i & (VOID_ENTER_RING - 1)]);
        if (used > sizeof line - 16) { void_log("%s", line); used = 0; line[0] = 0; }
    }
    if (used) void_log("%s", line);
    end = g_icall_trace_idx; count = end < 24 ? end : 24;
    used = 0;
    used += (size_t)snprintf(line + used, sizeof line - used, "[void] last icalls (from->to):");
    for (uint32_t i = end - count; i != end; ++i) {
        uint32_t k = i & (ICALL_TRACE_SIZE - 1);
        const char *name = void_thunk_name(g_icall_trace[k]);
        used += (size_t)snprintf(line + used, sizeof line - used, " %08X->%08X%s%s",
            g_icall_from[k], g_icall_trace[k], name ? ":" : "", name ? name : "");
        if (used > sizeof line - 80) { void_log("%s", line); used = 0; line[0] = 0; }
    }
    if (used) void_log("%s", line);
    void_log_flush();
}

static void log_code_address(const char *what, uintptr_t pc);

/* The lifted callers of a host trap: generated functions keep frame records. */
static void log_caller_backtrace(void)
{
#if defined(__aarch64__)
    uintptr_t fp = (uintptr_t)__builtin_frame_address(0), low = fp;
    for (int depth = 0; depth < 48 && fp >= low && fp - low < (96u << 20) && !(fp & 15); ++depth) {
        const uintptr_t *record = (const uintptr_t *)fp;
        uintptr_t next = record[0], ret = record[1];
        log_code_address("caller", ret);
        if (next <= fp) break;
        fp = next;
    }
#endif
}

_Noreturn void void_fatal(const char *fmt, ...)
{
    va_list ap;
    char message[1024];
    va_start(ap, fmt);
    vsnprintf(message, sizeof message, fmt, ap);
    va_end(ap);
    log_crash_mode = 1;
    void_log("[void] FATAL: %s", message);
    if (!atomic_exchange(&dumping, 1)) {
        log_caller_backtrace();
        void_dump_state(message);
    }
    void_log_flush();
    _exit(70);
}

void void_lift_trap(uint32_t address, const char *instruction)
{
    const char *name = void_thunk_name(address);
    void_fatal("guest trap at %08X%s%s: %s (caller %08X)", address, name ? " " : "", name ? name : "",
        instruction, g_cur_func);
}

/* Validation builds (build_android.py --protocol-check) report each call site
 * whose callee changed a register its summary keeps, and each x87-rewritten
 * body that left the global stack window at the wrong depth; once each. */
static int seen_once(uint32_t key)
{
    static uint32_t seen[4096];
    static unsigned count;
    static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
    int fresh = 1;
    pthread_mutex_lock(&lock);
    for (unsigned i = 0; i < count; ++i)
        if (seen[i] == key) { fresh = 0; break; }
    if (fresh && count < sizeof seen / sizeof seen[0]) seen[count++] = key;
    pthread_mutex_unlock(&lock);
    return fresh;
}

void recomp_protocol_violation(uint32_t site, uint32_t target, uint32_t regs)
{
    if (seen_once(site ^ (regs << 24)))
        void_log("[protocol] site %08X target %08X regs %02X", site, target, regs);
}

void recomp_x87_violation(uint32_t va, int found, int expected)
{
    if (seen_once(va ^ 0x80000000u))
        void_log("[x87] %08X net %d expected %d", va, found, expected);
}

/* A host code address as module+offset (llvm-symbolizer resolves it against
 * the unstripped libut3.so); production builds keep no guest call trace. */
static void log_code_address(const char *what, uintptr_t pc)
{
    Dl_info info;
    if (pc && dladdr((void *)pc, &info) && info.dli_fbase) {
        const char *slash = info.dli_fname ? strrchr(info.dli_fname, '/') : NULL;
        const char *module = slash ? slash + 1 : info.dli_fname ? info.dli_fname : "?";
        void_log("[void] %s %p = %s+0x%lx%s%s", what, (void *)pc, module,
                 (unsigned long)(pc - (uintptr_t)info.dli_fbase), info.dli_sname ? " " : "",
                 info.dli_sname ? info.dli_sname : "");
    } else {
        void_log("[void] %s %p", what, (void *)pc);
    }
}

static struct sigaction previous_actions[32];
static volatile sig_atomic_t crash_marker = -1, signal_log = -1;

void void_note_first_present(void)
{
    port_crash_marker_present(crash_marker);
}

static void on_signal(int signo, siginfo_t *info, void *context)
{
    (void)context;
    const int saved_errno = errno;
    port_crash_marker_signal(crash_marker, signo);
    char line[] = "[void] host signal 00\n";
    line[sizeof line - 4] = '0' + signo / 10;
    line[sizeof line - 3] = '0' + signo % 10;
    log_drain_from_signal(signal_log);
    if (signal_log >= 0) write(signal_log, line, sizeof line - 1);
    if ((unsigned)signo < sizeof previous_actions / sizeof previous_actions[0])
        sigaction(signo, &previous_actions[signo], NULL);
    else
        signal(signo, SIG_DFL);
    /* A synchronous fault repeats at the original instruction for Android's tombstone. */
    if (!info || info->si_code <= 0 || signo == SIGABRT) raise(signo);
    errno = saved_errno;
}

void void_install_fault_handlers(void)
{
    crash_marker = port_crash_marker_open();
    signal_log = log_file ? fileno(log_file) : STDERR_FILENO;
    static char alt[256 * 1024];
    stack_t ss = { .ss_sp = alt, .ss_size = sizeof alt, .ss_flags = 0 };
    sigaltstack(&ss, NULL);
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = on_signal;
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sigemptyset(&sa.sa_mask);
    int signals[] = { SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT };
    for (size_t i = 0; i < sizeof signals / sizeof signals[0]; ++i)
        sigaction(signals[i], &sa, &previous_actions[signals[i]]);
}

/* ---------------------------------------------------------------- CPUID */
/* Captured on the Windows PC the x86 build was validated on (Ryzen 7 5700X),
 * so the guest picks the same MMX/SSE/SSE2 library paths it ran there. */
static const uint32_t cpuid_table[][5] = {
    {0x00000000u, 0x00000010u, 0x68747541u, 0x444D4163u, 0x69746E65u},
    {0x00000001u, 0x00A20F12u, 0x08100800u, 0x7EF8320Bu, 0x178BFBFFu},
    {0x00000007u, 0x00000000u, 0x219C97A9u, 0x0040068Cu, 0x00000010u},
    {0x80000000u, 0x80000023u, 0x68747541u, 0x444D4163u, 0x69746E65u},
    {0x80000001u, 0x00A20F12u, 0x20000000u, 0x75C237FFu, 0x2FC3FBFFu},
    {0x80000002u, 0x20444D41u, 0x657A7952u, 0x2037206Eu, 0x30303735u},
    {0x80000003u, 0x2D382058u, 0x65726F43u, 0x6F725020u, 0x73736563u},
    {0x80000004u, 0x2020726Fu, 0x20202020u, 0x20202020u, 0x00202020u},
    {0x80000005u, 0xFF40FF40u, 0xFF40FF40u, 0x20080140u, 0x20080140u},
    {0x80000006u, 0x48002200u, 0x68004200u, 0x02006140u, 0x01009140u},
};

extern uint32_t void_guest_cpu_count(void);
extern uint32_t void_guest_cpu_id(void);
void void_cpuid(uint32_t leaf, uint32_t subleaf)
{
    (void)subleaf;
    if (leaf == 0x80000008u) {
        uint32_t count = void_guest_cpu_count();
        g_eax = 0x00002020u; /* Virtual 32-bit physical/linear address widths. */
        g_ebx = g_edx = 0;
        /* Legacy UE3 reads the entire ECX + 1 as its core count. A zero
         * APIC-width field also selects Valve's exact NC + 1 fallback. */
        g_ecx = count - 1;
        return;
    }
    for (size_t i = 0; i < sizeof cpuid_table / sizeof cpuid_table[0]; ++i) {
        if (cpuid_table[i][0] == leaf) {
            g_eax = cpuid_table[i][1]; g_ebx = cpuid_table[i][2];
            g_ecx = cpuid_table[i][3]; g_edx = cpuid_table[i][4];
            if (leaf == 1) {
                uint32_t count = void_guest_cpu_count();
                g_ebx = (g_ebx & 0x0000FFFFu) | (count << 16) | (void_guest_cpu_id() << 24);
                if (count == 1) g_edx &= ~(1u << 28);
            }
            return;
        }
    }
    g_eax = g_ebx = g_ecx = g_edx = 0;
}

uint64_t void_cmpxchg8b(uint32_t address, uint64_t expected, uint64_t replacement, uint32_t instruction)
{
    /* The caller owns the FIFO machine lock: no other guest load/store can run
     * during this instruction. Byte accesses also support unaligned and split
     * cache-line/page operands without ARM64's unaligned atomic fault. */
    if (!void_vm_readable(address, 8) || !void_vm_writable(address, 8))
        void_lift_trap(instruction, "CMPXCHG8B requires a readable/writable 8-byte operand");
    atomic_thread_fence(memory_order_seq_cst);
    volatile uint8_t *bytes = (volatile uint8_t *)GPTR(address);
    uint64_t old = 0;
    for (unsigned i = 0; i < 8; ++i) old |= (uint64_t)bytes[i] << (8 * i);
    uint64_t value = old == expected ? replacement : old;
    /* x86 performs a write cycle on failure too; validate all bytes before any write. */
    for (unsigned i = 0; i < 8; ++i) bytes[i] = (uint8_t)(value >> (8 * i));
    atomic_thread_fence(memory_order_seq_cst);
    return old;
}

void void_fpu_clear_exceptions(uint32_t address)
{
    /* FNCLEX clears IE..ES/SF and B; the existing condition/TOP encoding survives. */
    if (void_fpu_cmp >= 0x10000 && void_fpu_cmp <= 0x1FFFF)
        void_fpu_cmp &= ~0x80FF;
    else if (void_fpu_cmp < -1 || void_fpu_cmp > 2)
        void_lift_trap(address, "invalid x87 status encoding at FNCLEX");
    /* shortcut: arithmetic does not accumulate x87 sticky exceptions; extend producers and FNSTSW before claiming full exception status. */
}

/* ----------------------------------------------------------- FPREM/FPREM1 */
/* The x86 host executed the real instruction on the two doubles; here the
 * same double-model operation is computed exactly: the IEEE remainder of two
 * doubles is exact, and so is the partial-remainder step for exponent gaps
 * below 64, which is every case the Windows host accepted. */
void void_fpu_remainder(uint32_t address, int nearest)
{
    if (g_fp_top < 2 || g_fp_top > 8 || (nearest != 0 && nearest != 1))
        void_lift_trap(address, "unsupported FPREM stack/control word");
    double x = g_st[0], y = g_st[1];
    uint16_t status = 0;
    if (isnan(x) || isnan(y) || isinf(x) || y == 0.0) {
        /* Masked invalid operation: the x87 "real indefinite" QNaN. */
        uint64_t indefinite = 0xFFF8000000000000ull;
        memcpy(&g_st[0], &indefinite, sizeof indefinite);
        g_st_i64[0] = 0;
        void_fpu_cmp = (int)0x10000u;
        return;
    }
    if (isinf(y) || x == 0.0) {
        g_st_i64[0] = 0;          /* x unchanged, quotient 0 */
        void_fpu_cmp = (int)0x10000u;
        return;
    }
    int ex, ey;
    frexp(x, &ex); frexp(y, &ey);
    if (ex - ey < 64) {
        /* fmod and remainder are exact in IEEE arithmetic. The quotient's
         * low three bits (C0 C3 C1 = Q2 Q1 Q0) come from the exact truncated
         * remainder modulo 8|y|: r8 - rt = q8 * y with q8 in 0..7, so the
         * rounded division is within an ulp of a small integer. */
        double rt = fmod(x, y);
        double r8 = fmod(x, 8.0 * y);
        if (isinf(8.0 * y)) r8 = x;   /* |y| near DBL_MAX: |q| < 8 already */
        uint32_t q = (uint32_t)nearbyint(fabs((r8 - rt) / y)) & 7u;
        double r = rt;
        if (nearest) {
            double rn = remainder(x, y);
            if (rn != rt) q = (q + 1u) & 7u;  /* quotient rounded away from zero */
            r = rn;
        }
        if (q & 1u) status |= 0x0200u;   /* C1 = Q0 */
        if (q & 2u) status |= 0x4000u;   /* C3 = Q1 */
        if (q & 4u) status |= 0x0100u;   /* C0 = Q2 */
        g_st[0] = r;
    } else {
        /* Partial reduction (C2 = 1): reduce by y * 2^(D - N), N in 32..63,
         * exactly; callers loop until C2 clears, and the remainder they reach
         * is the same exact fmod/remainder whichever N the hardware used. */
        int n = 32 + ((ex - ey) & 31);
        g_st[0] = fmod(x, ldexp(y, ex - ey - n));
        status |= 0x0400u;
    }
    g_st_i64[0] = 0;
    void_fpu_cmp = (int)(0x10000u | status);
}

static int bioshock_navigation_medical;

static void bioshock_console_command(const char *requested)
{
    /* QA only: a private profile's perf-flags.env may name one console command (for example HDR or
     * BLOOM, both toggles of the original render config) to run on F10 instead of the travel. A list
     * separated by ';' runs its next command on each F10 that reaches the console (level sweeps). */
    const char *qa_command = requested ? NULL : getenv("BIOSHOCK_QA_CONSOLE");
    static unsigned qa_next;
    unsigned qa_count = 0, qa_index = 0;
    char qa_item[256];
    if (qa_command && strchr(qa_command, ';') && !bioshock_navigation_medical) {
        qa_count = 1;
        for (const char *p = qa_command; *p; ++p) qa_count += *p == ';';
        qa_index = qa_next % qa_count;
        const char *start = qa_command;
        for (unsigned i = 0; i < qa_index; ++i) start = strchr(start, ';') + 1;
        size_t length = strcspn(start, ";");
        snprintf(qa_item, sizeof qa_item, "%.*s", (int)length, start);
        qa_command = qa_item;
    }
    const char *command = requested ? requested : qa_command && *qa_command && !bioshock_navigation_medical ? qa_command :
        bioshock_navigation_medical ? "open 1-Medical" : "open 1-Welcome";
    extern uint32_t void_bioshock_find_function(uint32_t object, const char *name);
    const uint32_t process_event = 0x10967870u;
    void_machine_enter();
    uint32_t engine = MEM32(0x111BE314u);
    uint32_t client = engine ? MEM32(engine + 68) : 0;
    uint32_t viewport = client ? MEM32(client + 196) : 0;
    uint32_t controller = viewport ? MEM32(viewport + 64) : 0;
    uint32_t function = controller ? void_bioshock_find_function(controller, "ConsoleCommand") : 0;
    if (!function) function = 0x01F8CE80u;
    if (!controller || !MEM32(controller + 1044) || !void_vm_readable(function + 0x6C, 4) || MEM32(function + 0x6C) != 6) {
        uint32_t player = controller ? MEM32(controller + 1044) : 0;
        uint32_t shape = void_vm_readable(function + 0x6C, 4) ? MEM32(function + 0x6C) : 0;
        void_machine_leave();
        void_log("[navigation] original console is not ready (controller=%08X player=%08X function=%08X +6C=%u)",
            controller, player, function, shape);
        return;
    }
    uint32_t params = void_galloc(24);
    uint32_t units = (uint32_t)strlen(command) + 1;
    uint32_t text = void_galloc(units * 2);
    if (!params || !text) {
        void_gfree(params);
        void_gfree(text);
        void_machine_leave();
        void_log("[navigation] console command allocation failed");
        return;
    }
    memset(GPTR(params), 0, 24);
    MEM32(params) = text;
    MEM32(params + 4) = units;
    MEM32(params + 8) = units;
    void_utf8_to_gw(command, text, units);
    void_bioshock_log_object(function, "console-function");
    void_log("[navigation] dispatch thunk=%08X", MEM32(function + 0xB8));
    uint32_t args[] = { function, params, 0 };
    uint32_t result = void_call_guest_this(process_event, controller, 3, args);
    void_log("[navigation] %s controller=%08X function=%08X result=%08X", command, controller, function, result);
    if (qa_count) {
        void_log("[navigation] QA command %u of %u", qa_index + 1, qa_count);
        ++qa_next;
    }
    /* Travel keeps using the command asynchronously after this host call returns. */
    if (requested) {
        void_gfree(text);
        void_gfree(params);
    }
    void_machine_leave();
}

static atomic_int bioshock_navigation_pending;

void void_bioshock_request_navigation(void)
{
    bioshock_navigation_medical = 0;
    atomic_store_explicit(&bioshock_navigation_pending, 1, memory_order_release);
}

void void_bioshock_request_medical_navigation(void)
{
    bioshock_navigation_medical = 1;
    atomic_store_explicit(&bioshock_navigation_pending, 1, memory_order_release);
}

int void_bioshock_take_navigation(void)
{
    return atomic_exchange_explicit(&bioshock_navigation_pending, 0, memory_order_acquire);
}

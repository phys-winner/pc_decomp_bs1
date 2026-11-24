/* ARM64 command-line bring-up; the guest instructions are compiled ahead of time. */
#define _GNU_SOURCE
#include "host.h"
#include <errno.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

int void_modules_start(void);
void void_files_std_init(void);
void void_texture_cache_init(const char *game);
void void_crt_native_init(void);
void void_bioshock_settings_register(void);
void void_bioshock_render_register(void);

void void_process_exit(uint32_t code, int run_detach)
{
    if (run_detach) void_modules_detach_all(1);
    void_log("[bioshock] process exit %u", code);
    void_log_flush();
    _exit((int)code);
}

int ut3_game_main(const char *game, const char *data, const char *images, int selftest)
{
    setvbuf(stderr, NULL, _IONBF, 0);
    if (mkdir(data, 0775) && errno != EEXIST) {
        fprintf(stderr, "cannot create userdata %s: %s\n", data, strerror(errno));
        return 66;
    }
    const char *monitor = NULL;
#ifdef UT3_ANDROID_APP
    monitor = getenv("PORT_OPTION_CRASH_MONITORING");
#endif
    if (!monitor || strcmp(monitor, "0")) {
        char log_path[1024];
        if (snprintf(log_path, sizeof log_path, "%s/bioshock.log", data) >= (int)sizeof log_path) return 64;
        void_log_open(log_path);
    }
    void_install_fault_handlers();
    void_log("[bioshock] ARM64 static host: game=%s data=%s images=%s", game, data, images);
    if (!void_guest_reserve()) return 65;
    void_threads_init();
    void_dispatch_init();
    void_bioshock_settings_register();
    void_bioshock_render_register();
    if (!void_files_init(game, data)) return 66;
    void_files_std_init();
    void_texture_cache_init(game);
    if (!void_modules_init(images)) return 67;
    void_crt_native_init();
    void_machine_enter();
    if (!void_modules_start()) return 68;
    uint32_t base = void_module_exe_base();
    uint32_t entry = base + MEM32(base + MEM32(base + 0x3C) + 0x28);
    if (!void_dispatch_find(entry)) void_lift_trap(entry, "PE entry has no statically compiled function");
    if (selftest) {
        uint32_t block = void_galloc(32);
        if (!block || !void_vm_writable(block, 32)) void_fatal("guest heap selftest failed");
        MEM32(block) = 0x12345678u;
        if (MEM32(block) != 0x12345678u) void_fatal("guest memory selftest failed");
        void_gfree(block);
        void_log("[selftest] PE binding, DLL/TLS startup, guest heap and static entry lookup PASS; game entry not executed");
        void_machine_leave();
        return 0;
    }
    void_log("[bioshock] starting lifted Bioshock.exe at %08X", entry);
    uint32_t result = void_call_guest(entry, 0, NULL, 1);
    void_log("[bioshock] Bioshock.exe entry returned %08X", result);
    void_machine_leave();
    return (int)result;
}

#ifndef UT3_ANDROID_APP
int main(int argc, char **argv)
{
    const char *game = "/sdcard/BioShock/game", *data = "/sdcard/BioShock/userdata";
    const char *images = "/sdcard/BioShock/images";
    int selftest = 0;
    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--game") && i + 1 < argc) game = argv[++i];
        else if (!strcmp(argv[i], "--data") && i + 1 < argc) data = argv[++i];
        else if (!strcmp(argv[i], "--images") && i + 1 < argc) images = argv[++i];
        else if (!strcmp(argv[i], "--selftest")) selftest = 1;
        else if (!strcmp(argv[i], "-v")) void_verbose = 1;
        else { fprintf(stderr, "unknown or incomplete option %s\n", argv[i]); return 64; }
    }
    int code = ut3_game_main(game, data, images, selftest);
    void_process_exit((uint32_t)code, 0);
}
#endif

#include "../android/host/files.c"
#include "../android/host/data_obb.h"

#include <stdio.h>

static uint32_t api(const char *name, unsigned count, const uint32_t *args)
{
    const void_shim *s = void_shim_find("kernel32.dll", name, 0);
    return s ? void_call_guest(void_thunk(s->fn, name), count, args, 1) : 0;
}

int main(int argc, char **argv)
{
    void_install_fault_handlers();
    void_verbose = 1;
    if (argc != 4 || void_obb_init_external(argv[1])) {
        fprintf(stderr, "usage: test_zip_obb OBB_DIRECTORY MOUNT_INDEX ZIP_ARCHIVE\n");
        return 64;
    }
    int archive = open(argv[3], O_RDONLY);
    if (archive < 0) return 1;
    char descriptor[32];
    snprintf(descriptor, sizeof descriptor, "%d", archive);
    const char *sidecar = "/data/local/tmp/bioshock-sidecar";
    if (setenv("PORT_MOUNT_GAME", argv[2], 1) || setenv("PORT_MOUNT_FD_GAME", descriptor, 1) ||
        setenv("PORT_PART_GAME", sidecar, 1)) return 1;
    if (!void_guest_reserve()) return 1;
    void_threads_init();
    void_dispatch_init();
    void_machine_enter();
    char profile[] = "/data/local/tmp/bioshock-zip-obb-XXXXXX";
    if (!mkdtemp(profile) || !void_files_init(sidecar, profile)) {
        fprintf(stderr, "cannot initialize combined ZIP and OBB\n");
        return 1;
    }
    uint32_t error = 0;
    uint32_t handle = create_file("C:\\Games\\BioShock\\Builds\\Release\\Startup.ini",
        GENERIC_READ_, OPEN_EXISTING_, 0, &error);
    if (handle == WIN_INVALID_HANDLE) {
        fprintf(stderr, "Startup.ini is not readable through the mounted game: %u\n", error);
        return 1;
    }
    uint32_t memory = void_galloc(64);
    uint32_t read_args[5] = {handle, memory, 12, memory + 32, 0};
    int ok = api("ReadFile", 5, read_args) && MEM32(memory + 32) == 12 &&
        !memcmp(GPTR(memory), "[Filesystem]", 12);
    if (!api("CloseHandle", 1, &handle)) ok = 0;
    if (!ok) {
        fprintf(stderr, "wrong Startup.ini contents\n");
        return 1;
    }
    uint32_t attributes = attributes_of("C:\\Games\\BioShock\\Builds\\Release\\Startup.ini");
    if (attributes != 0x20u && attributes != 0x21u) {
        fprintf(stderr, "Startup.ini attributes are missing through the mounted game: %08X\n", attributes);
        return 1;
    }
    struct stat root;
    if (void_obb_stat(sidecar, &root) || !S_ISDIR(root.st_mode)) {
        fprintf(stderr, "mounted root is not a virtual directory\n");
        return 1;
    }
    fprintf(stderr, "Startup.ini read and attributes passed\n");
    char temp[1024];
    snprintf(temp, sizeof temp, "%s/Temp", profile);
    if (mkdir(temp, 0775) && errno != EEXIST) return 1;
    uint32_t create[6] = {WIN_INVALID_HANDLE, 0, 4, 0, 20, 0};
    fprintf(stderr, "before CreateFileMappingA\n");
    uint32_t mapping = api("CreateFileMappingA", 6, create);
    fprintf(stderr, "CreateFileMappingA=%08X\n", mapping);
    uint32_t view_args[5] = {mapping, 2, 0, 0, 0};
    fprintf(stderr, "before MapViewOfFile\n");
    uint32_t view = mapping ? api("MapViewOfFile", 5, view_args) : 0;
    fprintf(stderr, "mapping handle=%08X view=%08X\n", mapping, view);
    if (!view) ok = 0;
    else {
        MEM32(view + 12) = 9;
        if (!api("UnmapViewOfFile", 1, &view)) ok = 0;
        mapping_t *m = void_handle_get(mapping, VOID_OBJ_MAPPING)->data;
        uint32_t value = 0;
        if (pread(m->fd, &value, 4, 12) != 4 || value != 9) ok = 0;
    }
    if (!api("CloseHandle", 1, &mapping)) ok = 0;
    DIR *dir = opendir(temp);
    if (dir) {
        struct dirent *entry;
        while ((entry = readdir(dir))) if (strcmp(entry->d_name, ".") && strcmp(entry->d_name, "..")) ok = 0;
        closedir(dir);
    }
    void_machine_leave();
    zm_close(game_zip);
    game_zip = NULL;
    close(archive);
    void_obb_close();
    if (rmdir(temp)) ok = 0;
    const char *directories[] = {"AppData", "Documents", "ProgramData", "PublicDocuments", "drive_c"};
    for (unsigned i = 0; i < sizeof directories / sizeof *directories; ++i) {
        char path[1024];
        snprintf(path, sizeof path, "%s/%s", profile, directories[i]);
        if (rmdir(path)) ok = 0;
    }
    if (rmdir(profile)) ok = 0;
    if (!ok) return 1;
    puts("PASS stored ZIP PE/index paths, OBB fallback, selected sidecar and FUSE temporary mapping");
    return 0;
}

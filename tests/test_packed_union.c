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
    if (argc != 2 || void_obb_init_external(argv[1])) {
        fprintf(stderr, "usage: test_packed_union OBB_DIRECTORY\n");
        return 64;
    }
    snprintf(game_root, sizeof game_root, "/sdcard/BioShock/game");
    const char *directory = "/sdcard/BioShock/game/Content/Sounds_Windows";
    dir_listing *listing = listing_get(directory, 1);
    if (!listing) {
        fprintf(stderr, "cannot open union directory\n");
        return 1;
    }
    listed_entry *entry = listing_find(listing, "streams_0_audio.rus_fsb");
    if (!entry || entry->kind != DT_REG) {
        fprintf(stderr, "packed-only entry is hidden by the physical directory\n");
        return 1;
    }
    int count = 0;
    for (int i = 0; i < listing->count; ++i)
        if (!strcasecmp(listing->entries[i].name, entry->name)) ++count;
    if (count != 1) {
        fprintf(stderr, "duplicate union entries: %d\n", count);
        return 1;
    }
    char path[1024];
    snprintf(path, sizeof path, "%s/%s", directory, entry->name);
    struct stat st;
    if (cached_stat(path, &st) || !S_ISREG(st.st_mode) || st.st_size != 10521168) {
        fprintf(stderr, "packed-only stat failed\n");
        return 1;
    }
    char host[1024];
    unsigned char kind = DT_UNKNOWN;
    snprintf(host, sizeof host, "%s", directory);
    if (resolve_component(host, sizeof host, entry->name, &kind, 0) != 1 || kind != DT_REG) {
        fprintf(stderr, "packed-only component resolution failed\n");
        return 1;
    }
    mounts[0].win = "C:\\Games\\BioShock";
    snprintf(mounts[0].host, sizeof mounts[0].host, "%s", game_root);
    mount_count = 1;
    uint32_t error = 0;
    uint32_t handle = create_file("C:\\Games\\BioShock\\Content\\System\\..\\FlashMovies\\PausePC.swf",
        GENERIC_READ_, OPEN_EXISTING_, 0, &error);
    file_t *packed = file_get(handle);
    unsigned char signature[3] = {0};
    if (handle == WIN_INVALID_HANDLE || !packed || !packed->packed || packed->packed_size != 5439897 ||
        pread(packed->fd, signature, 3, (off_t)packed->bias) != 3 || memcmp(signature, "FWS", 3)) {
        fprintf(stderr, "normalized packed CreateFile failed: error=%u\n", error);
        return 1;
    }
    if (!void_handle_close(handle)) return 1;
    void_obb_close();
    char temporary[] = "/data/local/tmp/bioshock-profile-cache-XXXXXX";
    if (!mkdtemp(temporary)) return 1;
    char game[1024], profile[1024], file[1024];
    snprintf(game, sizeof game, "%s/game", temporary);
    snprintf(profile, sizeof profile, "%s/userdata", game);
    snprintf(file, sizeof file, "%s/settings.ini", profile);
    if (mkdir(game, 0775) || mkdir(profile, 0775)) return 1;
    snprintf(game_root, sizeof game_root, "%s", game);
    snprintf(data_root, sizeof data_root, "%s", profile);
    FILE *out = fopen(file, "wb");
    if (!out || fputc(1, out) != 1 || fclose(out)) return 1;
    struct stat before, after;
    int ok = !cached_stat(file, &before) && before.st_size == 1;
    out = fopen(file, "wb");
    if (!out || fwrite("12345678901234567", 1, 17, out) != 17 || fclose(out)) return 1;
    ok &= !cached_stat(file, &after) && after.st_size == 17;
    int map_ok = 0;
    if (void_guest_reserve()) {
        void_threads_init();
        void_dispatch_init();
        void_machine_enter();
        char temp[1024];
        snprintf(temp, sizeof temp, "%s/Temp", profile);
        if (mkdir(temp, 0775)) return 1;
        uint32_t create[6] = {WIN_INVALID_HANDLE, 0, 4, 0, 20, 0};
        uint32_t mapping = api("CreateFileMappingA", 6, create);
        if (mapping) {
            uint32_t view_args[5] = {mapping, 2, 0, 0, 0};
            uint32_t view = api("MapViewOfFile", 5, view_args);
            if (view && !MEM32(view + 12)) {
                MEM32(view + 12) = 7;
                map_ok = api("UnmapViewOfFile", 1, &view) == 1;
                mapping_t *m = void_handle_get(mapping, VOID_OBJ_MAPPING)->data;
                uint32_t value = 0;
                map_ok &= pread(m->fd, &value, 4, 12) == 4 && value == 7;
            }
            map_ok &= api("CloseHandle", 1, &mapping) == 1;
        }
        void_machine_leave();
        DIR *dir = opendir(temp);
        if (dir) {
            struct dirent *entry;
            while ((entry = readdir(dir))) if (strcmp(entry->d_name, ".") && strcmp(entry->d_name, "..")) map_ok = 0;
            closedir(dir);
        }
        if (rmdir(temp)) return 1;
    }
    if (unlink(file) || rmdir(profile) || rmdir(game) || rmdir(temporary)) return 1;
    if (!ok || !map_ok) {
        fprintf(stderr, "selected-folder profile or mapping test failed\n");
        return 1;
    }
    puts("PASS physical + OBB union listing, stat, component resolution, normalized packed CreateFile, uncached selected-folder profile and temporary shared mapping");
    return 0;
}

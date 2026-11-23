#include "../android/host/data_obb.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

void void_log(const char *format, ...)
{
    va_list arguments;
    va_start(arguments, format);
    vfprintf(stderr, format, arguments);
    va_end(arguments);
    fputc('\n', stderr);
}

int main(int argc, char **argv)
{
    if (argc != 2) {
        fprintf(stderr, "usage: test_data_obb_runtime OBB_DIRECTORY\n");
        return 64;
    }
    if (void_obb_init_external(argv[1])) {
        fprintf(stderr, "cannot initialize OBB directory\n");
        return 1;
    }
    struct stat st;
    if (void_obb_stat("/sdcard/BioShock/game/Content/Sounds_Windows", &st) || !S_ISDIR(st.st_mode)) {
        fprintf(stderr, "virtual directory missing\n");
        return 1;
    }
    if (void_obb_stat("game", &st) || !S_ISDIR(st.st_mode)) {
        fprintf(stderr, "virtual root missing\n");
        return 1;
    }
    unsigned banks = 0, russian_banks = 0;
    for (unsigned index = 0;; ++index) {
        char name[512];
        int directory = -1;
        int result = void_obb_list("game/Content/Sounds_Windows", index, name, sizeof name, &directory, &st);
        if (result < 0 || (result == 0 && (directory || !S_ISREG(st.st_mode) || !st.st_size))) {
            fprintf(stderr, "invalid bank listing at %u\n", index);
            return 1;
        }
        if (result > 0) break;
        ++banks;
        size_t length = strlen(name);
        if (length >= 8 && !strcasecmp(name + length - 8, ".rus_fsb")) ++russian_banks;
    }
    if (banks != 31 || russian_banks != 10) {
        fprintf(stderr, "unexpected bank counts: %u total, %u Russian\n", banks, russian_banks);
        return 1;
    }
    int found_directory = 0;
    for (unsigned index = 0;; ++index) {
        char name[512];
        int directory = 0;
        if (void_obb_list("game/Content", index, name, sizeof name, &directory, NULL)) break;
        if (directory && !strcmp(name, "Sounds_Windows")) found_directory = 1;
    }
    if (!found_directory) {
        fprintf(stderr, "nested virtual directory missing\n");
        return 1;
    }
    obb_file file;
    if (void_obb_lookup("game/Content/Sounds_Windows/streams_0_audio.rus_fsb", &file)) {
        fprintf(stderr, "Russian bank lookup failed\n");
        return 1;
    }
    close(file.fd);
    if (file.size != 10521168 || (file.bias & 16383)) {
        fprintf(stderr, "unexpected bank layout: size=%llu bias=%llu\n", (unsigned long long)file.size, (unsigned long long)file.bias);
        return 1;
    }
    if (void_obb_lookup("C:\\Games\\BioShock\\Builds\\Release\\Default.ini", &file)) {
        fprintf(stderr, "localized INI lookup failed\n");
        return 1;
    }
    close(file.fd);
    void *data = NULL;
    size_t size = 0;
    if (void_obb_read("game/Builds/Release/Default.ini", &data, &size) || !data ||
        (!strstr(data, "Language=rus") && !strstr(data, "language=rus"))) {
        fprintf(stderr, "localized INI mismatch\n");
        free(data);
        return 1;
    }
    free(data);
    if (void_obb_stat("C:\\Games\\BioShock\\Builds\\Release\\Startup.ini", &st) || !S_ISREG(st.st_mode) ||
        void_obb_read("C:\\Games\\BioShock\\Builds\\Release\\Startup.ini", &data, &size) || !data ||
        !strstr(data, "[Filesystem]") || !strstr(data, "FilePath=")) {
        fprintf(stderr, "startup filesystem config missing\n");
        free(data);
        return 1;
    }
    free(data);
    if (setenv("PORT_PART_GAME", "/storage/emulated/0/BioShock/game", 1) ||
        void_obb_read("/storage/emulated/0/BioShock/game/Builds/Release/Startup.ini", &data, &size) || !data ||
        !strstr(data, "[Filesystem]") || !strstr(data, "FilePath=")) {
        fprintf(stderr, "launcher path alias cannot read startup config\n");
        free(data);
        return 1;
    }
    free(data);
    if (setenv("PORT_PART_GAME", "/data/user/0/org.bioshock.port/files/launcher/mount-bioshock-game", 1) ||
        void_obb_stat("/data/user/0/org.bioshock.port/files/launcher/mount-bioshock-game", &st) || !S_ISDIR(st.st_mode) ||
        void_obb_read("/data/user/0/org.bioshock.port/files/launcher/mount-bioshock-game/Builds/Release/Startup.ini", &data, &size) || !data ||
        !strstr(data, "[Filesystem]") || !strstr(data, "FilePath=") || unsetenv("PORT_PART_GAME")) {
        fprintf(stderr, "mounted sidecar cannot read startup config from OBB\n");
        free(data);
        return 1;
    }
    free(data);
    int found_startup = 0;
    for (unsigned index = 0;; ++index) {
        char name[512];
        int directory = -1;
        int result = void_obb_list("game/Builds/Release", index, name, sizeof name, &directory, &st);
        if (result < 0 || index > 100) {
            fprintf(stderr, "release listing failed at %u\n", index);
            return 1;
        }
        if (result > 0) break;
        if (!directory && !strcasecmp(name, "Startup.ini")) found_startup = 1;
    }
    if (!found_startup) {
        fprintf(stderr, "startup config missing from release listing\n");
        return 1;
    }
    if (void_obb_read("game/Content/FlashMovies/PausePC.swf", &data, &size)) {
        fprintf(stderr, "packed PausePC SWF read failed\n");
        free(data);
        return 1;
    }
    const unsigned char *swf = data;
    if (size < 3 || !data || (swf[0] != 'F' && swf[0] != 'C') || swf[1] != 'W' || swf[2] != 'S') {
        fprintf(stderr, "packed PausePC SWF mismatch\n");
        free(data);
        return 1;
    }
    printf("PausePC SWF: %c%c%c, %zu bytes\n", swf[0], swf[1], swf[2], size);
    free(data);
    if (void_obb_read("images/Bioshock.exe", &data, &size) || size != 9938296 ||
        ((const unsigned char *)data)[0] != 'M' || ((const unsigned char *)data)[1] != 'Z') {
        fprintf(stderr, "original executable mismatch\n");
        free(data);
        return 1;
    }
    free(data);
    const struct { const char *path; uint32_t base; } images[] = {
        {"images/msvcr80.dll", 0x10000000}, {"images/msvcp80.dll", 0x10100000}, {"images/binkw32.dll", 0x18000000}
    };
    for (unsigned index = 0; index < sizeof images / sizeof *images; ++index) {
        if (void_obb_read(images[index].path, &data, &size) || size < 0x40) {
            fprintf(stderr, "AOT image missing: %s\n", images[index].path);
            free(data);
            return 1;
        }
        const unsigned char *bytes = data;
        uint32_t pe = bytes[0x3C] | (uint32_t)bytes[0x3D] << 8 | (uint32_t)bytes[0x3E] << 16 | (uint32_t)bytes[0x3F] << 24;
        if (pe + 0x38 > size) {
            free(data);
            return 1;
        }
        uint32_t base = bytes[pe + 0x34] | (uint32_t)bytes[pe + 0x35] << 8 | (uint32_t)bytes[pe + 0x36] << 16 | (uint32_t)bytes[pe + 0x37] << 24;
        free(data);
        if (base != images[index].base) {
            fprintf(stderr, "wrong preferred base for %s: %08X\n", images[index].path, base);
            return 1;
        }
    }
    if (void_obb_lookup("game/Content/does-not-exist", &file) == 0 ||
        void_obb_read("game/Content/does-not-exist", &data, &size) == 0 ||
        void_obb_stat("game/Content/does-not-exist", &st) == 0) {
        fprintf(stderr, "missing file was reported as present\n");
        close(file.fd);
        return 1;
    }
    void_obb_close();
    puts("PASS OBB direct-read: ZIP64 offsets, case-insensitive paths, localized override, executable CRC, virtual directories, missing files");
    return 0;
}

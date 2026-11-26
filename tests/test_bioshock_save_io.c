/* Run extracted production save/file helpers against a disposable POSIX directory. */
#define _GNU_SOURCE
#define _FILE_OFFSET_BITS 64
#include <assert.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

#define WIN_INVALID_HANDLE UINT32_MAX
#define VOID_OBJ_FILE 1
#define ERROR_ACCESS_DENIED_ 5
#define ERROR_INVALID_HANDLE_ 6
#define ERROR_NOT_ENOUGH_MEMORY_ 8
#define ERROR_ALREADY_EXISTS_ 183
#define WINAPI_EXPORT(dll, name, count) static uint32_t name(void)
static char data_root[1024];
static pthread_mutex_t save_path_lock = PTHREAD_MUTEX_INITIALIZER;
typedef struct {
    int fd, save_error, is_dir, std_stream, packed;
    uint32_t granted_access;
    char *host_path, *pending_save;
} file_t;
typedef struct void_object {
    file_t *data;
    int (*finish)(struct void_object *);
} void_object;
static void_object object;
static int refs, last_error, log_count, fail_sync, fail_rename, short_write, fail_write;
static unsigned begins, ends;
static uint32_t arguments[5];
static unsigned char guest[65536];
#define ARG(i) arguments[i]
#define GPTR(a) (guest + (a))
#define MEM32(a) (*(uint32_t *)(guest + (a)))

static void void_log(const char *format, ...) { (void)format; ++log_count; }
#define VLOG void_log
static int void_machine_is_owned(void) { return 1; }
static void void_blocking_begin(void) { ++begins; }
static void void_blocking_end(void) { ++ends; }
static int file_restore_kept_times(file_t *file) { (void)file; return 0; }
static uint32_t void_errno_to_win32(int error) { return (uint32_t)error; }
static void void_set_last_error(uint32_t error) { last_error = (int)error; }
static void console_write(file_t *file, const uint8_t *bytes, uint32_t count)
{ (void)file; (void)bytes; (void)count; assert(0); }
static uint32_t file_handle(int fd, const char *path, int standard)
{
    (void)standard;
    assert(!refs);
    object.data = calloc(1, sizeof(file_t));
    assert(object.data);
    object.data->fd = fd;
    object.data->host_path = strdup(path);
    object.finish = NULL;
    refs = 1;
    return 1;
}
static void_object *void_handle_get(uint32_t handle, int kind)
{ assert(refs && handle == 1 && kind == VOID_OBJ_FILE); return &object; }
static file_t *file_get(uint32_t handle) { return refs && handle == 1 ? object.data : NULL; }
static uint32_t void_handle_dup(uint32_t handle) { assert(handle == 1 && refs); ++refs; return 1; }
static int void_handle_close(uint32_t handle)
{
    assert(handle == 1 && refs);
    if (--refs) return 1;
    int error = object.finish ? object.finish(&object) : 0;
    if (object.data->fd >= 0) close(object.data->fd);
    free(object.data->pending_save);
    free(object.data->host_path);
    free(object.data);
    object.data = NULL;
    return error ? -error : 1;
}
static int fixture_fsync(int fd)
{ if (fail_sync) { errno = ENOSPC; return -1; } return fsync(fd); }
static int fixture_rename(const char *from, const char *to)
{ if (fail_rename) { errno = EACCES; return -1; } return rename(from, to); }
static ssize_t fixture_write(int fd, const void *bytes, size_t count)
{
    if (fail_write) { errno = ENOSPC; return -1; }
    return write(fd, bytes, short_write && count ? count - 1 : count);
}
#define fsync fixture_fsync
#define rename fixture_rename
#define write fixture_write
#include "save-io-production.inc"
#undef write

static void contents(const char *path, const void *bytes, size_t count)
{
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    assert(fd >= 0 && write(fd, bytes, count) == (ssize_t)count && !close(fd));
}
static void equal_file(const char *path, const void *bytes, size_t count)
{
    unsigned char buffer[65536];
    int fd = open(path, O_RDONLY);
    assert(fd >= 0 && count < sizeof buffer);
    assert(read(fd, buffer, sizeof buffer) == (ssize_t)count);
    assert(!memcmp(buffer, bytes, count) && !close(fd));
}
static char pending[2048];
static void stage(const char *path, const char *parent)
{
    uint32_t error;
    assert(create_save_writer(path, parent, 0x40000000, &error) == 1);
    assert(error == 0 || error == ERROR_ALREADY_EXISTS_);
    assert(object.finish == save_finish);
    strcpy(pending, object.data->pending_save);
    assert(strstr(pending, ".pending") && access(pending, F_OK) == 0);
}
static void guest_write(const void *bytes, uint32_t count, int success)
{
    assert(count + 128 < sizeof guest);
    memcpy(guest + 128, bytes, count);
    arguments[0] = 1; arguments[1] = 128; arguments[2] = count; arguments[3] = 4; arguments[4] = 0;
    assert(WriteFile() == (uint32_t)success && refs == 1);
}
int main(int argc, char **argv)
{
    assert(argc == 3);
    strcpy(data_root, argv[1]);
    FILE *fixture = fopen(argv[2], "rb");
    unsigned char valid[65536];
    assert(fixture);
    size_t size = fread(valid, 1, sizeof valid, fixture);
    assert(feof(fixture) && size && !fclose(fixture));
    char documents[2048], bioshock[2048], parent[2048], target[2048];
    assert(snprintf(documents, sizeof documents, "%s/Documents", data_root) > 0);
    assert(snprintf(bioshock, sizeof bioshock, "%s/Bioshock", documents) > 0);
    assert(snprintf(parent, sizeof parent, "%s/SaveGames", bioshock) > 0);
    assert(snprintf(target, sizeof target, "%s/QuickSave.bsb", parent) > 0);
    assert(!mkdir(documents, 0700) && !mkdir(bioshock, 0700) && !mkdir(parent, 0700));
    assert(profile_save_path(target));
    assert(!profile_save_path("/other/Documents/Bioshock/SaveGames/QuickSave.bsb"));
    char nested[2048];
    assert(snprintf(nested, sizeof nested, "%s/damaged/QuickSave.bsb", parent) > 0);
    assert(!profile_save_path(nested));
    assert(save_guard_enabled());
    assert(!setenv("BIOSHOCK_SAVE_GUARD", "0", 1) && !save_guard_enabled());
    assert(!unsetenv("BIOSHOCK_SAVE_GUARD") && save_guard_enabled());

    contents(target, valid, size);
    assert(!save_skip_damaged(target));
    equal_file(target, valid, size);
    unsigned char incoming[65536];
    memcpy(incoming, valid, size);
    incoming[12] ^= 1; /* A different valid timestamp proves successful replacement. */
    for (int failure = 0; failure < 6; ++failure) {
        stage(target, parent);
        equal_file(target, valid, size); /* Old committed save survives during the write. */
        short_write = failure == 1; fail_write = failure == 2;
        guest_write(incoming, failure == 0 ? 10 : (uint32_t)size, !fail_write);
        short_write = fail_write = 0;
        fail_sync = failure == 3; fail_rename = failure == 4;
        int result = void_handle_close(1);
        fail_sync = fail_rename = 0;
        assert(begins == ends);
        if (failure < 5) {
            equal_file(target, valid, size);
            assert(result < 0 && access(pending, F_OK) == 0); /* No lost incomplete data. */
        } else {
            equal_file(target, incoming, size);
            assert(result == 1 && access(pending, F_OK) < 0 && errno == ENOENT);
        }
    }
    /* Colliding quarantine names must preserve both damaged files. */
    contents(target, "bad", 3);
    assert(save_skip_damaged(target) && access(target, F_OK) < 0);
    char damaged[2048];
    assert(snprintf(damaged, sizeof damaged, "%s/damaged", parent) > 0);
    DIR *directory = opendir(damaged);
    assert(directory);
    struct dirent *entry;
    int files = 0;
    while ((entry = readdir(directory))) if (entry->d_name[0] != '.') ++files;
    closedir(directory);
    assert(files == 1);
    contents(target, "worse", 5);
    assert(save_skip_damaged(target) && access(target, F_OK) < 0);
    directory = opendir(damaged);
    assert(directory);
    files = 0;
    while ((entry = readdir(directory))) if (entry->d_name[0] != '.') ++files;
    closedir(directory);
    assert(files == 2 && log_count >= 8);
    puts("PASS production save I/O: atomic visibility, rejected partial/short/failed writes, sync/rename failures, quarantine collisions, profile scope and switch");
    return 0;
}

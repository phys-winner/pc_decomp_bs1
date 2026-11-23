#ifndef BIOSHOCK_DATA_OBB_H
#define BIOSHOCK_DATA_OBB_H

#include <stddef.h>
#include <stdint.h>
#include <sys/stat.h>

typedef struct {
    int fd;
    uint64_t bias;
    uint64_t size;
    uint64_t archive_offset;
    uint32_t crc32;
    const char *path;
} obb_file;

#ifdef __ANDROID__
#include <jni.h>
int void_obb_init_assets(JNIEnv *env, jobject assets);
#endif
int void_obb_init_external(const char *directory);
int void_obb_ready(void);
int void_obb_lookup(const char *path, obb_file *out);
int void_obb_stat(const char *path, struct stat *out);
int void_obb_list(const char *directory, unsigned index, char *name, size_t capacity, int *is_directory, struct stat *st);
int void_obb_read(const char *path, void **data, size_t *size);
void void_obb_close(void);

#endif

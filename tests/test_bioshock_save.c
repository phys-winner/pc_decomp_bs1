#define _FILE_OFFSET_BITS 64
#include <stdint.h>
#include <stdio.h>
#include <sys/stat.h>

const char *void_bioshock_save_damage(FILE *, uint64_t, int *);

int main(int argc, char **argv)
{
    for (int i = 1; i < argc; ++i) {
        FILE *stream = fopen(argv[i], "rb");
        struct stat st;
        if (!stream || fstat(fileno(stream), &st)) {
            perror(argv[i]);
            return 1;
        }
        int error;
        const char *damage = void_bioshock_save_damage(stream, (uint64_t)st.st_size, &error);
        if (error || ferror(stream)) {
            fprintf(stderr, "%s: host read error %d\n", argv[i], error);
            return 1;
        }
        puts(damage ? damage : "valid");
        fclose(stream);
    }
    return 0;
}

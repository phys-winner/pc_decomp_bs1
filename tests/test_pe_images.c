/* The production image source over the installed game: every image must match generated/input. */
#include "../android/host/pe_images.c"
#include <stdarg.h>

static const char *game_root, *prepared_root;
static int damage;

void void_log(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    putchar('\n');
}

static uint8_t *slurp(const char *root, const char *relative, size_t *size)
{
    char path[2048];
    snprintf(path, sizeof path, "%s/%s", root, relative);
    FILE *f = fopen(path, "rb");
    if (!f) { errno = ENOENT; return NULL; }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *data = (uint8_t *)malloc(n ? (size_t)n : 1);
    if (!data || fread(data, 1, (size_t)n, f) != (size_t)n) { fclose(f); free(data); errno = EIO; return NULL; }
    fclose(f);
    *size = (size_t)n;
    return data;
}

uint8_t *void_files_read_game(const char *relative, size_t *size)
{
    uint8_t *data = slurp(game_root, relative, size);
    if (data && damage) { data[*size / 2] ^= 0x01u; damage = 0; }
    return data;
}

static uint8_t *linked[2];
const uint8_t *void_pe_builtin(const char *file, size_t *size)
{
    int slot = !strcmp(file, "msvcr80.dll") ? 0 : !strcmp(file, "msvcp80.dll") ? 1 : -1;
    if (slot < 0) return NULL;
    free(linked[slot]);
    linked[slot] = slurp(prepared_root, file, size);
    return linked[slot];
}

uLong crc32(uLong crc, const Bytef *buf, uInt len)
{
    uint32_t c = ~(uint32_t)crc;
    for (uInt i = 0; i < len; ++i) {
        c ^= buf[i];
        for (int k = 0; k < 8; ++k) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1u)));
    }
    return ~c;
}

int main(int argc, char **argv)
{
    if (argc != 4) { fprintf(stderr, "usage: game-root prepared-root module-list\n"); return 2; }
    game_root = argv[1];
    prepared_root = argv[2];
    FILE *list = fopen(argv[3], "r");
    if (!list) return 2;
    char file[256];
    unsigned base, checked = 0;
    while (fscanf(list, "%255s %x", file, &base) == 2) {
        size_t size = 0, expected_size = 0;
        uint8_t *image = void_pe_image(file, base, &size);
        uint8_t *expected = slurp(prepared_root, file, &expected_size);
        if (!image || !expected || size != expected_size || memcmp(image, expected, size)) {
            printf("FAIL %s\n", file);
            return 1;
        }
        printf("equal %s %zu\n", file, size);
        free(image);
        free(expected);
        ++checked;
    }
    fclose(list);
    size_t size = 0;
    damage = 1;
    if (void_pe_image("binkw32.dll", 0x18000000u, &size) || errno != EILSEQ) { printf("FAIL damaged file accepted\n"); return 1; }
    game_root = prepared_root;
    if (void_pe_image("Bioshock.exe", 0x10900000u, &size) || errno != ENOENT) { printf("FAIL missing file\n"); return 1; }
    if (void_pe_image("unknown.dll", 0x30000000u, &size) || errno != ENOENT) { printf("FAIL unknown module\n"); return 1; }
    printf("PASS %u images equal to generated/input; damaged, missing and unknown files refused\n", checked);
    return checked == sizeof images / sizeof images[0] ? 0 : 1;
}

#include "../android/host/data_obb.h"

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "../android/host/data_obb.c"

void void_log(const char *format, ...)
{
    (void)format;
}

int main(void)
{
    const uint8_t local_only[] = {1, 0, 8, 0, 0, 0x40, 0, 0, 0, 0, 0, 0};
    uint8_t all_fields[28] = {1, 0, 24, 0};
    const uint64_t fields[] = {0x200000, 0x200000, 0x80000000ull};
    for (unsigned field = 0; field < 3; ++field) {
        for (unsigned byte = 0; byte < 8; ++byte) all_fields[4 + field * 8 + byte] = (uint8_t)(fields[field] >> (byte * 8));
    }
    size_t position = 0;
    uint64_t value = 0;
    if (zip64_value(local_only, sizeof local_only, &position, &value) || value != 0x4000 || position != 8) return 1;
    position = 0;
    if (zip64_value(all_fields, sizeof all_fields, &position, &value) || value != 0x200000 || position != 8) {
        fprintf(stderr, "size: value=%llu position=%zu\n", (unsigned long long)value, position);
        return 2;
    }
    if (zip64_value(all_fields, sizeof all_fields, &position, &value) || value != 0x200000 || position != 16) {
        fprintf(stderr, "compressed: value=%llu position=%zu\n", (unsigned long long)value, position);
        return 3;
    }
    if (zip64_value(all_fields, sizeof all_fields, &position, &value) || value != 0x80000000ull || position != 24) return 4;
    const uint8_t truncated[] = {1, 0, 4, 0, 0, 0x40, 0, 0};
    position = 0;
    if (!zip64_value(truncated, sizeof truncated, &position, &value)) return 5;
    const uint8_t wrong_field[] = {9, 0, 8, 0, 0, 0x40, 0, 0, 0, 0, 0, 0};
    position = 0;
    if (!zip64_value(wrong_field, sizeof wrong_field, &position, &value)) return 6;
    puts("PASS ZIP64 local-only offset, size+compressed+offset, truncated and unknown extra fields");
    return 0;
}

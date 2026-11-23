#include "../android/host/host.h"
#include <assert.h>
#include <unistd.h>
#include "sub_10EA6529.inc"

const recomp_dispatch_entry_t recomp_dispatch_table[] = {{0x10EA6529u, sub_10EA6529}};
const uint32_t recomp_dispatch_count = 1;

void void_process_exit(uint32_t code, int detach) { (void)detach; _exit((int)code); }
void void_audio_output(const int16_t *frames, uint32_t count) { (void)frames; (void)count; }
void *ut3_dxvk_native_window(void) { return NULL; }
int void_android_render_ready(void) { return 0; }
int void_android_display_size(int *width, int *height) { *width = 1920; *height = 1080; return 1; }

static void decode_block(uint32_t source, uint32_t destination)
{
    uint32_t args[] = {source, destination, 1, 32, 64, 1};
    assert(void_call_guest(0x10EA6529u, 6, args, 0) == 0);
}

int main(int argc, char **argv)
{
    assert(argc == 4);
    FILE *bank = fopen(argv[1], "rb"), *expected_file = fopen(argv[2], "rb");
    assert(bank && expected_file);
    uint8_t compressed[72], expected[256];
    assert(fseek(bank, 2002976, SEEK_SET) == 0);
    assert(fread(compressed, 1, sizeof compressed, bank) == sizeof compressed);
    assert(fread(expected, 1, sizeof expected, expected_file) == sizeof expected);
    fclose(bank);
    fclose(expected_file);
    assert(void_guest_reserve());
    void_threads_init();
    void_dispatch_init();
    void_machine_enter();
    assert(void_modules_init(argv[3]));
    for (unsigned channel = 0; channel < 2; ++channel) {
        uint32_t source = void_galloc(36), destination = void_galloc(128);
        memcpy(GPTR(source), compressed + channel * 4, 4);
        for (unsigned group = 0; group < 8; ++group)
            memcpy(GPTR(source + 4 + group * 4), compressed + 8 + group * 8 + channel * 4, 4);
        decode_block(source, destination);
        for (unsigned frame = 0; frame < 64; ++frame) {
            int16_t actual = (int16_t)MEM16(destination + frame * 2);
            int16_t reference;
            memcpy(&reference, expected + (frame * 2 + channel) * 2, 2);
            if (actual != reference)
                fprintf(stderr, "channel=%u frame=%u actual=%d expected=%d\n", channel, frame, actual, reference);
            assert(actual == reference);
        }
        void_gfree(source);
        void_gfree(destination);
    }
    puts("PASS original ARM64 FMOD IMA decoder matches 128 vgmstream PCM samples");
    return 0;
}

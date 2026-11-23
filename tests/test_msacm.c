#include "../android/host/msacm.c"
#include <assert.h>
#include <unistd.h>

void void_process_exit(uint32_t code, int detach) { (void)detach; _exit((int)code); }

static uint32_t api(const char *name, unsigned count, const uint32_t *args)
{
    const void_shim *shim = void_shim_find("msacm32.dll", name, 0);
    assert(shim);
    uint32_t before = g_esp;
    uint32_t result = void_call_guest(void_thunk(shim->fn, shim->name), (int)count, args, 1);
    assert(g_esp == before);
    return result;
}

static void wave_format(uint32_t address, uint32_t tag, uint32_t rate, uint32_t samples)
{
    memset(GPTR(address), 0, 64);
    MEM16(address) = (uint16_t)tag;
    MEM16(address + 2) = 1;
    MEM32(address + 4) = rate;
    MEM32(address + 8) = rate * 256 / samples;
    MEM16(address + 12) = 256;
    MEM16(address + 14) = 4;
    MEM16(address + 16) = tag == 2 ? 32 : 2;
    MEM16(address + 18) = (uint16_t)samples;
    if (tag == 2) {
        MEM16(address + 20) = 7;
        for (unsigned i = 0; i < 7; ++i) {
            MEM16(address + 22 + i * 4) = (uint16_t)ms_coeff1[i];
            MEM16(address + 24 + i * 4) = (uint16_t)ms_coeff2[i];
        }
    }
}

static uint32_t open_stream(uint32_t tag)
{
    uint32_t source = void_galloc(64), destination = void_galloc(18), stream = void_galloc(4);
    fprintf(stderr, "open source=%08X destination=%08X stream=%08X\n", source, destination, stream);
    wave_format(source, tag, 22050, tag == 2 ? 500 : 505);
    memset(GPTR(destination), 0, 18);
    MEM16(destination) = 1;
    uint32_t args[5] = {0, source, destination, 18, 0x10000};
    assert(api("acmFormatSuggest", 5, args) == 0);
    assert(MEM16(destination) == 1 && MEM16(destination + 2) == 1 && MEM32(destination + 4) == 22050);
    assert(MEM16(destination + 12) == 2 && MEM16(destination + 14) == 16);
    uint32_t open[8] = {stream, 0, source, destination, 0, 0, 0, 4};
    assert(api("acmStreamOpen", 8, open) == 0 && MEM32(stream));
    uint32_t size_args[4] = {MEM32(stream), 256, destination, 0};
    assert(api("acmStreamSize", 4, size_args) == 0 && MEM32(destination) == (tag == 2 ? 1000u : 1010u));
    return MEM32(stream);
}

static void convert_stream(uint32_t stream, uint32_t encoded, uint32_t decoded)
{
    uint32_t expected_bytes = stream_find(stream)->tag == 2 ? 1000 : 1010;
    uint32_t header = void_galloc(84);
    memset(GPTR(header), 0, 84);
    MEM32(header) = 84;
    MEM32(header + 12) = encoded;
    MEM32(header + 16) = 256;
    MEM32(header + 28) = decoded;
    MEM32(header + 32) = expected_bytes;
    uint32_t args[3] = {stream, header, 0};
    assert(api("acmStreamPrepareHeader", 3, args) == 0);
    args[2] = 4;
    assert(api("acmStreamConvert", 3, args) == 0);
    assert(MEM32(header + 20) == 256 && MEM32(header + 36) == expected_bytes);
    args[2] = 0;
    assert(api("acmStreamUnprepareHeader", 3, args) == 0);
    args[2] = 4;
    assert(api("acmStreamConvert", 3, args) == ACMERR_UNPREPARED);
    uint32_t close[2] = {stream, 0};
    assert(api("acmStreamClose", 2, close) == 0);
    assert(api("acmStreamClose", 2, close) == 10);
}

static void test_ima(void)
{
    uint32_t encoded = void_galloc(256), decoded = void_galloc(1010);
    for (unsigned i = 0; i < 256; ++i) MEM8(encoded + i) = 0x11;
    MEM16(encoded) = 0;
    MEM8(encoded + 2) = 0;
    convert_stream(open_stream(0x11), encoded, decoded);
    for (unsigned i = 0; i < 9; ++i) fprintf(stderr, "ima[%u]=%d\n", i, (int16_t)MEM16(decoded + i * 2));
    const int16_t expected[] = {0,2,4,6,8,10,12,14,16};
    for (unsigned i = 0; i < sizeof expected / sizeof *expected; ++i)
        assert((int16_t)MEM16(decoded + i * 2) == expected[i]);
    for (unsigned i = 0; i < 256; ++i) MEM8(encoded + i) = 0xFF;
    MEM16(encoded) = 0;
    MEM8(encoded + 2) = 0;
    convert_stream(open_stream(0x11), encoded, decoded);
    for (unsigned i = 0; i < 9; ++i) fprintf(stderr, "negative[%u]=%d\n", i, (int16_t)MEM16(decoded + i * 2));
    const int16_t negative[] = {0,-13,-43,-106,-242,-536,-1167,-2524,-5434};
    for (unsigned i = 0; i < sizeof negative / sizeof *negative; ++i)
        assert((int16_t)MEM16(decoded + i * 2) == negative[i]);
}

static void test_ms(void)
{
    uint32_t encoded = void_galloc(256), decoded = void_galloc(1010);
    MEM8(encoded) = 0;
    MEM16(encoded + 1) = 16;
    MEM16(encoded + 3) = 1000;
    MEM16(encoded + 5) = 2000;
    MEM8(encoded + 7) = 1;
    MEM8(encoded + 8) = 0xF0;
    for (unsigned i = 0; i < 16; ++i) fprintf(stderr, "encoded[%u]=%02X\n", i, MEM8(encoded + i));
    convert_stream(open_stream(2), encoded, decoded);
    for (unsigned i = 0; i < 6; ++i) fprintf(stderr, "ms[%u]=%d\n", i, (int16_t)MEM16(decoded + i * 2));
    const int16_t expected[] = {2000,1000,1000,1016,1000,1000};
    for (unsigned i = 0; i < sizeof expected / sizeof *expected; ++i)
        assert((int16_t)MEM16(decoded + i * 2) == expected[i]);
    assert(MEM8(encoded + 7) == 1 && MEM8(encoded + 8) == 0xF0);
}

static void test_ms_stereo(void)
{
    uint32_t source = void_galloc(64), destination = void_galloc(18), stream = void_galloc(4);
    wave_format(source, 2, 22050, 244);
    MEM16(source + 2) = 2;
    MEM32(source + 8) = 22050 * 256 / 244;
    memset(GPTR(destination), 0, 18);
    MEM16(destination) = 1;
    MEM16(destination + 2) = 2;
    MEM32(destination + 4) = 22050;
    uint32_t suggest[5] = {0, source, destination, 18, 0x10000};
    assert(api("acmFormatSuggest", 5, suggest) == 0);
    assert(MEM16(destination + 2) == 2 && MEM16(destination + 12) == 4);
    uint32_t open[8] = {stream, 0, source, destination, 0, 0, 0, 4};
    assert(api("acmStreamOpen", 8, open) == 0 && MEM32(stream));
    uint32_t size_args[4] = {MEM32(stream), 256, destination, 0};
    assert(api("acmStreamSize", 4, size_args) == 0 && MEM32(destination) == 976);
    uint32_t encoded = void_galloc(256), decoded = void_galloc(976);
    for (unsigned channel = 0; channel < 2; ++channel) {
        MEM8(encoded + channel) = 0;
        MEM16(encoded + 2 + channel * 2) = 16;
        MEM16(encoded + 6 + channel * 2) = channel ? (uint16_t)-1000 : 1000;
        MEM16(encoded + 10 + channel * 2) = 0;
    }
    MEM8(encoded + 14) = 0x11;
    MEM8(encoded + 15) = 0;
    MEM8(encoded + 16) = 0x22;
    MEM8(encoded + 17) = 0x88;
    MEM8(encoded + 18) = 0xFF;
    uint8_t original[21];
    memcpy(original, GPTR(encoded), sizeof original);
    uint32_t header = void_galloc(84);
    memset(GPTR(header), 0, 84);
    MEM32(header) = 84;
    MEM32(header + 12) = encoded;
    MEM32(header + 16) = 256;
    MEM32(header + 28) = decoded;
    MEM32(header + 32) = 976;
    uint32_t args[3] = {MEM32(stream), header, 0};
    assert(api("acmStreamPrepareHeader", 3, args) == 0);
    args[2] = 4;
    assert(api("acmStreamConvert", 3, args) == 0);
    assert(MEM32(header + 20) == 256 && MEM32(header + 36) == 976);
    assert(!memcmp(original, GPTR(encoded), sizeof original));
    const int16_t expected[] = {0,0,1000,-1000,1016,-984,1016,-984,1048,-952,920,-1080,872,-1128};
    for (unsigned i = 0; i < sizeof expected / sizeof *expected; ++i) {
        if ((int16_t)MEM16(decoded + i * 2) != expected[i])
            fprintf(stderr, "stereo[%u]=%d expected=%d\n", i, (int16_t)MEM16(decoded + i * 2), expected[i]);
        assert((int16_t)MEM16(decoded + i * 2) == expected[i]);
    }
    args[2] = 0;
    assert(api("acmStreamUnprepareHeader", 3, args) == 0);
    args[2] = 4;
    assert(api("acmStreamConvert", 3, args) == ACMERR_UNPREPARED);
    uint32_t close[2] = {MEM32(stream), 0};
    assert(api("acmStreamClose", 2, close) == 0);
}

int main(void)
{
    assert(void_guest_reserve());
    void_threads_init();
    void_dispatch_init();
    void_machine_enter();
    test_ima();
    test_ms();
    test_ms_stereo();
    uint32_t source = void_galloc(64), destination = void_galloc(18);
    memset(GPTR(source), 0, 64);
    memset(GPTR(destination), 0, 18);
    MEM16(source) = 0x55;
    MEM16(destination) = 1;
    uint32_t suggest[5] = {0, source, destination, 18, 0x10000};
    assert(api("acmFormatSuggest", 5, suggest) == ACMERR_NOTPOSSIBLE);
    uint32_t invalid[4] = {0, 256, destination, 0};
    assert(api("acmStreamSize", 4, invalid) == 10);
    puts("PASS MSACM IMA/Microsoft ADPCM PCM vectors, format-correct blocks, const input, prepare/convert/unprepare and error paths");
    return 0;
}

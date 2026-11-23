#include "../android/host/dsound.c"
#include <assert.h>
#include <unistd.h>

void void_process_exit(uint32_t code, int detach) { (void)detach; _exit((int)code); }

static uint32_t method(void_fn fn, unsigned count, const uint32_t *args)
{
    uint32_t before = g_esp;
    uint32_t result = void_call_guest(void_thunk(fn, "dsound-mix-test"), (int)count, args, 1);
    assert(g_esp == before);
    return result;
}

static void check_frame(const float *mix, uint32_t frame, int16_t expected)
{
    float normalized = (float)expected / 32768.0f;
    assert(fabsf(mix[frame * 2] - normalized) < 0.00001f);
    assert(fabsf(mix[frame * 2 + 1] + normalized) < 0.00001f);
}

int main(void)
{
    assert(void_guest_reserve());
    void_threads_init();
    void_dispatch_init();
    void_machine_enter();

    uint32_t device = void_galloc(4);
    assert(ds_create(0, 0, device) == DS_OK && MEM32(device));
    uint32_t desc = void_galloc(20), format = void_galloc(18), out = void_galloc(20);
    MEM16(format) = 1;
    MEM16(format + 2) = 2;
    MEM32(format + 4) = MIX_RATE;
    MEM32(format + 8) = MIX_RATE * 4;
    MEM16(format + 12) = 4;
    MEM16(format + 14) = 16;
    MEM16(format + 16) = 0;
    MEM32(desc) = 20;
    MEM32(desc + 4) = 0x80;
    MEM32(desc + 8) = 4096;
    MEM32(desc + 12) = 0;
    MEM32(desc + 16) = format;
    uint32_t create[] = {MEM32(device), desc, out, 0};
    assert(method(ds_CreateSoundBuffer, 4, create) == DS_OK);
    dsbuf_t *buffer = (dsbuf_t *)void_com_host(MEM32(out));
    assert(buffer && buffer->size == 4096 && buffer->block == 4);

    uint32_t lock[] = {MEM32(out), 0, 4096, out + 4, out + 8, out + 12, out + 16, 0};
    assert(method(dsb_Lock, 8, lock) == DS_OK);
    assert(MEM32(out + 8) == 4096 && MEM32(out + 12) == 0 && MEM32(out + 16) == 0);
    for (uint32_t frame = 0; frame < 1024; ++frame) {
        MEM16(MEM32(out + 4) + frame * 4) = (uint16_t)(int16_t)(frame + 1);
        MEM16(MEM32(out + 4) + frame * 4 + 2) = (uint16_t)(int16_t)-(frame + 1);
    }
    uint32_t unlock[] = {MEM32(out), MEM32(out + 4), 4096, 0, 0};
    assert(method(dsb_Unlock, 5, unlock) == DS_OK);
    uint32_t play[] = {MEM32(out), 0, 0, 0};
    assert(method(dsb_Play, 4, play) == DS_OK);
    float mix[2 * MIX_CHUNK];
    mix_chunk(mix, MIX_CHUNK);
    for (uint32_t frame = 0; frame < MIX_CHUNK; ++frame) check_frame(mix, frame, (int16_t)(frame + 1));

    uint32_t position[] = {MEM32(out), 4080};
    assert(method(dsb_SetCurrentPosition, 2, position) == DS_OK);
    play[3] = DSBPLAY_LOOPING;
    assert(method(dsb_Play, 4, play) == DS_OK);
    mix_chunk(mix, MIX_CHUNK);
    for (uint32_t frame = 0; frame < MIX_CHUNK; ++frame)
        check_frame(mix, frame, (int16_t)(frame < 4 ? 1021 + frame : frame - 3));
    assert(buffer->playing && buffer->looping && buffer->play_pos == 1904);

    assert(method(dsb_SetCurrentPosition, 2, position) == DS_OK);
    play[3] = 0;
    assert(method(dsb_Play, 4, play) == DS_OK);
    mix_chunk(mix, MIX_CHUNK);
    for (uint32_t frame = 0; frame < MIX_CHUNK; ++frame) {
        if (frame < 4) check_frame(mix, frame, (int16_t)(1021 + frame));
        else assert(mix[frame * 2] == 0.0f && mix[frame * 2 + 1] == 0.0f);
    }
    assert(!buffer->playing && buffer->play_pos == 0);
    puts("PASS DirectSound stereo PCM, loop boundary and one-shot stop");
    return 0;
}

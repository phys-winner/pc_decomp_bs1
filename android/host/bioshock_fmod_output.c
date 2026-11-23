/* FMOD Ex's output without DirectSound: a native output plugin over AAudio.
 *
 * BioShock plays every game sound through FMOD Ex, linked into Bioshock.exe.
 * FMOD mixes all channels itself (software mixing, its DSP graph with the
 * native Freeverb of bioshock_audio_dsp.c). Its DirectSound output only
 * copied that mix into a looping 16 KiB buffer (4 x 1024 frames, 44.1 kHz,
 * 93 ms) from the "FMOD mixer thread", which polled the play cursor every
 * 7.7 ms; dsound.c emulated the buffer and a host mixer thread resampled it
 * to 48 kHz in 10 ms chunks for a blocking AAudio writer (android_main.c).
 *
 * Here the description of that output (FMOD's FMOD_OUTPUT_DESCRIPTION for
 * FMOD_OUTPUTTYPE_DSOUND, 0x90 bytes at 0x111B5628, filled by 0x10EAC923)
 * describes a native, non-polled plugin instead (the seam is in
 * tools/build_android.py). FMOD then creates a plain Output object, calls
 * init with pointers to its software rate and format, which the plugin sets
 * to the device's rate and PCM16, and starts no thread of its own. The plugin
 * starts one FMOD thread (FMOD's own Thread class, priority 3 as FMOD's mixer
 * thread), whose callback sleeps with the machine released until the AAudio
 * callback of audio_native.c wants data, then reads FMOD's mix through the
 * readfrommixer function FMOD put into the output state, straight into the
 * FIFO that the callback drains. No resampler, no second mixer, no emulated
 * buffer.
 *
 *   description +0x0C getnumdrivers(state, int *)                   -> 1
 *               +0x10 getdrivername(state, id, char *, int)         -> "Android AAudio"
 *               +0x18 init(state, driver, flags, int *rate, channels, int *format, dsplen, dspnum, extra)
 *               +0x1C close(state)
 *               +0x20 update(state)                                 -> nothing (FMOD mixes 3D itself)
 *               +0x24 gethandle(state, void **)                     -> NULL
 *               +0x40 type 6 (FMOD_OUTPUTTYPE_DSOUND: autodetect and setOutput(DSOUND) pick it)
 *               +0x50 getdrivercapsex(state, id, caps *, minf *, maxf *, speakermode *)
 *               +0x58 start(state), +0x5C stop(state)
 * The hardware channel pools of the DirectSound output (32 2D, 32 3D) are not
 * provided; FMOD creates FMOD_HARDWARE sounds in software when the output has
 * no createsample (0x10E57880). Recording is not provided either (no capture).
 *
 * BIOSHOCK_FMOD_OUTPUT=dsound (perf-flags.env or the environment) keeps the
 * original description, so FMOD uses DirectSound on dsound.c as before.
 * BIOSHOCK_FMOD_RATE=<Hz> asks FMOD to mix at that rate instead of the
 * device's (AAudio then resamples). */
#include "host.h"
#include <stdlib.h>
#include <string.h>

#define FMOD_DSOUND_DESCRIPTION 0x111B5628u   /* FMOD_OUTPUT_DESCRIPTION_EX, 0x90 bytes */
#define FMOD_DESCRIPTION_BYTES  0x90u
#define FMOD_READ_FROM_MIXER    0x10E8CE53u   /* stdcall (state, buffer, frames): Output::mix */
#define FMOD_THREAD_CTOR        0x10E4B334u   /* thiscall FMOD::Thread::Thread() */
#define FMOD_THREAD_INIT        0x10E4B36Fu   /* thiscall (name, callback, param, priority, 0, stack, event, period) */
#define FMOD_THREAD_CLOSE       0x10E4B425u   /* thiscall: clears the run flag, waits for the exit */
#define FMOD_THREAD_BYTES       0x140u        /* fields up to +0x120 */
#define FMOD_OUTPUTTYPE_DSOUND  6u
#define FMOD_FORMAT_PCM16       2u
#define FMOD_SPEAKERMODE_STEREO 2u
#define FMOD_CAPS_HARDWARE_EMULATED 0x2u
#define FMOD_CAPS_FORMAT_PCM16  0x10u
#define FMOD_OK                 0u
#define FMOD_ERR_INVALID_PARAM  37u
#define FMOD_ERR_OUTPUT_INIT    59u           /* FMOD_ERR_OUTPUT_INIT in this build's numbering */
#define MAX_CHANNELS            8u
#define SCRATCH_FRAMES          4096u

static int enabled = -1;
static uint32_t channels = 2, scratch, thread_object, out_rate;
static uint64_t renders, rendered_frames;

static int native_enabled(void)
{
    if (enabled < 0) {
        const char *mode = getenv("BIOSHOCK_FMOD_OUTPUT");
        enabled = !(mode && !strcmp(mode, "dsound"));
        void_log("[fmod-output] %s", enabled ? "native AAudio output (DirectSound output replaced)"
                                             : "BIOSHOCK_FMOD_OUTPUT=dsound: the original DirectSound output");
    }
    return enabled;
}

/* FMOD's speaker order FL FR C LFE BL BR SL SR to stereo; LFE is left out. */
static void downmix(int16_t *out, const int16_t *in, uint32_t frames, uint32_t count)
{
    for (uint32_t i = 0; i < frames; ++i, in += count) {
        float l, r;
        if (count == 1) l = r = in[0];
        else {
            l = in[0];
            r = in[1];
            if (count > 2) { l += 0.7071f * in[2]; r += 0.7071f * in[2]; }
            for (uint32_t c = 4; c + 1 < count; c += 2) { l += 0.7071f * in[c]; r += 0.7071f * in[c + 1]; }
        }
        out[2 * i] = (int16_t)(l > 32767.0f ? 32767 : l < -32768.0f ? -32768 : (int)l);
        out[2 * i + 1] = (int16_t)(r > 32767.0f ? 32767 : r < -32768.0f ? -32768 : (int)r);
    }
}

/* The producer: one call per turn of FMOD's thread loop (cdecl, the output
 * state as its parameter; the thread has no period, so this wait paces it). */
static void producer_turn(void)
{
    uint32_t state = ARG(0);
    if (void_audio_wait(20)) {
        for (uint32_t want = void_audio_want(); want;) {
            uint32_t at, frames = void_audio_region(&at, want);
            if (!frames) break;
            if (channels == 2) {
                uint32_t args[3] = { state, at, frames };
                void_call_guest(FMOD_READ_FROM_MIXER, 3, args, 1);
            } else {
                uint32_t done = 0;
                while (done < frames) {
                    uint32_t n = frames - done < SCRATCH_FRAMES ? frames - done : SCRATCH_FRAMES;
                    uint32_t args[3] = { state, scratch, n };
                    void_call_guest(FMOD_READ_FROM_MIXER, 3, args, 1);
                    downmix((int16_t *)GPTR(at + done * 4u), (const int16_t *)GPTR(scratch), n, channels);
                    done += n;
                }
            }
            void_audio_commit(frames);
            ++renders;
            rendered_frames += frames;
            want -= frames;
        }
    }
    g_eax = 0;
    g_esp += 4;
}

static void out_getnumdrivers(void)
{
    uint32_t count = ARG(1);
    if (count) MEM32(count) = 1;
    g_eax = count ? FMOD_OK : FMOD_ERR_INVALID_PARAM;
    g_esp += 12;
}

static void out_getdrivername(void)
{
    static const char name[] = "Android AAudio";
    uint32_t id = ARG(1), text = ARG(2);
    int32_t size = (int32_t)ARG(3);
    if (id || !text || size < 1) { g_eax = FMOD_ERR_INVALID_PARAM; g_esp += 20; return; }
    uint32_t n = (uint32_t)size - 1u < sizeof name - 1u ? (uint32_t)size - 1u : (uint32_t)sizeof name - 1u;
    memcpy(GPTR(text), name, n);
    MEM8(text + n) = 0;
    g_eax = FMOD_OK;
    g_esp += 20;
}

/* As the DirectSound output reported for dsound.c's device: no hardware voices
 * (FMOD mixes everything), 100..200000 Hz, stereo. */
static void out_getdrivercapsex(void)
{
    uint32_t id = ARG(1), caps = ARG(2), minf = ARG(3), maxf = ARG(4), mode = ARG(5);
    if (id) { g_eax = FMOD_ERR_INVALID_PARAM; g_esp += 28; return; }
    if (caps) MEM32(caps) |= FMOD_CAPS_HARDWARE_EMULATED | FMOD_CAPS_FORMAT_PCM16;
    if (minf) MEM32(minf) = 100;
    if (maxf) MEM32(maxf) = 200000;
    if (mode) MEM32(mode) = FMOD_SPEAKERMODE_STEREO;
    g_eax = FMOD_OK;
    g_esp += 28;
}

static void out_init(void)
{
    uint32_t state = ARG(0), rate = ARG(3), count = ARG(4), format = ARG(5), dsplen = ARG(6), dspnum = ARG(7);
    if (!rate || !format || !count || count > MAX_CHANNELS) {
        void_log("[fmod-output] init refused: rate %08X format %08X channels %u", rate, format, count);
        g_eax = FMOD_ERR_OUTPUT_INIT;
        g_esp += 40;
        return;
    }
    uint32_t asked = MEM32(rate), low = 8000, high = 96000;
    const char *forced = getenv("BIOSHOCK_FMOD_RATE");
    if (forced && atoi(forced) >= 8000 && atoi(forced) <= 96000) low = high = (uint32_t)atoi(forced);
    if (count != 2 && !scratch) scratch = void_galloc(SCRATCH_FRAMES * MAX_CHANNELS * 2u);
    static int log_started;
    if (!log_started) { log_started = 1; void_audio_log_start(); }
    out_rate = void_audio_open(low, high);
    MEM32(rate) = out_rate;
    MEM32(format) = FMOD_FORMAT_PCM16;
    MEM32(state) = 0;                   /* plugindata: one FMOD system, the state is here */
    channels = count;
    renders = rendered_frames = 0;
    void_log("[fmod-output] init: FMOD mixes %u channels at %u Hz (it asked %u Hz), PCM16, DSP %u x %u frames, "
             "no DirectSound; thread %u", count, out_rate, asked, dsplen, dspnum, void_thread_id());
    g_eax = FMOD_OK;
    g_esp += 40;
}

static void out_start(void)
{
    uint32_t state = ARG(0);
    static uint32_t turn, name;
    if (!turn) turn = void_thunk(producer_turn, "fmod_output_producer_turn");
    if (!name) name = void_gstrdup("FMOD AAudio output");
    uint32_t result = FMOD_OK;
    if (!thread_object) {
        uint32_t object = void_galloc(FMOD_THREAD_BYTES);
        if (!object) { g_eax = FMOD_ERR_OUTPUT_INIT; g_esp += 8; return; }
        memset(GPTR(object), 0, FMOD_THREAD_BYTES);
        void_call_guest_this(FMOD_THREAD_CTOR, object, 0, NULL);
        /* name, callback, its parameter, priority 3 (FMOD's mixer thread: time
           critical), 0, stack, no wake event, no period. */
        uint32_t args[8] = { name, turn, state, 3, 0, 0x8000, 0, 0 };
        result = void_call_guest_this(FMOD_THREAD_INIT, object, 8, args);
        if (result) { void_gfree(object); void_log("[fmod-output] thread start failed: %u", result); }
        else thread_object = object;
    }
    g_eax = result;
    g_esp += 8;
}

static void out_stop(void)
{
    uint32_t result = FMOD_OK;
    if (thread_object) {
        result = void_call_guest_this(FMOD_THREAD_CLOSE, thread_object, 0, NULL);
        void_gfree(thread_object);
        thread_object = 0;
        void_audio_producer_stopped();
        void_log("[fmod-output] stop: %llu reads, %llu frames", (unsigned long long)renders, (unsigned long long)rendered_frames);
    }
    g_eax = result;
    g_esp += 8;
}

static void out_close(void)
{
    void_audio_close();
    g_eax = FMOD_OK;
    g_esp += 8;
}

static void out_update(void) { g_eax = FMOD_OK; g_esp += 8; }

static void out_gethandle(void)
{
    if (ARG(1)) MEM32(ARG(1)) = 0;
    g_eax = FMOD_OK;
    g_esp += 12;
}

/* Seam at 0x10EAC923 (cdecl, no arguments, returns the description): 1 when
 * the native description replaced the original body. */
int void_bioshock_fmod_output_description(void)
{
    if (!native_enabled()) return 0;
    static uint32_t name, thunks[10];
    if (!name) {
        name = void_gstrdup("FMOD Android AAudio Output");
        thunks[0] = void_thunk(out_getnumdrivers, "fmod_output_getnumdrivers");
        thunks[1] = void_thunk(out_getdrivername, "fmod_output_getdrivername");
        thunks[2] = void_thunk(out_init, "fmod_output_init");
        thunks[3] = void_thunk(out_close, "fmod_output_close");
        thunks[4] = void_thunk(out_update, "fmod_output_update");
        thunks[5] = void_thunk(out_gethandle, "fmod_output_gethandle");
        thunks[6] = void_thunk(out_getdrivercapsex, "fmod_output_getdrivercapsex");
        thunks[7] = void_thunk(out_start, "fmod_output_start");
        thunks[8] = void_thunk(out_stop, "fmod_output_stop");
    }
    uint32_t d = FMOD_DSOUND_DESCRIPTION;
    memset(GPTR(d), 0, FMOD_DESCRIPTION_BYTES);
    MEM32(d + 0x00) = name;
    MEM32(d + 0x04) = 0x10100u;
    MEM32(d + 0x08) = 0;                /* not polled: the plugin reads the mix */
    MEM32(d + 0x0C) = thunks[0];
    MEM32(d + 0x10) = thunks[1];
    MEM32(d + 0x18) = thunks[2];
    MEM32(d + 0x1C) = thunks[3];
    MEM32(d + 0x20) = thunks[4];
    MEM32(d + 0x24) = thunks[5];
    MEM32(d + 0x40) = FMOD_OUTPUTTYPE_DSOUND;
    MEM32(d + 0x44) = 0;                /* FMOD's own Output object (236 bytes) */
    MEM32(d + 0x50) = thunks[6];
    MEM32(d + 0x58) = thunks[7];
    MEM32(d + 0x5C) = thunks[8];
    g_eax = d;
    g_esp += 4;
    return 1;
}

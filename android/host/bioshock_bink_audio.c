/* Bink's movie sound without DirectSound: a native Bink sound driver over AAudio.
 *
 * The game's movies (the intro, the menu loops, the cut scenes) play their
 * sound through Bink 1.x, which takes a sound driver from the game:
 * BinkSetSoundSystem(BinkOpenDirectSound, ds) at 0x10BAC7F8. That driver
 * (binkw32 0x1800EE40) copied Bink's decoded PCM into a looping 600 ms
 * DirectSound buffer, which dsound.c emulated and a host mixer thread
 * resampled to 48 kHz for the blocking AAudio writer of android_main.c.
 *
 * BinkOpenDirectSound (binkw32 0x1800F120, stdcall, one argument) now hands
 * Bink this driver instead (the seam is in tools/build_android.py). Bink's
 * driver contract is its public BINKSND structure (bink.h 1.x, the offsets the
 * DirectSound driver uses): ten stdcall callbacks at +0x00..+0x24, then
 * +0x54 BestSizeIn16, +0x5C SoundDroppedOut, +0x64 OnOff, +0x68 Latency,
 * +0x70 freq, +0x74 bits, +0x78 chans and the driver's own bytes from +0x7C.
 *
 *   open(snd, freq, bits, chans, flags, bink) -> 1   a 600 ms ring and an AAudio stream at the movie's rate
 *   Ready(snd)                                         room for one chunk (50 ms), on and not paused
 *   Lock(snd, U8 **addr, U32 *len)                     a guest staging chunk
 *   Unlock(snd, filled)                                into the ring (8-bit and mono become stereo PCM16)
 *   Volume(snd, vol), Pan(snd, pan)                    0x7FFF is 0 dB, 0x8000 the centre
 *   Pause(snd, on), SetOnOff(snd, on), Close(snd)
 *   MixBins, MixBinVols                                Xbox speaker bins: ignored
 *
 * Each stream is a normal-mode AAudio stream at the movie's own rate (Android
 * resamples it), drained by its data callback from the ring with the gain and
 * pan applied there; it needs no guest machine. In the background it plays
 * silence and keeps its ring; a lost route reopens it at the next Ready
 * (a failed open is retried 20 ms, 40 ms, ... up to 2 s later).
 *
 * <userdata>/bink_audio_fault, read at most once a second, injects failures as
 * audio_native.c's audio_fault: "disconnect [n]" (the next n streams lose their
 * route) or "unavailable [n]" (the next n opens fail). The file is removed. */
#include "host.h"
#include "audio_compat.h"
#include <math.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define SLOTS        8u
#define SND_TAG      0x4B4E4942u            /* 'BINK' at +0x7C, the slot at +0x80 */
#define SND_BEST     0x54u
#define SND_DROPPED  0x5Cu
#define SND_ONOFF    0x64u
#define SND_LATENCY  0x68u
#define SND_FREQ     0x70u
#define SND_BITS     0x74u
#define SND_CHANS    0x78u
#define SND_PRIVATE  0x7Cu
#define SND_BYTES    0x17Cu

typedef struct {
    int used;
    uint32_t snd, freq, chans, bits;
    uint32_t chunk;                      /* bytes of Bink's PCM per Lock */
    uint32_t stage;                      /* guest staging chunk */
    int16_t *ring;                       /* stereo frames */
    uint32_t ring_frames;
    _Atomic uint64_t in, out;            /* frames written / played */
    _Atomic int gain_l, gain_r;          /* Q15 */
    _Atomic int paused, on, dropped, started;
    _Atomic int failed;
    int32_t volume, pan;
    uint64_t pause_epoch;
    AAudioStream *stream;
    uint64_t retry_at, retry_delay, lost_at;
    unsigned lost_opens;
    uint64_t frames_in, underruns;
    int peak;
} bink_snd;

static bink_snd slots[SLOTS];
static uint32_t thunk_open;
static unsigned fault_disconnects, fault_opens;
static uint64_t fault_polled;

static void fault_poll(uint64_t now)
{
    if (fault_polled && now - fault_polled < 1000000000ull) return;
    fault_polled = now;
    const char *dir = void_data_dir();
    if (!dir || !*dir) return;
    char path[1200], line[64] = "", kind[16] = "";
    snprintf(path, sizeof path, "%s/bink_audio_fault", dir);
    FILE *file = fopen(path, "r");
    if (!file) return;
    if (!fgets(line, sizeof line, file)) line[0] = 0;
    fclose(file);
    if (unlink(path)) return;
    unsigned count = 1;
    if (sscanf(line, "%15s %u", kind, &count) < 1) return;
    if (count > 1000) count = 1000;
    if (!strcmp(kind, "disconnect")) fault_disconnects = count;
    else if (!strcmp(kind, "unavailable")) fault_opens = count;
    else { void_log("[bink-audio] fault injection: unknown request '%s'", kind); return; }
    void_log("[bink-audio] fault injection: %s x%u", kind, count);
}

static bink_snd *slot_of(uint32_t snd)
{
    if (!snd || MEM32(snd + SND_PRIVATE) != SND_TAG) return NULL;
    uint32_t i = MEM32(snd + SND_PRIVATE + 4);
    return i < SLOTS && slots[i].used && slots[i].snd == snd ? &slots[i] : NULL;
}

/* As the DirectSound driver set its buffer: volume up to 0x7FFF is linear
 * (0x7FFF = 0 dB), pan 0..0x10000 becomes (pan - 0x8000) * 10000 / 32768
 * millibels that attenuate the opposite side. */
static void update_gain(bink_snd *s)
{
    float v = (float)(s->volume < 0 ? 0 : s->volume > 0x7FFF ? 0x7FFF : s->volume) / 32767.0f;
    int32_t p = s->pan < 0 ? 0 : s->pan > 0x10000 ? 0x10000 : s->pan;
    float mb = (float)(p - 0x8000) * 10000.0f / 32768.0f;
    float l = mb > 0 ? powf(10.0f, -mb / 2000.0f) : 1.0f, r = mb < 0 ? powf(10.0f, mb / 2000.0f) : 1.0f;
    atomic_store(&s->gain_l, (int)(v * l * 32768.0f));
    atomic_store(&s->gain_r, (int)(v * r * 32768.0f));
}

static aaudio_data_callback_result_t data_callback(AAudioStream *stream, void *user, void *data, int32_t count)
{
    (void)stream;
    bink_snd *s = (bink_snd *)user;
    int16_t *out = (int16_t *)data;
    uint32_t frames = count > 0 ? (uint32_t)count : 0u;
    if ((void_audio_pause_state() & 1u) || atomic_load(&s->paused) || !atomic_load(&s->on)) {
        memset(out, 0, (size_t)frames * 4u);
        return AAUDIO_CALLBACK_RESULT_CONTINUE;
    }
    uint64_t at = atomic_load_explicit(&s->out, memory_order_relaxed);
    uint64_t level = atomic_load_explicit(&s->in, memory_order_acquire) - at;
    uint32_t n = level < frames ? (uint32_t)level : frames;
    int gl = atomic_load_explicit(&s->gain_l, memory_order_relaxed), gr = atomic_load_explicit(&s->gain_r, memory_order_relaxed);
    for (uint32_t i = 0; i < n; ++i) {
        const int16_t *f = s->ring + 2u * (uint32_t)((at + i) % s->ring_frames);
        int l = (f[0] * gl) >> 15, r = (f[1] * gr) >> 15;
        out[2 * i] = (int16_t)(l > 32767 ? 32767 : l < -32768 ? -32768 : l);
        out[2 * i + 1] = (int16_t)(r > 32767 ? 32767 : r < -32768 ? -32768 : r);
    }
    if (n < frames) {
        memset(out + 2u * n, 0, (size_t)(frames - n) * 4u);
        if (atomic_load_explicit(&s->started, memory_order_relaxed)) {
            atomic_store(&s->dropped, 1);
            ++s->underruns;
        }
    }
    atomic_store_explicit(&s->out, at + n, memory_order_release);
    return AAUDIO_CALLBACK_RESULT_CONTINUE;
}

static void error_callback(AAudioStream *stream, void *user, aaudio_result_t error)
{
    (void)stream; (void)error;
    atomic_store(&((bink_snd *)user)->failed, 1);
}

static void stream_close(bink_snd *s)
{
    if (!s->stream) return;
    AAudioStream_requestStop(s->stream);
    AAudioStream_close(s->stream);       /* no callback runs after this */
    s->stream = NULL;
}

/* Called with the machine released. */
static void stream_open(bink_snd *s)
{
    AAudioStreamBuilder *b = NULL;
    atomic_store(&s->failed, 0);
    s->pause_epoch = void_audio_pause_state();
    if (s->pause_epoch & 1u) return;
    uint64_t now = void_monotonic_ns();
    if (fault_opens || AAudio_createStreamBuilder(&b) != AAUDIO_OK) {
        if (fault_opens) --fault_opens;
        goto failed;
    }
    AAudioStreamBuilder_setFormat(b, AAUDIO_FORMAT_PCM_I16);
    AAudioStreamBuilder_setChannelCount(b, 2);
    AAudioStreamBuilder_setSampleRate(b, (int32_t)s->freq);
    AAudioStreamBuilder_setSharingMode(b, AAUDIO_SHARING_MODE_SHARED);
    AAudioStreamBuilder_setPerformanceMode(b, AAUDIO_PERFORMANCE_MODE_NONE);
    aaudio_game_attributes(b, AAUDIO_CONTENT_TYPE_MOVIE);
    AAudioStreamBuilder_setDataCallback(b, data_callback, s);
    AAudioStreamBuilder_setErrorCallback(b, error_callback, s);
    aaudio_result_t r = AAudioStreamBuilder_openStream(b, &s->stream);
    AAudioStreamBuilder_delete(b);
    if (r != AAUDIO_OK || AAudioStream_getSampleRate(s->stream) != (int32_t)s->freq) {
        if (r == AAUDIO_OK) AAudioStream_close(s->stream);
        s->stream = NULL;
        goto failed;
    }
    r = AAudioStream_requestStart(s->stream);
    if (r != AAUDIO_OK) { stream_close(s); goto failed; }
    s->retry_delay = 0;
    if (s->lost_at) {
        void_log("[bink-audio] output back after %.0f ms, %u failed opens", (double)(now - s->lost_at) / 1e6, s->lost_opens);
        s->lost_at = 0;
        s->lost_opens = 0;
        return;
    }
    void_log("[bink-audio] stream %u Hz x %u ch %u bit -> AAudio normal, burst %d, buffer %d",
             s->freq, s->chans, s->bits, (int)AAudioStream_getFramesPerBurst(s->stream),
             (int)AAudioStream_getBufferSizeInFrames(s->stream));
    return;
failed:
    if (!s->lost_at) s->lost_at = now;
    ++s->lost_opens;
    s->retry_delay = !s->retry_delay ? 20000000ull : s->retry_delay < 1000000000ull ? s->retry_delay * 2 : 2000000000ull;
    s->retry_at = now + s->retry_delay;
    if (s->lost_opens <= 8)
        void_log("[bink-audio] stream %u Hz unavailable; next attempt in %.0f ms", s->freq, (double)s->retry_delay / 1e6);
}

/* A new foreground epoch or a lost route: a fresh stream (the ring is kept). */
static void stream_service(bink_snd *s)
{
    uint64_t pause = void_audio_pause_state(), now = void_monotonic_ns();
    fault_poll(now);
    if (s->stream && fault_disconnects) { --fault_disconnects; atomic_store(&s->failed, 1); }
    if (s->stream && !atomic_load(&s->failed) && pause == s->pause_epoch) return;
    if (!s->stream && (pause & 1u)) return;
    if (!s->stream && now < s->retry_at) return;
    if (s->stream && atomic_load(&s->failed) && !s->lost_at) {
        s->lost_at = now;
        void_log("[bink-audio] stream lost its route; reopening");
    }
    void_blocking_begin();
    if (s->stream) stream_close(s);
    stream_open(s);
    void_blocking_end();
}

static uint32_t ring_free_bytes(bink_snd *s)
{
    uint64_t level = atomic_load(&s->in) - atomic_load(&s->out);
    uint32_t free_frames = level >= s->ring_frames ? 0 : s->ring_frames - (uint32_t)level;
    return free_frames * s->chans * (s->bits / 8u);
}

/* Ready(snd) */
static void snd_ready(void)
{
    bink_snd *s = slot_of(ARG(0));
    uint32_t r = 0;
    if (s) {
        stream_service(s);
        if (atomic_load(&s->dropped)) { MEM32(s->snd + SND_DROPPED) = 1; atomic_store(&s->dropped, 0); }
        r = atomic_load(&s->on) && !atomic_load(&s->paused) && ring_free_bytes(s) >= s->chunk;
    }
    g_eax = r;
    g_esp += 8;
}

/* Lock(snd, U8 **addr, U32 *len) */
static void snd_lock(void)
{
    bink_snd *s = slot_of(ARG(0));
    uint32_t addr = ARG(1), len = ARG(2), ok = s && addr && len;
    if (ok) { MEM32(addr) = s->stage; MEM32(len) = s->chunk; }
    g_eax = ok;
    g_esp += 16;
}

/* Unlock(snd, U32 filled) */
static void snd_unlock(void)
{
    bink_snd *s = slot_of(ARG(0));
    uint32_t filled = ARG(1);
    if (s) {
        if (filled > s->chunk) filled = s->chunk;
        uint32_t bytes = s->chans * (s->bits / 8u), frames = bytes ? filled / bytes : 0;
        uint32_t room = ring_free_bytes(s) / (bytes ? bytes : 1);
        if (frames > room) frames = room;
        uint64_t at = atomic_load_explicit(&s->in, memory_order_relaxed);
        const uint8_t *src = (const uint8_t *)GPTR(s->stage);
        for (uint32_t i = 0; i < frames; ++i) {
            int16_t l, r;
            if (s->bits == 16) {
                const int16_t *p = (const int16_t *)src + (size_t)i * s->chans;
                l = p[0]; r = s->chans > 1 ? p[1] : p[0];
            } else {
                const uint8_t *p = src + (size_t)i * s->chans;
                l = (int16_t)((p[0] - 128) << 8); r = s->chans > 1 ? (int16_t)((p[1] - 128) << 8) : l;
            }
            int16_t *f = s->ring + 2u * (uint32_t)((at + i) % s->ring_frames);
            f[0] = l; f[1] = r;
            int a = l < 0 ? -l : l, b = r < 0 ? -r : r;
            if (a > s->peak) s->peak = a;
            if (b > s->peak) s->peak = b;
        }
        atomic_store_explicit(&s->in, at + frames, memory_order_release);
        s->frames_in += frames;
        if (frames) atomic_store(&s->started, 1);
    }
    g_eax = s != NULL;
    g_esp += 12;
}

static void snd_volume(void)
{
    bink_snd *s = slot_of(ARG(0));
    if (s) { s->volume = (int32_t)ARG(1); update_gain(s); }
    g_eax = s != NULL;
    g_esp += 12;
}

static void snd_pan(void)
{
    bink_snd *s = slot_of(ARG(0));
    if (s) { s->pan = (int32_t)ARG(1); update_gain(s); }
    g_eax = s != NULL;
    g_esp += 12;
}

/* Pause(snd, status): returns whether it is paused now. */
static void snd_pause(void)
{
    bink_snd *s = slot_of(ARG(0));
    uint32_t r = 0;
    if (s) { atomic_store(&s->paused, ARG(1) != 0); r = ARG(1) != 0; }
    g_eax = r;
    g_esp += 12;
}

/* SetOnOff(snd, status): off drops what is queued; returns OnOff. */
static void snd_onoff(void)
{
    bink_snd *s = slot_of(ARG(0));
    uint32_t r = 0;
    if (s) {
        int on = ARG(1) != 0;
        if (on != atomic_load(&s->on)) {
            atomic_store(&s->on, on);
            atomic_store(&s->out, atomic_load(&s->in));
            atomic_store(&s->started, 0);
        }
        MEM32(s->snd + SND_ONOFF) = (uint32_t)on;
        r = (uint32_t)on;
    }
    g_eax = r;
    g_esp += 12;
}

static void snd_close(void)
{
    bink_snd *s = slot_of(ARG(0));
    if (s) {
        void_blocking_begin();
        stream_close(s);
        void_blocking_end();
        void_log("[bink-audio] closed: %llu frames queued (%.1f s), peak %d, %llu underruns", (unsigned long long)s->frames_in,
                 s->freq ? (double)s->frames_in / s->freq : 0.0, s->peak, (unsigned long long)s->underruns);
        free(s->ring);
        void_gfree(s->stage);
        MEM32(s->snd + SND_PRIVATE) = 0;
        memset(s, 0, sizeof *s);
    }
    g_eax = 0;
    g_esp += 8;
}

static void snd_mixbins(void) { g_eax = 0; g_esp += 16; }
static void snd_mixbinvols(void) { g_eax = 0; g_esp += 20; }

/* open(BINKSND *snd, freq, bits, chans, flags, HBINK bink) -> 1 on success */
static void snd_open(void)
{
    uint32_t snd = ARG(0), freq = ARG(1), bits = ARG(2), chans = ARG(3);
    static uint32_t cb[10];
    if (!cb[0]) {
        cb[0] = void_thunk(snd_ready, "bink_snd_ready");
        cb[1] = void_thunk(snd_lock, "bink_snd_lock");
        cb[2] = void_thunk(snd_unlock, "bink_snd_unlock");
        cb[3] = void_thunk(snd_volume, "bink_snd_volume");
        cb[4] = void_thunk(snd_pan, "bink_snd_pan");
        cb[5] = void_thunk(snd_pause, "bink_snd_pause");
        cb[6] = void_thunk(snd_onoff, "bink_snd_onoff");
        cb[7] = void_thunk(snd_close, "bink_snd_close");
        cb[8] = void_thunk(snd_mixbins, "bink_snd_mixbins");
        cb[9] = void_thunk(snd_mixbinvols, "bink_snd_mixbinvols");
    }
    uint32_t result = 0, index = SLOTS;
    for (uint32_t i = 0; i < SLOTS; ++i) if (!slots[i].used) { index = i; break; }
    if (snd && freq >= 4000 && freq <= 192000 && (bits == 8 || bits == 16) && (chans == 1 || chans == 2) && index < SLOTS) {
        bink_snd *s = &slots[index];
        memset(GPTR(snd), 0, SND_BYTES);
        uint32_t per_second = freq * chans * (bits / 8u);
        s->chunk = (per_second * 50u / 1000u + 255u) & ~255u;       /* as the DirectSound driver: 50 ms */
        s->ring_frames = (uint32_t)((uint64_t)freq * 600u / 1000u);  /* and 600 ms */
        s->ring = (int16_t *)calloc(s->ring_frames, 4u);
        s->stage = void_galloc(s->chunk);
        if (s->ring && s->stage) {
            s->used = 1;
            s->snd = snd; s->freq = freq; s->chans = chans; s->bits = bits;
            s->volume = 0x7FFF; s->pan = 0x8000;
            update_gain(s);
            atomic_store(&s->on, 1);
            for (uint32_t k = 0; k < 10; ++k) MEM32(snd + 4u * k) = cb[k];
            MEM32(snd + SND_BEST) = bits == 8 ? s->chunk * 2u : s->chunk;
            MEM32(snd + SND_ONOFF) = 1;
            MEM32(snd + SND_LATENCY) = 0;
            MEM32(snd + SND_FREQ) = freq;
            MEM32(snd + SND_BITS) = bits;
            MEM32(snd + SND_CHANS) = chans;
            MEM32(snd + SND_PRIVATE) = SND_TAG;
            MEM32(snd + SND_PRIVATE + 4) = index;
            void_blocking_begin();
            stream_open(s);
            void_blocking_end();
            result = 1;
        } else {
            free(s->ring);
            if (s->stage) void_gfree(s->stage);
            memset(s, 0, sizeof *s);
        }
    }
    if (!result) void_log("[bink-audio] open refused: %u Hz %u bit %u ch", freq, bits, chans);
    g_eax = result;
    g_esp += 28;
}

/* Seam at binkw32 0x1800F120, BinkOpenDirectSound(param) (stdcall): the
 * native driver's open function instead of the DirectSound one. */
int void_bioshock_bink_open_sound(void)
{
    const char *mode = getenv("BIOSHOCK_BINK_OUTPUT");
    if (mode && !strcmp(mode, "dsound")) return 0;
    if (!thunk_open) {
        thunk_open = void_thunk(snd_open, "bink_snd_open");
        void_log("[bink-audio] native Bink sound driver (DirectSound driver replaced)");
    }
    g_eax = thunk_open;
    g_esp += 8;
    return 1;
}

"""Exercise production AAudio priming, bounds and partial-write/lifecycle edges."""
from pathlib import Path
import subprocess
import unittest

from tests.host_environment import CLANG, environment
from tests.test_mobile_shaders import function

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / "build/tests/android-audio-buffer"

HARNESS = r'''
#define _CRT_SECURE_NO_WARNINGS
#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#define AAUDIO_OK 0
#define AAUDIO_ERROR_DISCONNECTED (-899)
#define AAUDIO_ERROR_UNAVAILABLE (-889)
#define unlink(path) remove(path)
static char data_dir[64] = "no-such-audio-fault-dir";
static const char *AAudio_convertResultToText(int32_t result) { (void)result; return "error"; }
#define AUDIO_PRIME_FRAMES 1440u
#define CLOCK_MONOTONIC 1
typedef int32_t aaudio_result_t;
typedef struct { int32_t capacity, size, burst, started; } AAudioStream;
static AAudioStream instance, *stream;
static uint64_t stream_pause_state, pause_state, tick;
static uint64_t audio_short_writes, audio_dropped_frames, audio_errors;
static unsigned audio_zero_chunks;
static int set_error, start_error, fail_write, interrupted, calls, pause_write, stops, closes, reported, request;
static int16_t accepted[8192];
static unsigned accepted_frames;
static uint64_t void_audio_pause_state(void) { return pause_state; }
static void void_log(const char *format, ...) { (void)format; ++reported; }
static int AAudioStream_requestStop(AAudioStream *s) { assert(s == &instance); ++stops; return 0; }
static int AAudioStream_close(AAudioStream *s) { assert(s == &instance); ++closes; return 0; }
static int32_t AAudioStream_getBufferCapacityInFrames(AAudioStream *s) { return s->capacity; }
static int32_t AAudioStream_getBufferSizeInFrames(AAudioStream *s) { return s->size; }
static int32_t AAudioStream_getFramesPerBurst(AAudioStream *s) { return s->burst; }
static int32_t AAudioStream_getSampleRate(AAudioStream *s) { (void)s; return 48000; }
static int32_t AAudioStream_getPerformanceMode(AAudioStream *s) { (void)s; return 2; }
static int32_t AAudioStream_setBufferSizeInFrames(AAudioStream *s, int32_t n) {
    assert(s == &instance && n > 0 && n <= s->capacity); request = n;
    if (set_error) return set_error;
    s->size = n; return n;
}
static int AAudioStream_requestStart(AAudioStream *s) {
    assert(s == &instance && !s->started && !accepted_frames);
    if (start_error) return start_error;
    s->started = 1; return 0;
}
static int fixture_clock(int clock, struct timespec *t) {
    assert(clock == CLOCK_MONOTONIC); tick += 1000000;
    t->tv_sec = (time_t)(tick/1000000000); t->tv_nsec = (long)(tick%1000000000); return 0;
}
#define clock_gettime fixture_clock
static void fixture_usleep(unsigned us) { tick += (uint64_t)us*1000; }
#define usleep fixture_usleep
static int AAudioStream_write(AAudioStream *s, const void *input, int32_t frames, int64_t timeout) {
    assert(s == &instance && s->started && frames > 0 && timeout > 0); ++calls;
    if (fail_write) return -899;
    if (interrupted) { interrupted = 0; return 0; }
    int32_t n = frames > 257 ? 257 : frames;
    assert(accepted_frames+(unsigned)n <= sizeof accepted/sizeof accepted[0]/2);
    memcpy(accepted+2*accepted_frames, input, (size_t)n*2*sizeof(int16_t)); accepted_frames += (unsigned)n;
    if (pause_write) { pause_write = 0; pause_state = 1; }
    return n;
}
/* ACTUAL */
static void reset(void) {
    memset(&instance, 0, sizeof instance); stream = &instance;
    instance.capacity = 1924; instance.size = 1924; instance.burst = 96;
    stream_pause_state = pause_state = tick = 0;
    audio_short_writes = audio_dropped_frames = audio_errors = 0;
    audio_zero_chunks = 0;
    audio_retry_at = audio_retry_delay = audio_stream_opened = audio_lost_at = audio_log_window = 0;
    audio_lost_attempts = audio_log_count = 0; audio_lost_frames = audio_reconnects = 0;
    audio_pending_frames = 0; audio_pending_state = 0; audio_fault_writes = audio_fault_opens = 0;
    set_error = start_error = fail_write = interrupted = calls = pause_write = stops = closes = reported = request = 0;
    accepted_frames = 0; memset(accepted, 0x7f, sizeof accepted);
}
int main(void) {
    reset(); interrupted = 1;
    assert(audio_start_stream(0) == 0 && accepted_frames == 1440 && request == 1440 && reported);
    assert(audio_short_writes && !audio_dropped_frames && !audio_errors && instance.started);
    for (unsigned i = 0; i < accepted_frames*2; ++i) assert(accepted[i] == 0);
    int16_t pcm[960]; for (unsigned i = 0; i < 960; ++i) pcm[i] = (int16_t)(i+1);
    assert(audio_write_frames(pcm, 480, 1) == 480 && accepted_frames == 1920);
    assert(!memcmp(accepted+2880, pcm, sizeof pcm));
    reset(); instance.burst = 960;
    assert(audio_start_stream(0) == 0 && request == 1920 && accepted_frames == 1440);
    reset(); instance.capacity = instance.size = 760;
    assert(audio_start_stream(0) == 0 && request == 760 && accepted_frames == 760);
    reset(); instance.burst = INT32_MAX;
    assert(audio_start_stream(0) == 0 && request == 1440 && accepted_frames == 1440);
    reset(); set_error = -895; instance.size = 384;
    assert(audio_start_stream(0) == 0 && instance.started && accepted_frames == 384);
    reset(); pause_state = 1;
    assert(audio_start_stream(0) == 0 && !stream && stops == 1 && closes == 1 && !calls);
    reset(); pause_write = 1;
    assert(audio_start_stream(0) == 0 && accepted_frames == 257 && !audio_dropped_frames && calls == 1);
    reset(); start_error = -899;
    assert(audio_start_stream(0) == -899 && !calls && !stops);
    audio_close_failed(-899); assert(!stream && audio_errors == 1 && closes == 1);
    reset(); fail_write = 1;
    assert(audio_start_stream(0) == 0 && !stream && calls == 1 && audio_errors == 1 && closes == 1);
    /* A6: a route change fails a write of game PCM; its unwritten tail (<= 480 frames) waits for the
       next stream, the rest is counted as lost, and the reopen happens at the next chunk. */
    int16_t big[2 * 1200]; for (unsigned i = 0; i < 2400; ++i) big[i] = (int16_t)(i + 7);
    reset(); assert(audio_start_stream(0) == 0); audio_stream_opened = 0; fail_write = 1;
    assert(audio_write_frames(big, 1200, 1) == 0 && !stream && audio_errors == 1 && closes == 1);
    assert(audio_pending_frames == 480 && !memcmp(audio_pending, big, 480 * 2 * sizeof big[0]));
    assert(audio_dropped_frames == 720 && audio_lost_frames == 720 && audio_lost_at && !audio_retry_at);
    /* The silent prime never becomes a pending tail. */
    reset(); fail_write = 1;
    assert(audio_start_stream(0) == 0 && !stream && !audio_pending_frames);
    /* A stream that fails within a second of opening is an unavailable output: back off, keep nothing. */
    reset(); assert(audio_start_stream(0) == 0); audio_stream_opened = audio_now(); fail_write = 1;
    assert(audio_write_frames(big, 480, 1) == 0 && !stream && audio_retry_delay == 20000000LL);
    assert(audio_retry_at > audio_stream_opened && !audio_pending_frames && audio_dropped_frames == 480);
    /* Failed opens retry 20, 40, ... ms, at most 2 s apart: no open attempt every 10 ms chunk. */
    reset(); stream = NULL;
    int64_t expected = 20000000LL;
    for (int i = 0; i < 10; ++i) {
        audio_open_failed(AAUDIO_ERROR_UNAVAILABLE);
        assert(audio_retry_delay == (expected < 2000000000LL ? expected : 2000000000LL));
        expected *= 2;
    }
    assert(audio_lost_attempts == 10 && audio_errors == 10);
    /* Losses and returns are logged at most 8 lines per 10 s for the whole session, then counted. */
    reset(); reported = 0;
    for (int i = 0; i < 50; ++i) audio_close_failed(AAUDIO_ERROR_DISCONNECTED);
    assert(reported == 8 && audio_errors == 50);
    tick += 11000000000ULL; audio_close_failed(AAUDIO_ERROR_DISCONNECTED);
    assert(reported == 10);
    /* QA fault injection fails the next write without calling AAudio. */
    reset(); assert(audio_start_stream(0) == 0); audio_stream_opened = 0;
    int before = calls; audio_fault_writes = 1;
    assert(audio_write_frames(big, 100, 1) == 0 && calls == before && !stream && audio_pending_frames == 100);
    audio_fault_poll(audio_now()); assert(!audio_fault_writes && !audio_fault_opens); /* no request file */
    puts("PASS AAudio route loss: tail kept for the next stream, prime never pending, early failure backs off 20 ms..2 s, bounded loss logs, fault injection");
    puts("PASS AAudio: 30 ms live priming, burst/capacity bounds, every accepted PCM prefix once, short writes, pause/disconnect/start/config errors");
    return 0;
}
'''


class AndroidAudioBufferTest(unittest.TestCase):
    def test_production_output(self):
        source = (ROOT / "android/host/android_main.c").read_text(encoding="utf-8")
        start = source.index("#define AUDIO_RETRY_FIRST_NS")
        end = source.index("static aaudio_result_t audio_start_stream(")
        route = source[start:end]
        self.assertIn("static uint32_t audio_write_frames(", route)
        actual = "\n".join([function(source, "static void audio_close_stream(void)"), route,
                             function(source, "static aaudio_result_t audio_start_stream(")])
        OUT.mkdir(parents=True, exist_ok=True)
        probe = OUT / "probe.c"
        probe.write_text(HARNESS.replace("/* ACTUAL */", actual), encoding="utf-8")
        env, linker = environment("x64")
        obj, binary = OUT / "probe.obj", OUT / "probe.exe"
        for command in ([str(CLANG), "/nologo", "/O2", "/W4", "/WX", "/std:c11",
                         "/clang:--target=x86_64-pc-windows-msvc", "/c", str(probe), "/Fo" + str(obj)],
                        [str(linker), "/nologo", str(obj), "/OUT:" + str(binary)], [str(binary)]):
            result = subprocess.run(command, cwd=ROOT, env=env, capture_output=True, text=True, timeout=60)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        result = subprocess.run([str(CLANG.with_name("clang.exe")), "--target=aarch64-linux-android29", "-O2",
                                 "-std=gnu11", str(probe), "-Wl,-z,max-page-size=16384", "-o", str(OUT / "test-android-audio-buffer")],
                                capture_output=True, text=True, timeout=60)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        output = function(source, "void void_audio_output(")
        self.assertIn("AAudioStreamBuilder_setBufferCapacityInFrames(b, AUDIO_PRIME_FRAMES);", output)
        self.assertIn("result = audio_start_stream(pause);", output)
        # The open path honours the backoff and plays the kept tail first (A6).
        self.assertIn("if (!stream && audio_retry_at && now < audio_retry_at)", output)
        self.assertIn("audio_write_frames(audio_pending, pending, 0)", output)
        self.assertIn("audio_write_frames(frames, count, 1)", output)
        # The return line is never hidden by the loss-line limit (it was, after 6 failed opens on the device).
        self.assertIn("unsigned hidden = audio_may_log(back) ? 0 : audio_log_count - 9;", output)
        self.assertNotIn("if (audio_may_log(back))", output)


if __name__ == "__main__":
    unittest.main()

"""Exercise the production DirectSound starvation policy without a device."""
from pathlib import Path
import subprocess
import unittest

from tests.host_environment import CLANG, environment
from tests.test_mobile_shaders import function

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / "build/tests/dsound-streaming"

HARNESS = r'''
#include <assert.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define MIX_RATE 48000
#define MIX_CHUNK 480
#define STALL_TICKS_MAX 25
#define STREAM_REFILLS 8
#define DSBCAPS_CTRL3D 0x10u
#define DSTRACE(...) ((void)0)
typedef struct { uint32_t guest; } void_com_object;
typedef struct dsbuf dsbuf_t;
typedef struct { void_com_object com; dsbuf_t *owner; } facet_t;
/* STRUCT */
static unsigned char guest[65536];
#define MEM16(a) (*(uint16_t *)(guest + (a)))
#define MEM8(a) guest[(a)]
static dsbuf_t *buffers;
static float listener_pos[3], listener_front[3] = {0, 0, 1}, listener_top[3] = {0, 1, 0};
static float rolloff = 1.0f, distance_factor = 1.0f;
static int dstrace, stall_guard = 1;
static void signal_event(uint32_t event) { (void)event; }
/* ACTUAL */
static void silence(const float *mix) {
    for (unsigned i = 0; i < 2 * MIX_CHUNK; ++i) assert(mix[i] == 0.0f);
}
static void audible(const float *mix, unsigned begin, unsigned end) {
    for (unsigned i = begin; i < end; ++i) {
        assert(mix[2*i] == 12000.0f/32768.0f);
        assert(mix[2*i+1] == -12000.0f/32768.0f);
    }
}
int main(void) {
    dsbuf_t b = {0};
    b.data = 4096; b.size = 4096; b.block = 4; b.channels = 2;
    b.rate = b.frequency = MIX_RATE; b.bits = 16;
    b.playing = b.looping = 1; b.refills = STREAM_REFILLS; b.fresh = 1024;
    buffers = &b;
    for (unsigned i = 0; i < b.size/4; ++i) {
        MEM16(b.data + 4*i) = 12000;
        MEM16(b.data + 4*i + 2) = (uint16_t)(int16_t)-12000;
    }
    float mix[2*MIX_CHUNK];
    for (unsigned tick = 0; tick < STALL_TICKS_MAX; ++tick) {
        mix_chunk(mix, MIX_CHUNK); silence(mix); assert(b.play_pos == 0);
    }
    assert(b.stalled_frames == STALL_TICKS_MAX*MIX_CHUNK);
    mix_chunk(mix, MIX_CHUNK);
    assert(b.play_pos == 1920 && b.fresh == 0 && b.stale_frames == 224);
    audible(mix, 0, 256);
    for (unsigned i = 256; i < MIX_CHUNK; ++i) assert(mix[2*i] == 0 && mix[2*i+1] == 0);
    /* A producer waiting for the next play-cursor boundary must be able to
     * refill; resetting the hold here used to insert 250 ms on EVERY tick. */
    mix_chunk(mix, MIX_CHUNK); silence(mix);
    assert(b.play_pos == 3840 && b.stale_frames == 704);
    assert(b.stall_ticks == STALL_TICKS_MAX);
    unsigned consumed = 0;
    for (unsigned tick = 0; tick < 50; ++tick) {
        uint32_t previous = b.play_pos;
        mix_chunk(mix, MIX_CHUNK); silence(mix);
        consumed += (b.play_pos + b.size - previous) % b.size;
    }
    assert(consumed == 50*MIX_CHUNK*b.block);
    assert(b.stalled_frames == STALL_TICKS_MAX*MIX_CHUNK);
    /* Exactly one fresh chunk resumes at once, including a ring wrap. */
    b.fresh = MIX_CHUNK*b.block;
    mix_chunk(mix, MIX_CHUNK); audible(mix, 0, MIX_CHUNK);
    assert(b.fresh == 0 && !b.stall_ticks);
    uint32_t refilled_position = b.play_pos;
    mix_chunk(mix, MIX_CHUNK); silence(mix);
    assert(b.play_pos == refilled_position && b.stall_ticks == 1);
    /* Static loops and the diagnostic legacy switch keep their semantics. */
    b.refills = 0;
    mix_chunk(mix, MIX_CHUNK); audible(mix, 0, MIX_CHUNK);
    b.refills = STREAM_REFILLS; b.fresh = 0; stall_guard = 0;
    mix_chunk(mix, MIX_CHUNK); audible(mix, 0, MIX_CHUNK);
    puts("PASS DirectSound: one bounded hold per starvation, cursor notifications can recover, stale PCM stays muted, fresh refill rearms, static loops and legacy policy preserved");
    return 0;
}
'''


class DirectSoundStreamingTest(unittest.TestCase):
    def test_production_streaming(self):
        source = (ROOT / "android/host/dsound.c").read_text(encoding="utf-8")
        struct = source.split("struct dsbuf {", 1)[1].split("\n};", 1)[0]
        actual = "\n".join(function(source, signature) for signature in (
            "static int stall_enabled(void)", "static float sample_at(",
            "static void notify_passed(", "static void mix_chunk("))
        OUT.mkdir(parents=True, exist_ok=True)
        probe = OUT / "probe.c"
        probe.write_text(HARNESS.replace("/* STRUCT */", "struct dsbuf {" + struct + "\n};")
                         .replace("/* ACTUAL */", actual), encoding="utf-8")
        env, linker = environment("x64")
        obj, binary = OUT / "probe.obj", OUT / "probe.exe"
        commands = ([str(CLANG), "/nologo", "/O2", "/W4", "/WX", "/std:c11", "/D_CRT_SECURE_NO_WARNINGS",
                     "/clang:--target=x86_64-pc-windows-msvc", "/c", str(probe), "/Fo" + str(obj)],
                    [str(linker), "/nologo", str(obj), "/OUT:" + str(binary)], [str(binary)])
        for command in commands:
            result = subprocess.run(command, cwd=ROOT, env=env, capture_output=True, text=True, timeout=60)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("(void)void_native_worker_affinity_apply();", function(source, "static void *mixer_main("))
        compiler = CLANG.with_name("clang.exe")
        result = subprocess.run([str(compiler), "--target=aarch64-linux-android29", "-O2", "-std=gnu11",
                                 str(probe), "-lm", "-Wl,-z,max-page-size=16384", "-o", str(OUT / "test-dsound-streaming")],
                                capture_output=True, text=True, timeout=60)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


if __name__ == "__main__":
    unittest.main()

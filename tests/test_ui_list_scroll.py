"""M6: a vertical drag over the original Save/Load list scrolls it by the game's own arrow handlers.

The production touch_list_rows, touch_list_follow and void_touch_ui_scroll of
android/host/bioshock_touch.c run against a fake scene: the input pump's protocol (64 claims the
gesture, 0 asks for the ACK, INT32_MIN retires it), whole rows per row pitch with the remainder kept,
no scrolling without a claimed gesture or after the menu changed. The Java side must send the claim
for the ROWS probe flag, and only the list frames of pausePC.swf report it.
"""
from pathlib import Path
import re
import subprocess
import unittest

from tests.host_environment import CLANG, environment
from tests.test_mobile_shaders import function

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / "build/tests/ui-list-scroll"

HARNESS = r'''
#define _CRT_SECURE_NO_WARNINGS
#include <assert.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static uint8_t memory[1024];
#define MEM32(a) (*(uint32_t *)&memory[(a)])
static uint32_t current_scene;
static uint32_t touch_ui_scene(void) { return current_scene; }
static char calls[64][24]; static unsigned call_count;
static void void_bioshock_movie_call(uint32_t movie, const char *method, const char *key, const char *value)
{ assert(movie == current_scene && !key && !value); strcpy(calls[call_count++], method); }
static unsigned logged; static char last_log[128];
static void void_log(const char *fmt, ...) { ++logged; strcpy(last_log, fmt); }
#define LIST_ROW_PITCH 0.03176f
static uint32_t list_drag_scene;
static int list_drag_anchor = -1, list_drag_rows;
/* PRODUCTION */
static unsigned count(const char *name) { unsigned n = 0; for (unsigned i = 0; i < call_count; ++i) n += !strcmp(calls[i], name); return n; }
int main(void)
{
    const uint32_t scene = 0x100;
    MEM32(scene + 108) = 1080;            /* viewport height: pitch 34 */
    MEM32(scene + 244) = 600;             /* the movie's current mouse point */
    current_scene = scene;
    /* Hover and taps without a claimed list gesture never scroll. */
    touch_list_follow(scene, 100);
    assert(call_count == 0);
    /* Claim at y=600; the ACK query and unknown requests stay harmless. */
    assert(void_touch_ui_scroll(64) == 1 && void_touch_ui_scroll(0) == 1);
    touch_list_follow(scene, 590);        /* under one row */
    assert(call_count == 0);
    touch_list_follow(scene, 560);        /* finger up 40 px: one row, the next saves */
    assert(call_count == 1 && !strcmp(calls[0], "DownArrowPressed"));
    touch_list_follow(scene, 567);        /* jitter back across the boundary: no step back */
    assert(call_count == 1);
    touch_list_follow(scene, 600);        /* down 34 px from the kept anchor 566: one row up */
    assert(call_count == 2 && !strcmp(calls[1], "UpArrowPressed"));
    touch_list_follow(scene, 1000);       /* a fling is capped at 8 rows per step (11 rows owed) */
    assert(count("UpArrowPressed") == 9);
    touch_list_follow(scene, 1000);       /* the next step catches up with the finger */
    assert(count("UpArrowPressed") == 12);
    touch_list_follow(scene, 1000 - 34 * 3);   /* anchor 974: 76 px is two whole rows */
    assert(count("DownArrowPressed") == 3);
    assert(void_touch_ui_scroll(INT32_MIN) == 1 && logged == 1 && strstr(last_log, "[ui-list]"));
    /* After the gesture retired nothing follows the finger. */
    touch_list_follow(scene, 0);
    assert(call_count == 15);
    /* A new menu replaced the list during the gesture: no calls into the new movie. */
    assert(void_touch_ui_scroll(64) == 1);
    current_scene = 0x200;
    touch_list_follow(0x200, 0);
    assert(call_count == 15);
    assert(void_touch_ui_scroll(INT32_MIN) == 1);
    /* A retired gesture without rows logs nothing. */
    current_scene = scene; MEM32(scene + 244) = 300;
    assert(void_touch_ui_scroll(64) == 1 && void_touch_ui_scroll(INT32_MIN) == 1 && logged == 1);
    printf("PASS list drag: claim/ACK/retire, rows per pitch with kept remainder, 8-row cap, no calls without a claim or into a new menu\n");
    return 0;
}
'''


class UiListScrollTest(unittest.TestCase):
    def test_production_list_drag(self):
        source = (ROOT / "android/host/bioshock_touch.c").read_text(encoding="utf-8")
        self.assertIn("#define LIST_ROW_PITCH 0.03176f", source)
        production = "\n".join(function(source, signature) for signature in (
            "static int touch_list_rows(", "static void touch_list_follow(uint32_t scene, int y)\n{", "int void_touch_ui_scroll("))
        OUT.mkdir(parents=True, exist_ok=True)
        probe = OUT / "probe.c"
        probe.write_text(HARNESS.replace("/* PRODUCTION */", production), encoding="utf-8")
        env, linker = environment("x64")
        obj, binary = OUT / "probe.obj", OUT / "probe.exe"
        for command in ([str(CLANG), "/nologo", "/Od", "/W3", "/std:c11", "/clang:--target=x86_64-pc-windows-msvc",
                         "-Wno-unused-function", "/c", str(probe), "/Fo" + str(obj)],
                        [str(linker), "/nologo", str(obj), "/OUT:" + str(binary)],
                        [str(binary)]):
            result = subprocess.run(command, capture_output=True, text=True, env=env, timeout=120)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("PASS list drag", result.stdout)

    def test_list_frames_feed_and_java_claim(self):
        source = (ROOT / "android/host/bioshock_touch.c").read_text(encoding="utf-8")
        screen = function(source, "static int touch_list_screen(")
        # Frames 3 (Save), 4 and 19 (Load) of pausePC.swf define UpArrowPressed/DownArrowPressed.
        self.assertIn("frame == 3 || frame == 4 || frame == 19", screen)
        self.assertIn('"ConfirmBox._currentframe"', screen)
        control = function(source, "int void_touch_ui_control(")
        self.assertIn("touch_list_screen() ? 2 : 0", control)
        position = function(source, "int void_touch_ui_position(")
        self.assertIn("touch_list_follow(scene, y);", position)
        # The finger position fed to the movie is the one the rows follow.
        self.assertLess(position.index("void_call_guest_this(0x10A65A60u"), position.index("touch_list_follow(scene, y);"))
        java = (ROOT / "android/app/src/org/bioshock/port/UiTouchInput.java").read_text(encoding="utf-8")
        self.assertRegex(java, r"TAP = 1, ROWS = 2,")
        self.assertRegex(java, r"HOVER = 64,")
        self.assertIn("if((flags&ROWS)!=0&&!sink.scroll(HOVER))", java)
        # The original list scripts: the three frames of pausePC.swf name these functions.
        self.assertEqual(len(re.findall(r'"(?:Down|Up)ArrowPressed"', source)), 2)


if __name__ == "__main__":
    unittest.main()

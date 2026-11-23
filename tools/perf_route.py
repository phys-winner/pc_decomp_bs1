"""Scripted movement during a capture: the original touch controls only.

`walk` first backs away from the saved U-Invent pose, then turns the camera
with an upper-right drag and repeats. This is a motion smoke route, not the
old forward benchmark. Anchors use the default right-handed QA touch layout,
avoid the ammo HUD/context actions and scale from 2780x1264 to the input viewport.
"""
import re
import threading
import time

STICK = (778, 758)          # movement zone, right of the interactive ammo HUD
BACKWARD = (778, 1036)      # drag down: back away before turning
LOOK_FROM, LOOK_TO = (2500, 250), (2200, 250)
CANONICAL_SCREEN = (2780, 1264)


def scale_point(point, screen):
    if len(screen) != 2 or min(screen) < 8 or max(screen) > 16384:
        raise ValueError(f"Invalid landscape viewport: {screen}")
    return tuple(round(value * actual / reference) for value, actual, reference in zip(point, screen, CANONICAL_SCREEN))


def screen_size(adb):
    sizes = re.findall(r"(?:Physical|Override) size:\s*(\d+)x(\d+)", adb("shell", "wm", "size"))
    if not sizes:
        raise ValueError("Cannot read current display size; pass --screen WIDTH HEIGHT")
    return tuple(sorted(map(int, sizes[-1]), reverse=True))


def walk(adb, seconds, stop=None, screen=CANONICAL_SCREEN):
    """Run in the caller's thread; adb(*args) is perf_session.adb."""
    deadline = time.monotonic() + seconds
    steps = 0
    stick, backward, look_from, look_to = (scale_point(point, screen) for point in (STICK, BACKWARD, LOOK_FROM, LOOK_TO))
    while time.monotonic() < deadline and not (stop and stop.is_set()):
        hold = min(4000, max(500, int((deadline - time.monotonic()) * 1000) - 200))
        adb("shell", "input", "swipe", *stick, *backward, str(hold), timeout=hold / 1000 + 30, check=False)
        if time.monotonic() >= deadline:
            break
        adb("shell", "input", "swipe", *look_from, *look_to, "600", timeout=30, check=False)
        steps += 1
    return steps


def start_walk(adb, seconds, screen=CANONICAL_SCREEN):
    """Walk in a background thread; returns (thread, stop event, result list)."""
    stop, result = threading.Event(), []
    thread = threading.Thread(target=lambda: result.append(walk(adb, seconds, stop, screen)), daemon=True)
    thread.start()
    return thread, stop, result

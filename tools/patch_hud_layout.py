"""Original HUD movies with an aspect-aware SetRes (android/flash/hud_layout.as).

BioShock's gameswf stretches its 1280x720 HUD stage onto screens wider than 4:3.
The patched SetRes declares PortAspectLayout; native bioshock_flash_aspect.c then
gives the stage the uniform 720/h scale and calls PortHudLayout(width, interface
scale, Pause clearance), which re-anchors the original groups to the screen edges.
The original HUDPC/HUDRadial assets are read-only inputs; only SetRes is replaced.
"""
import hashlib
import json
import os
from pathlib import Path
import re
import struct
import subprocess
import tempfile
import zlib

if __package__:
    from .patch_native_minigames import parse, tag_bytes
else:
    from patch_native_minigames import parse, tag_bytes

ROOT = Path(__file__).resolve().parents[1]
SOURCES = {
    "HUDPC.swf": "579d88ecf54b9248e63dfaf519d682161fc4bf6055063063f9c4ee077129b712",
    "HUDRadial.swf": "db2f9716e490b4421981cf982d34d85f02e810446a094af311ca00947351abd1",
}
ORIGINAL_SETRES = re.compile(r"function SetRes\(XRes, YRes\)\r?\n\{\r?\n.*?\r?\n\}\r?\n?", re.S)
NEEDLES = ("_loc1_.centerBox._x = 160;", "_loc1_.rightBox._x = 320;", "_loc1_.HandsIcons._x = 160;",
           "_loc1_.PhotoDisplayBox._x = 160;", "if(YRes / XRes == 0.75)")


# Original controller HUD bug ("Use Xbox 360 Controller" with dialog subtitles): the subtitle holder
# (sprite 211: frame 1 hidden + stop, 2..24 fade in, 25 shown + stop, 26..50 fade out) restarts its
# fade-in only past frame 25 or at frame 1. The game clears a line (gotoAndPlay(25), which plays the
# fade-out) and sends the next one in the same tick: the holder is still at frame 25, so the new line
# fades out with the old one. HUDPC always restarts at frame 2; restarting from frame 25 on as well keeps
# the controller HUD's intent (no restart while a line fades in) and shows every line. The port's touch
# controls turn the game's controller mode on, so they met this bug too.
RADIAL_SUBTITLE_BEFORE = ("if(_loc1_.centerBox.subtitleTextHolder._currentframe > 25 || "
                          "_loc1_.centerBox.subtitleTextHolder._currentframe == 1)")
RADIAL_SUBTITLE_AFTER = RADIAL_SUBTITLE_BEFORE.replace("_currentframe > 25", "_currentframe >= 25")


def fix_radial_subtitles(message):
    if message.count(RADIAL_SUBTITLE_BEFORE) != 1:
        raise ValueError("HUDRadial.swf: subtitle restart preimage changed")
    return message.replace(RADIAL_SUBTITLE_BEFORE, RADIAL_SUBTITLE_AFTER, 1)


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def tools():
    machine = json.loads((Path.home() / ".agents/skills/adapt-arm32-game/machine_config.json").read_text())
    return Path(machine["java_exe"]), Path(machine["analyzers"]["ffdec_cli"]).with_suffix(".jar")


def patch(name, output):
    source = ROOT / "build/localization/Content/FlashMovies" / name
    if digest(source) != SOURCES[name]:
        raise ValueError(f"{name}: the pinned localized original changed; inputs stay read-only")
    layout = ROOT / "android/flash/hud_layout.as"
    touch = ROOT / "android/flash/hud_touch.as"
    java, ffdec = tools()
    identity = {"source": name, "source_sha256": SOURCES[name], "layout_sha256": digest(layout), "touch_sha256": digest(touch),
                "generator_sha256": digest(Path(__file__)), "ffdec_sha256": digest(ffdec)}
    receipt_path = output.with_suffix(".json")
    if output.is_file() and receipt_path.is_file():
        old = json.loads(receipt_path.read_text())
        if all(old.get(k) == v for k, v in identity.items()) and old.get("sha256") == digest(output):
            return old
    output.parent.mkdir(parents=True, exist_ok=True)

    def run(*args):
        result = subprocess.run([str(java), "-Xmx2g", "-jar", str(ffdec), "-onerror", "abort", *map(str, args)],
                                capture_output=True, text=True, encoding="utf-8", errors="replace", timeout=300)
        if result.returncode:
            raise RuntimeError(result.stdout + result.stderr)

    with tempfile.TemporaryDirectory(prefix="native-hud-", dir=output.parent) as temporary:
        stage = Path(temporary)
        run("-export", "script", stage / "export", source)
        scripts = stage / "export/scripts"
        hits = [p for p in scripts.glob("frame_1/*.as") if "function SetRes(" in p.read_text(encoding="utf-8")]
        if len(hits) != 1:
            raise ValueError(f"{name}: expected one root SetRes, found {len(hits)}")
        text = hits[0].read_text(encoding="utf-8")
        found = ORIGINAL_SETRES.search(text)
        if not found:
            raise ValueError(f"{name}: original SetRes not found")
        body = found.group(0)
        for needle in NEEDLES:
            if body.count(needle) != 1:
                raise ValueError(f"{name}: SetRes preimage changed ({needle})")
        patched = text.replace(body, "", 1) + "\n" + layout.read_text(encoding="utf-8") + "\n" + touch.read_text(encoding="utf-8")
        target = stage / "patch" / hits[0].relative_to(scripts)
        target.parent.mkdir(parents=True)
        target.write_text(patched, encoding="utf-8")
        # The gauge end caps read a child _width that this player scales by the
        # gauge's extra (interface) scale while the bars are set without it; the
        # PC HUD already divides by 0.76 for the authored 0.75. Divide that width
        # by the interface scale: at 1.0 (and without the port layout) unchanged.
        gauges = [p for p in scripts.glob("frame_1/*.as") if "HealthBGCapEnd._x = " in p.read_text(encoding="utf-8")]
        if len(gauges) != 1 or gauges[0].name == hits[0].name:
            raise ValueError(f"{name}: expected one gauge update script apart from SetRes")
        gauge = gauges[0].read_text(encoding="utf-8")
        for bar, middle in (("healthBar", "HealthBGMid"), ("bioAmmoBar", "BioAmmoBGMid")):
            needle = f"_loc1_.{bar}.{middle}._width"
            lines = [line for line in gauge.splitlines() if "CapEnd._x = " in line and needle in line]
            if len(lines) != 1 or gauge.count(lines[0]) != 1:
                raise ValueError(f"{name}: gauge cap preimage changed ({bar})")
            fixed = lines[0].replace(needle, f"{needle} / (_root.PortHudScale >= 1 ? _root.PortHudScale : 1)")
            gauge = gauge.replace(lines[0], fixed, 1)
        (stage / "patch" / gauges[0].relative_to(scripts)).write_text(gauge, encoding="utf-8")
        messages = [p for p in scripts.glob("frame_1/*.as") if "function HUDMessage(MsgType, Msg)" in p.read_text(encoding="utf-8")]
        if len(messages) != 1 or messages[0] in (hits[0], gauges[0]):
            raise ValueError(f"{name}: expected one original HUDMessage script")
        message = messages[0].read_text(encoding="utf-8")
        header = "function HUDMessage(MsgType, Msg)\n{"
        if message.count(header) != 1:
            raise ValueError(f"{name}: HUDMessage preimage changed")
        message = message.replace(header, header + "\n   _root.PortRememberContext(MsgType,Msg);\n   Msg = _root.PortPrompt(MsgType,Msg);", 1)
        if name == "HUDRadial.swf":
            message = fix_radial_subtitles(message)
        (stage / "patch" / messages[0].relative_to(scripts)).write_text(message, encoding="utf-8")
        run("-importScript", source, stage / "patched.swf", stage / "patch")
        data = (stage / "patched.swf").read_bytes()
        plain = zlib.decompress(data[8:]) if data[:3] == b"CWS" else data[8:]
        for marker in (b"PortHudLayout", b"PortAspectLayout", b"PortLayout", b"PortHudClear", b"PortTouchContext", b"PortTouchPrompts"):
            if marker not in plain:
                raise ValueError(f"{name}: missing {marker!r} after import")
        output.write_bytes(data)
    receipt = {**identity, "sha256": digest(output), "bytes": output.stat().st_size, "script": hits[0].name,
               "gauge_script": gauges[0].name, "message_script": messages[0].name,
               "change": "Original SetRes replaced by the aspect-aware layout; gauge end caps divide the scaled child "
                         "width by the interface scale; HUDMessage publishes original action availability and optional touch prompts" +
                         ("; controller HUD subtitles restart from frame 25 on (lines no longer vanish)" if name == "HUDRadial.swf" else "")}
    receipt_path.write_text(json.dumps(receipt, indent=2) + "\n", encoding="utf-8")
    return receipt


def build(directory):
    directory = Path(directory)
    return {"native-hud.swf": patch("HUDPC.swf", directory / "native-hud.swf"),
            "native-hud-radial.swf": patch("HUDRadial.swf", directory / "native-hud-radial.swf"),
            "native-picker.swf": patch_picker(directory / "native-picker.swf")}


def map_with_cyrillic(source):
    locale = ROOT / "build/localization/Content/FlashMovies"
    donor = (locale / "HUDPC.swf").read_bytes()
    if hashlib.sha256(donor).hexdigest() != SOURCES["HUDPC.swf"]:
        raise ValueError("Unexpected HUD font source")
    donors = {struct.unpack_from("<H", t["payload"])[0]: t["payload"] for t in parse(donor)[2] if t["code"] == 48}
    fonts = {b"Century Gothic": donors[6], b"Century Gothic_rus": donors[6],
             b"LHF Bell Boy": donors[156], b"Anastasia": donors[196]}
    header, _, tags = parse(source)
    replaced = 0
    output = bytearray(header)
    for tag in tags:
        raw = tag["raw"]
        if tag["code"] == 48:
            payload = tag["payload"]
            end_name = 5 + payload[4]
            name = payload[5:end_name].rstrip(b"\0 ")
            if name in fonts and struct.unpack_from("<H", payload, end_name)[0] == 0:
                # Empty device fonts rely on the Windows font library. Keep the original Flash character ID.
                raw = tag_bytes(48, payload[:2] + fonts[name][2:])
                replaced += 1
        output.extend(raw)
    if not replaced:
        raise ValueError("No map device fonts found")
    struct.pack_into("<I", output, 4, len(output))
    return bytes(output)


def build_map(output):
    game = Path(os.environ.get("BIOSHOCK_GAME", json.loads((ROOT / "config/inputs.json").read_text())["source_root"]))
    source = (game / "Content/FlashMovies/mapsPC.swf").read_bytes()
    if hashlib.sha256(source).hexdigest() != "d90c7e4e49d378c7e19c249b93499bec2d3f74a093cc160063351477efc6052a":
        raise ValueError("Unexpected original mapsPC.swf")
    output.write_bytes(map_with_cyrillic(source))


def patch_picker(output):
    source = ROOT / "build/localization/Content/FlashMovies/PCWeaponSelection.swf"
    layout = ROOT / "android/flash/touch_picker.as"
    expected = "80d34aefccf27d63b36696c03f25f8b15dfc9bcb87c952db62a420ad6219d0ec"
    if digest(source) != expected:
        raise ValueError("Original PCWeaponSelection changed")
    java, ffdec = tools()
    identity = {"source_sha256": expected, "script_sha256": digest(layout), "generator_sha256": digest(Path(__file__))}
    receipt_path = output.with_suffix(".json")
    if output.is_file() and receipt_path.is_file():
        receipt = json.loads(receipt_path.read_text())
        if all(receipt.get(k) == v for k, v in identity.items()) and receipt.get("sha256") == digest(output): return receipt
    output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="native-picker-", dir=output.parent) as temporary:
        stage = Path(temporary)
        def run(*args):
            result = subprocess.run([str(java), "-Xmx1g", "-jar", str(ffdec), *map(str, args)],
                                    capture_output=True, text=True, encoding="utf-8", errors="replace", timeout=120)
            if result.returncode: raise RuntimeError(result.stdout + result.stderr)
        run("-export", "script", stage / "export", source)
        scripts = stage / "export/scripts"
        hits = [p for p in scripts.glob("frame_1/*.as") if "function FinishedAddingWeapons(" in p.read_text(encoding="utf-8")]
        if len(hits) != 1: raise ValueError("Original weapon picker root changed")
        target = stage / "patch" / hits[0].relative_to(scripts)
        target.parent.mkdir(parents=True)
        code = hits[0].read_text(encoding="utf-8")
        finished = "   _loc1_.SetPendingAmmo(-1,-1);\n}"
        if code.count(finished) != 1:
            raise ValueError("Original picker completion changed")
        target.write_text(code.replace(finished, "   _root._visible = false;\n" + finished, 1)
                          + "\n" + layout.read_text(encoding="utf-8"), encoding="utf-8")
        run("-importScript", source, output, stage / "patch")
    receipt = {**identity, "sha256": digest(output), "bytes": output.stat().st_size}
    receipt_path.write_text(json.dumps(receipt, indent=2) + "\n")
    return receipt


if __name__ == "__main__":
    print(json.dumps(build(ROOT / "build/native-hud"), indent=2))

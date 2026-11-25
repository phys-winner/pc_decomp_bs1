"""Original HUD movies with an aspect-aware SetRes (android/flash/hud_layout.as).

BioShock's gameswf stretches its 1280x720 HUD stage onto screens wider than 4:3.
The patched SetRes declares PortAspectLayout; native bioshock_flash_aspect.c then
gives the stage the uniform 720/h scale and calls PortHudLayout(width, interface
scale, Pause clearance), which re-anchors the original groups to the screen edges.
The original HUDPC/HUDRadial assets are read-only inputs. The generated movies
also carry the controller prompt glyphs and use the original HUD text fields.
"""
import hashlib
import json
import math
import os
from pathlib import Path
import re
import struct
import subprocess
import tempfile
import zlib

if __package__:
    from .patch_native_minigames import parse, rect, tag_bytes
else:
    from patch_native_minigames import parse, rect, tag_bytes

ROOT = Path(__file__).resolve().parents[1]
SOURCES = {
    "HUDPC.swf": "579d88ecf54b9248e63dfaf519d682161fc4bf6055063063f9c4ee077129b712",
    "HUDRadial.swf": "db2f9716e490b4421981cf982d34d85f02e810446a094af311ca00947351abd1",
}
ORIGINAL_SETRES = re.compile(r"function SetRes\(XRes, YRes\)\r?\n\{\r?\n.*?\r?\n\}\r?\n?", re.S)
NEEDLES = ("_loc1_.centerBox._x = 160;", "_loc1_.rightBox._x = 320;", "_loc1_.HandsIcons._x = 160;",
           "_loc1_.PhotoDisplayBox._x = 160;", "if(YRes / XRes == 0.75)")


def glyph_shape(contours):
    bits = ["00010000"]  # one fill bit, no line styles

    def write(value, width):
        bits.append(format(value & ((1 << width) - 1), f"0{width}b"))

    for index, points in enumerate(contours):
        x, y = points[0]
        move = max(abs(x), abs(y)).bit_length() + 1
        write(3 if index == 0 else 1, 6)  # move, and select fill 1 on the first contour
        write(move, 5)
        write(x, move)
        write(y, move)
        if index == 0:
            write(1, 1)
        for next_x, next_y in points[1:] + points[:1]:
            dx, dy = next_x - x, next_y - y
            width = max(2, max(abs(dx), abs(dy)).bit_length() + 1)
            write(3, 2)  # straight edge
            write(width - 2, 4)
            write(1, 1)  # both axes
            write(dx, width)
            write(dy, width)
            x, y = next_x, next_y
    write(0, 6)
    joined = "".join(bits)
    return int(joined.ljust((len(joined) + 7) // 8 * 8, "0"), 2).to_bytes((len(joined) + 7) // 8, "big")


def controller_glyphs():
    cross = [(120, -570), (190, -640), (350, -480), (510, -640), (580, -570), (420, -410),
             (580, -250), (510, -180), (350, -340), (190, -180), (120, -250), (280, -410)]
    square = [[(80, -680), (620, -680), (620, -140), (80, -140)],
              [(160, -600), (160, -220), (540, -220), (540, -600)]]
    triangle = [[(350, -730), (660, -160), (40, -160)], [(350, -580), (175, -240), (525, -240)]]
    ring = [[(round(350 + radius * math.cos(angle * math.tau / 24)),
              round(-410 + radius * math.sin(angle * math.tau / 24))) for angle in angles]
            for radius, angles in ((345, range(24)), (286, range(23, -1, -1)))]
    def inset(points):
        return [(round(350 + (x - 350) * 0.72), round(-410 + (y + 410) * 0.72)) for x, y in points]

    letter_a = [[(99, -153), (295, -664), (405, -664), (601, -153), (485, -153),
                 (444, -267), (256, -267), (215, -153)], [(290, -362), (410, -362), (350, -529)]]
    letter_y = [[(96, -661), (225, -661), (350, -451), (475, -661), (604, -661),
                 (407, -338), (407, -154), (293, -154), (293, -338)]]
    # Private-use codes greater than the pinned fonts' U+2183 tail. Appending leaves the original glyph indices intact.
    return {0xE060 + i: glyph_shape(contours) for i, contours in enumerate((
        [cross], square, triangle, ring + list(map(inset, letter_a)), ring + [inset(cross)],
        ring + list(map(inset, letter_y)),
    ))}


def append_font_glyphs(font, glyphs):
    count_at = 5 + font[4]
    count, = struct.unpack_from("<H", font, count_at)
    if not count:
        return font
    if font[2] & 0x8C != 0x8C:
        raise ValueError("HUD glyph insertion requires wide offsets, wide codes and font metrics")
    table = count_at + 2
    offsets = struct.unpack_from(f"<{count + 1}I", font, table)
    code_at = table + offsets[-1]
    codes = struct.unpack_from(f"<{count}H", font, code_at)
    if offsets[0] != 4 * (count + 1) or tuple(sorted(codes)) != codes or codes[-1] >= min(glyphs):
        raise ValueError("Unexpected HUD font offsets or glyph ordering")
    shapes = bytearray(font[table + offsets[0]:code_at])
    first = offsets[0] + 4 * len(glyphs)
    positions = [offset + 4 * len(glyphs) for offset in offsets[:-1]]
    for shape in glyphs.values():
        positions.append(first + len(shapes))
        shapes.extend(shape)
    positions.append(first + len(shapes))
    metrics_at = code_at + 2 * count
    bounds_at = metrics_at + 6 + 2 * count
    metrics_end = bounds_at
    for _ in range(count):
        _, metrics_end = rect(font, metrics_end)
    return (font[:count_at] + struct.pack("<H", count + len(glyphs)) + struct.pack(f"<{len(positions)}I", *positions)
            + shapes + font[code_at:metrics_at] + struct.pack(f"<{len(glyphs)}H", *glyphs)
            + font[metrics_at:bounds_at] + struct.pack(f"<{len(glyphs)}h", *([720] * len(glyphs)))
            + font[bounds_at:metrics_end] + b"\x08\x00" * len(glyphs) + font[metrics_end:])


def add_controller_glyphs(movie):
    glyphs = controller_glyphs()
    header, _, tags = parse(movie)
    output = bytearray(header)
    for tag in tags:
        if tag["code"] == 48:
            output.extend(tag_bytes(48, append_font_glyphs(tag["payload"], glyphs), tag["long"]))
        else:
            output.extend(tag["raw"])
    struct.pack_into("<I", output, 4, len(output))
    return b"CWS" + output[3:8] + zlib.compress(output[8:]) if movie[:3] == b"CWS" else bytes(output)


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


def fix_container_slot_lifecycle(message):
    """Keep the three loot-slot labels inside the actual OpenContainer/CloseContainer interval."""
    for name, assignment in (("OpenContainer", "   _root.PortContainerOpen = true;\n"),
                            ("CloseContainer", "   _root.PortContainerOpen = false;\n")):
        header = f"function {name}()\n{{"
        if message.count(header) != 1:
            raise ValueError(f"HUDPC.swf: {name} preimage changed")
        message = message.replace(header, header + "\n" + assignment.rstrip("\n"), 1)
    close = """function CloseContainer()
{
   _root.PortContainerOpen = false;
   _root.centerBox.focusArea.gotoAndPlay(105);"""
    close_clear = """function CloseContainer()
{
   _root.PortContainerOpen = false;
   for(var PortSlot = 0; PortSlot < 3; PortSlot++)
   {
      _root.centerBox.focusArea["itemName" + PortSlot]["containerItem" + PortSlot] = "";
   }
   _root.centerBox.focusArea.gotoAndPlay(105);"""
    if message.count(close) != 1:
        raise ValueError("HUDPC.swf: CloseContainer body changed")
    message = message.replace(close, close_clear, 1)
    setter = """function SetContainerItem(index, id, friendlyName, count)
{
   var _loc1_ = index;"""
    setter_guard = """function SetContainerItem(index, id, friendlyName, count)
{
   if(!_root.PortContainerOpen) return;
   var _loc1_ = index;"""
    if message.count(setter) != 1:
        raise ValueError("HUDPC.swf: SetContainerItem preimage changed")
    return message.replace(setter, setter_guard, 1)


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
        # The iOS pad replaces the PC quick-selection strip, including wheel-triggered reveals.
        reveals = 0
        for original in scripts.glob("frame_1/*.as"):
            data = original.read_text(encoding="utf-8")
            count = data.count(".HandsIcons._visible = true;")
            if not count:
                continue
            reveals += count
            destination = stage / "patch" / original.relative_to(scripts)
            if destination.exists():
                data = destination.read_text(encoding="utf-8")
            destination.parent.mkdir(parents=True, exist_ok=True)
            destination.write_text(data.replace(".HandsIcons._visible = true;",
                ".HandsIcons._visible = _root.PortTouchPromptMode != 4;"), encoding="utf-8")
        if reveals != 4:
            raise ValueError(f"{name}: expected four original quick-selection reveals, got {reveals}")
        ammo_functions = [p for p in scripts.glob("frame_1/*.as")
                          if "function AmmoSelectionMenu(movementDirection)" in p.read_text(encoding="utf-8")]
        if len(ammo_functions) != 1:
            raise ValueError(f"{name}: expected one original ammo selection handler")
        ammo_source = ammo_functions[0]
        ammo_target = stage / "patch" / ammo_source.relative_to(scripts)
        ammo_text = (ammo_target if ammo_target.exists() else ammo_source).read_text(encoding="utf-8")
        ammo_target.write_text(ammo_text.replace("function AmmoSelectionMenu(movementDirection)",
            "function PortOriginalAmmoSelectionMenu(movementDirection)"), encoding="utf-8")
        ammo_counts = [p for p in scripts.glob("frame_1/*.as")
                       if "function MakeCurrentAmmoSelectionVisible()" in p.read_text(encoding="utf-8")]
        if len(ammo_counts) != 1:
            raise ValueError(f"{name}: expected one ammunition count refresh handler")
        counts_source = ammo_counts[0]
        counts_target = stage / "patch" / counts_source.relative_to(scripts)
        counts_text = (counts_target if counts_target.exists() else counts_source).read_text(encoding="utf-8")
        counts_target.write_text(counts_text.replace("function MakeCurrentAmmoSelectionVisible()\n{",
            "function MakeCurrentAmmoSelectionVisible()\n{\n   _root.PortAmmoCounts();"), encoding="utf-8")
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
        containers = [p for p in scripts.glob("frame_1/*.as")
                      if "function SetContainerItem(index, id, friendlyName, count)" in p.read_text(encoding="utf-8")]
        if len(containers) > 1 or (name == "HUDPC.swf" and len(containers) != 1):
            raise ValueError(f"{name}: unexpected container item script count {len(containers)}")
        if containers:
            if containers[0] in (hits[0], gauges[0], messages[0]):
                raise ValueError(f"{name}: container item script overlaps another patched script")
            container_script = fix_container_slot_lifecycle(containers[0].read_text(encoding="utf-8"))
            (stage / "patch" / containers[0].relative_to(scripts)).write_text(container_script, encoding="utf-8")
            container_script_name = containers[0].name
        else:
            container_script_name = None
        run("-importScript", source, stage / "patched.swf", stage / "patch")
        data = (stage / "patched.swf").read_bytes()
        plain = zlib.decompress(data[8:]) if data[:3] == b"CWS" else data[8:]
        markers = [b"PortHudLayout", b"PortAspectLayout", b"PortLayout", b"PortHudClear", b"PortTouchContext", b"PortTouchPrompts"]
        markers.extend(chr(code).encode("utf-8") for code in range(0xE060, 0xE066))
        if container_script_name: markers.append(b"PortContainerOpen")
        for marker in markers:
            if marker not in plain:
                raise ValueError(f"{name}: missing {marker!r} after import")
        output.write_bytes(add_controller_glyphs(data))
    receipt = {**identity, "sha256": digest(output), "bytes": output.stat().st_size, "script": hits[0].name,
               "gauge_script": gauges[0].name, "message_script": messages[0].name,
               "container_script": container_script_name,
               "change": "Original SetRes replaced by the aspect-aware layout; gauge end caps divide the scaled child "
                         "width by the interface scale; HUDMessage publishes action availability, touch prompts and embedded controller glyphs; "
                         "container item labels clear on close and late slot updates are ignored" +
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

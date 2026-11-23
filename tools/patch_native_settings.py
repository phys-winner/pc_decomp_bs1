"""Extend the original BioShock Flash Graphics page with its own widgets and skin."""
import copy
import hashlib
import json
from pathlib import Path
import re
import struct
import subprocess
import tempfile
import xml.etree.ElementTree as ET
import zlib

ROOT = Path(__file__).resolve().parents[1]
SOURCE_SHA256 = "52bd0483a21dcb2de7e94c40e88f150bfa4473d53c34ccb8afe3e980f34c024a"
EDITOR_FILES = ("native-editor-button-up.png", "native-editor-button-over.png",
                "native-editor-button-down.png", "native-editor-font.ttf")
PORT_DEPTHS = (420, 422, 423, 424, 425)
PORT_NAVIGATION = (
    '\nfunction PortLanguage(Code,Unused){ _root.PortGameLanguage=Code; _root.PortRussian=Code=="rus"; }\n'
    "\nfunction PortOpenSection(Page){ var Frame=Page==2?_root.SubOptionsScreen2:_root.SubOptionsScreen1; "
    "if(_root._currentframe==Frame){_root.PortShow(Page);}else{_root.PortRequested=Page; "
    "if(Page==2){_root.SwitchToSubOptionsScreen2();}else{_root.SwitchToSubOptionsScreen1();}} }\n"
    "function PortOpenAdditional(){ _root.PortFromAdditional=true; _root.PortOpenSection(3); }\n")


def responsive_release(source):
    """Commit the original option on release, before another Flash Tick changes focus."""
    source = re.sub(r'_root\.guiVal = "(?:TRUE|FALSE)";', '_root.UIHighlightRight();', source)
    if '_root.guiVal = "NULL";' in source and '_root.sliderIdx = -1;' in source:
        source = source.replace('_root.guiVal = "NULL";', '_root.HandleGuiUpdate();\n   _root.guiVal = "NULL";')
    return source


def responsive_slider_press(source, row):
    needle = '_root.guiVal = "SLIDER";'
    if source.count(needle) != 1:
        raise ValueError("Original slider press preimage changed")
    return source.replace(needle, f'_root.selectorPos = {row};\n'
                          '   _root.ScrollBar0._y = _root.SlotHolder["Slot" + _root.selectorPos]._y;\n   ' + needle)


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def placed_bounds(node, characters, matrix="matrix", hit_test=False):
    box = character_bounds(node.get("characterId"), characters, hit_test)
    transform = node.find(matrix)
    sx = float(transform.get("scaleX", 1)) if transform.get("hasScale") == "true" else 1
    sy = float(transform.get("scaleY", 1)) if transform.get("hasScale") == "true" else 1
    if transform.get("hasRotate") == "true" or sx == 0 or sy == 0:
        raise ValueError("Unsupported native-widget geometry")
    x, y = float(transform.get("translateX", 0)), float(transform.get("translateY", 0))
    left, right = sorted((box[0] * sx + x, box[2] * sx + x))
    top, bottom = sorted((box[1] * sy + y, box[3] * sy + y))
    return left, top, right, bottom


def character_bounds(ident, characters, hit_test=False):
    node = characters[ident]
    bounds = node.find("shapeBounds")
    if bounds is None:
        bounds = node.find("bounds")
    if bounds is not None:
        return tuple(float(bounds.get(k)) for k in ("Xmin", "Ymin", "Xmax", "Ymax"))
    children = node.find("subTags")
    if children is None:
        children = node.find("characters")
    boxes, matrices = [], {}
    for child in children:
        typ, depth = child.get("type", ""), child.get("depth")
        if typ.startswith("RemoveObject"):
            matrices.pop(depth, None)
        elif typ.startswith("PlaceObject") or typ == "BUTTONRECORD":
            if hit_test and typ == "BUTTONRECORD" and child.get("buttonStateHitTest") != "true":
                continue
            matrix = "placeMatrix" if typ == "BUTTONRECORD" else "matrix"
            transform = child.find(matrix)
            if transform is not None:
                matrices[depth] = transform
            if child.get("characterId"):
                placed = copy.deepcopy(child)
                if transform is None:
                    placed.append(copy.deepcopy(matrices[depth]))
                boxes.append(placed_bounds(placed, characters, matrix, hit_test))
    if not boxes:
        return 0, 0, 0, 0
    return min(b[0] for b in boxes), min(b[1] for b in boxes), max(b[2] for b in boxes), max(b[3] for b in boxes)


def slider_bounds(tags, characters, exported):
    """Use the original slider input buttons, not their offset measuring clips."""
    sliders = {}
    for path in exported.glob("DefineButton2_*/BUTTONCONDACTION on(press).as"):
        if '_root.guiVal = "SLIDER";' not in path.read_text(encoding="utf-8"):
            continue
        rollover = (path.parent / "BUTTONCONDACTION on(rollOver).as").read_text(encoding="utf-8")
        rows = re.findall(r"_root\.selectorPos = (\d+);", rollover)
        if len(rows) != 1:
            raise ValueError("Original slider row preimage changed: " + str(path))
        sliders[path.parent.name.removeprefix("DefineButton2_")] = int(rows[0])
    if len(sliders) != 10:
        raise ValueError("Original PausePC slider button count changed")
    frame, placements, result = 1, {}, {}
    for node in tags:
        typ, depth = node.get("type", ""), node.get("depth")
        if typ.startswith("RemoveObject"):
            placements.pop(depth, None)
        elif typ.startswith("PlaceObject"):
            placed = copy.deepcopy(node)
            previous = placements.get(depth)
            if previous is not None:
                if not placed.get("characterId"):
                    placed.set("characterId", previous.get("characterId"))
                if placed.find("matrix") is None:
                    placed.append(copy.deepcopy(previous.find("matrix")))
            placements[depth] = placed
        elif typ == "ShowFrameTag":
            rows = {}
            for placed in placements.values():
                ident = placed.get("characterId")
                if ident in sliders:
                    row = sliders[ident]
                    if row in rows:
                        raise ValueError(f"Multiple original slider inputs for frame {frame}, row {row}")
                    rows[row] = tuple(value / 20 for value in placed_bounds(placed, characters, hit_test=True))
            result[frame] = rows
            frame += 1
    return result


def slider_bounds_script(frames):
    # Native get_variable resolves dotted prefixes as display-list clips, not AS objects.
    source = "\n"
    for frame, rows in sorted(frames.items()):
        for row, bounds in sorted(rows.items()):
            for key, value in zip(("left", "top", "right", "bottom"), bounds):
                source += f"_root.PortSlider{frame}_{row}_{key} = {value:.9f};\n"
    return source


def name_confirm_buttons(characters):
    children = [node for node in characters["81"].find("subTags") if node.get("characterId") == "73"]
    if len(children) != 1 or children[0].get("name"):
        raise ValueError("Original ConfirmBox buttons preimage changed")
    children[0].set("name", "PortButtons")
    children[0].set("placeFlagHasName", "true")


# D6: on the Graphics frame (7) the bottom row (APPLY, DISCARD, ADJUST BRIGHTNESS and their captions) is
# 10 px lower than on every other screen: 691..724 of the 720-line stage, so a screen that shows the whole
# stage height (2400x1080 and other wide phones) cuts it at the bottom edge. It moves to the row of the other
# screens (681..714). Depth -> original translateY in twips: buttons 708.1 px, captions 700.85 px.
GRAPHICS_BOTTOM_ROW = {"5": 14162, "142": 14017, "143": 14162, "145": 14017, "146": 14162, "148": 14017}
GRAPHICS_ROW_LIFT = 200


def lift_graphics_bottom_row(tags):
    frame, moved = 1, []
    for tag in tags:
        typ = tag.get("type", "")
        if typ == "ShowFrameTag":
            frame += 1
            continue
        if frame == 7 and typ.startswith("PlaceObject") and tag.get("depth") in GRAPHICS_BOTTOM_ROW:
            matrix = tag.find("matrix")
            if matrix is None or int(matrix.get("translateY", "0")) != GRAPHICS_BOTTOM_ROW[tag.get("depth")]:
                raise ValueError("Original Graphics bottom row changed")
            matrix.set("translateY", str(GRAPHICS_BOTTOM_ROW[tag.get("depth")] - GRAPHICS_ROW_LIFT))
            moved.append(tag.get("depth"))
    if sorted(moved, key=int) != sorted(GRAPHICS_BOTTOM_ROW, key=int):
        raise ValueError("Original Graphics bottom row changed")


def validate_port_depths(tags):
    occupied = {int(node.get("depth")) for node in tags if node.get("depth") is not None}
    modal = [int(node.get("depth")) for node in tags if node.get("name") == "ConfirmBox"]
    if len(modal) != 1 or set(PORT_DEPTHS) & occupied or max(PORT_DEPTHS) >= modal[0]:
        raise ValueError("Port widget depths must be unused and below the original ConfirmBox")


def editor_bitmap(payload):
    """Decode the original PausePC tag512 BC3 bitmap; no raster-art dependency."""
    if len(payload) != 8202:
        raise ValueError("Original editor bitmap byte count changed")
    ident, width, height, fmt = struct.unpack_from("<HHHI", payload)
    if ident not in (63, 65, 67) or (width, height, fmt) != (256, 32, 4):
        raise ValueError("Original editor bitmap header changed")
    # Original10D765CA..10D765E3 maps image format4 to DXT5. Match the
    # production dxt_decode.h integer expansion/interpolation exactly.
    rgba = bytearray(width * height * 4)
    for by in range(height // 4):
        for bx in range(width // 4):
            block = payload[10 + (by * (width // 4) + bx) * 16:][:16]
            a, b = block[:2]
            alphas = [a, b]
            alphas += ([(a * (7 - i) + b * (i - 1) + 3) // 7 for i in range(2, 8)] if a > b else
                       [(a * (6 - i) + b * (i - 1) + 2) // 5 for i in range(2, 6)] + [0, 255])
            colors = []
            for value in struct.unpack_from("<HH", block, 8):
                r, g, blue = value >> 11, (value >> 5) & 63, value & 31
                colors.append(((r << 3) | (r >> 2), (g << 2) | (g >> 4), (blue << 3) | (blue >> 2)))
            colors += [tuple((colors[0][c] * aw + colors[1][c] * bw + 1) // 3 for c in range(3))
                       for aw, bw in ((2, 1), (1, 2))]
            alpha_bits, color_bits = int.from_bytes(block[2:8], "little"), int.from_bytes(block[12:], "little")
            for i in range(16):
                at = ((by * 4 + i // 4) * width + bx * 4 + i % 4) * 4
                rgba[at:at + 4] = bytes((*colors[(color_bits >> (i * 2)) & 3], alphas[(alpha_bits >> (i * 3)) & 7]))
    return bytes(rgba)


def editor_png(rgba):
    if len(rgba) != 256 * 32 * 4:
        raise ValueError("Invalid editor RGBA size")

    def chunk(kind, data):
        return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data))

    scanlines = b"".join(b"\0" + rgba[y * 1024:(y + 1) * 1024] for y in range(32))
    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", 256, 32, 8, 6, 0, 0, 0)) +
            chunk(b"IDAT", zlib.compress(scanlines, 9)) + chunk(b"IEND", b""))


def export_editor_assets(root, run, source, stage, destination):
    tags = root.find("tags")
    images = {struct.unpack_from("<H", bytes.fromhex(tag.get("unknownData")))[0]:
              bytes.fromhex(tag.get("unknownData")) for tag in tags
              if tag.get("type") == "UnknownTag" and tag.get("tagId") == "512"}
    font = next(tag for tag in tags if tag.get("type") == "DefineFont2Tag" and tag.get("fontID") == "5")
    codes = {int(item.text) for item in font.find("codeTable")}
    required = ("Готово Сбросить? Перетаскивайте кнопки. Сброс дважды возвращает исходную раскладку "
                "Огонь Оружие плазмид Прыжок Присесть Действие Зарядить EVE выбор Ещё Пауза Done Reset Fire")
    if not set(map(ord, required)) <= codes:
        raise ValueError("Original editor font lacks required Latin/Cyrillic glyphs")
    run("-selectid", "5", "-format", "font:ttf", "-export", "font", stage / "editor-font", source)
    exported = list((stage / "editor-font").glob("5_*.ttf"))
    if len(exported) != 1 or exported[0].read_bytes()[:4] != b"\0\1\0\0":
        raise ValueError("Original editor font export failed")
    records = {}
    for name, ident in zip(EDITOR_FILES[:3], (63, 65, 67)):
        rgba = editor_bitmap(images[ident])
        (destination / name).write_bytes(editor_png(rgba))
        records[name] = {"sha256": digest(destination / name), "bytes": (destination / name).stat().st_size,
                         "bitmap_id": ident, "size": [256, 32], "rgba_sha256": hashlib.sha256(rgba).hexdigest(),
                         "original_payload_sha256": hashlib.sha256(images[ident]).hexdigest()}
    (destination / EDITOR_FILES[3]).write_bytes(exported[0].read_bytes())
    records[EDITOR_FILES[3]] = {"sha256": digest(destination / EDITOR_FILES[3]),
                               "bytes": (destination / EDITOR_FILES[3]).stat().st_size, "font_id": 5,
                               "font_name": font.get("fontName"), "glyph_count": len(codes)}
    return {"source": "Original Russian PausePC button227 states (bitmaps63/65/67), font5",
            "files": records}


def build(output):
    source = ROOT / "build/localization/Content/FlashMovies/pausePC.swf"
    script = ROOT / "android/flash/port_settings.as"
    if digest(source) != SOURCE_SHA256:
        raise ValueError("Native settings require the pinned Russian PausePC.swf; original input stays read-only")
    machine = json.loads((Path.home() / ".agents/skills/adapt-arm32-game/machine_config.json").read_text())
    java = Path(machine["java_exe"])
    ffdec = Path(machine["analyzers"]["ffdec_cli"]).with_suffix(".jar")
    identity = {"source_sha256": SOURCE_SHA256, "patch_sha256": digest(script),
                "generator_sha256": digest(Path(__file__)), "ffdec_sha256": digest(ffdec), "language": "RUS"}
    receipt_path = output.with_suffix(".json")
    if output.is_file() and receipt_path.is_file():
        old = json.loads(receipt_path.read_text())
        files = old.get("editor_assets", {}).get("files", {})
        assets_valid = set(files) == set(EDITOR_FILES) and all((output.parent / name).is_file() and
            digest(output.parent / name) == files[name].get("sha256") for name in EDITOR_FILES)
        if (all(old.get(k) == v for k, v in identity.items()) and old.get("sha256") == digest(output) and assets_valid):
            return old
    output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="native-settings-", dir=output.parent) as temporary:
        stage = Path(temporary)

        def run(*args):
            result = subprocess.run([str(java), "-Xmx2g", "-jar", str(ffdec), "-onerror", "abort", *map(str, args)],
                                    capture_output=True, text=True, encoding="utf-8", errors="replace", timeout=120)
            if result.returncode:
                raise RuntimeError(result.stdout + result.stderr)

        run("-export", "script", stage / "export", source)
        run("-swf2xml", source, stage / "source.xml")
        tree = ET.parse(stage / "source.xml")
        root = tree.getroot()
        tags = root.find("tags")
        definitions = {t.get("spriteId", t.get("buttonId", t.get("characterID", t.get("shapeId")))): t
                       for t in tags if t.get("type", "").startswith("Define")}
        if definitions["227"].get("type") != "DefineButton2Tag" or definitions["253"].get("variableName") != "ApplyGraphicsChanges":
            raise ValueError("Original Flash widget preimage changed")
        validate_port_depths(tags)
        lift_graphics_bottom_row(tags)
        frame_slider_bounds = slider_bounds(tags, definitions, stage / "export/scripts")
        name_confirm_buttons(definitions)
        editor_assets = export_editor_assets(root, run, source, stage, output.parent)
        ids = [int(v) for t in tags for k, v in t.attrib.items()
               if k in ("spriteId", "buttonId", "characterID", "shapeId", "fontID", "fontId") and v.isdecimal()]
        next_id = max(ids) + 1
        added = []
        patches = stage / "patch"

        def allocate():
            nonlocal next_id
            result = next_id
            next_id += 1
            if result >= 65535:
                raise ValueError("SWF character ID overflow")
            return result

        def action(path, text):
            target = patches / path
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_text(text, encoding="utf-8")

        def button(release, rollover):
            node = copy.deepcopy(definitions["227"])
            ident = allocate()
            node.set("buttonId", str(ident))
            added.append(node)
            for event, body in (("release", release), ("rollOver", rollover)):
                action(f"DefineButton2_{ident}/BUTTONCONDACTION on({event}).as", "on(" + event + "){\n" + body + "\n}\n")
            return ident

        def text(variable, width=600, height=28):
            node = copy.deepcopy(definitions["253"])
            ident = allocate()
            node.set("characterID", str(ident))
            node.set("variableName", variable)
            node.set("initialText", "")
            node.set("html", "false")
            node.find("bounds").set("Xmin", "0")
            node.find("bounds").set("Xmax", str(width * 20))
            node.find("bounds").set("Ymax", str(height * 20))
            added.append(node)
            return ident

        def place(character, depth, x=0, y=0, name=None, sx=1, sy=1):
            node = ET.Element("item", {"type": "PlaceObject2Tag", "characterId": str(character), "depth": str(depth),
                "forceWriteAsLong": "true", "placeFlagHasCharacter": "true", "placeFlagHasClipActions": "false",
                "placeFlagHasClipDepth": "false", "placeFlagHasColorTransform": "false", "placeFlagHasMatrix": "true",
                "placeFlagHasName": str(name is not None).lower(), "placeFlagHasRatio": "false", "placeFlagMove": "false"})
            if name is not None:
                node.set("name", name)
            ET.SubElement(node, "matrix", {"type": "MATRIX", "hasRotate": "false", "hasScale": str(sx != 1 or sy != 1).lower(),
                "scaleX": str(sx), "scaleY": str(sy), "rotateSkew0": "0", "rotateSkew1": "0",
                "translateX": str(round(x * 20)), "translateY": str(round(y * 20))})
            return node

        def sprite(children):
            ident = allocate()
            node = ET.Element("item", {"type": "DefineSpriteTag", "spriteId": str(ident), "frameCount": "1",
                                      "hasEndTag": "true", "forceWriteAsLong": "true"})
            sub = ET.SubElement(node, "subTags")
            sub.extend(children)
            ET.SubElement(sub, "item", {"type": "ShowFrameTag", "forceWriteAsLong": "false"})
            added.append(node)
            return ident

        def native_button(caption, release, rollover, width=250, height=60, focus=False):
            hit = button(release, rollover)
            field = text(caption, width)
            children = [place(hit, 1, -3.5 * width / 257, .5 * height / 33, sx=width / 257, sy=height / 33),
                        place(field, 2, -width / 2, -9)]
            if focus:
                children.insert(0, place(focus_skin, 0, name="Focus", sx=width / 256, sy=height / 32))
            return sprite(children)

        # Original sprite 258 is a text label; the APPLY hover shape is the actual focus skin.
        focus_skin = sprite([place(66, 1, -4, 1)])
        # Six rows per column leave room for every touch preference above the status line.
        rows = []
        fov_sliders = {}
        for row in range(12):
            hover = f"_root.PortFocus({row});"
            center = native_button("Caption", f"_root.PortAdjust({row},1);", hover, 300, 56)
            left = native_button("Caption", f"_root.PortAdjust({row},-1);", hover, 60, 56)
            right = native_button("Caption", f"_root.PortAdjust({row},1);", hover, 60, 56)
            children = [place(focus_skin, 1, name="Focus", sx=440 / 256, sy=56 / 32), place(center, 2, name="Value"),
                        place(left, 3, -190, name="Left", sx=-1), place(right, 4, 190, name="Right")]
            if row in (3, 7):
                hit = copy.deepcopy(definitions["278"])
                hit_id = allocate()
                hit.set("buttonId", str(hit_id))
                hit_record = next(r for r in definitions["227"].find("characters") if r.get("buttonStateHitTest") == "true")
                hit.find("characters")[:] = [copy.deepcopy(hit_record)]
                events = hit.find("actions")
                outside = copy.deepcopy(events[1])
                outside.set("condOverDownToOverUp", "false")
                outside.set("condOutDownToIdle", "true")
                events.append(outside)
                added.append(hit)
                for event, body in (("press", f"_root.PortFovPress({row});"), ("rollOver", hover),
                                    ("release", "_root.PortFovRelease();"), ("rollOut", "_root.PortFovRelease();"),
                                    ("releaseOutside", "_root.PortFovRelease();")):
                    action(f"DefineButton2_{hit_id}/BUTTONCONDACTION on({event}).as", f"on({event}){{\n{body}\n}}\n")
                fov_hit = place(hit_id, 1, -3.5 * 300 / 257, 0.5 * 56 / 33, sx=300 / 257, sy=56 / 33)
                thumb = sprite([place(66, 1, -.22, .4, sx=.055, sy=.4)])
                fov = sprite([fov_hit, place(text("Caption", 300), 2, -150, -21),
                              place(118, 3, 0, 12, sx=252 / 749, sy=.32), place(thumb, 4, 0, 15, "Thumb")])
                fov_sliders[row] = (fov_hit, 410 if row == 3 else 870, 480)
                children.append(place(fov, 5, name="Fov"))
            row_sprite = sprite(children)
            # Placed where the two-column layout puts them; the script moves them per section.
            rows.append(place(row_sprite, 10 + row, 410 if row < 6 else 870, 312 + (row % 6) * 56, "Row" + str(row)))

        def label(width):
            return sprite([place(text("Caption", width), 1, -width / 2, -9)])

        back_button = native_button("Caption", "_root.CancelOrExit();", "_root.PortFocus(_root.PortKeys.length);", focus=True)
        panel = sprite([place(text("Header", 800), 1, 240, 210),
                        place(label(400), 2, 410, 272, "ColumnLeft"), place(label(400), 3, 870, 272, "ColumnRight"),
                        *rows,
                        place(text("Status", 900, 30), 30, 190, 624),
                        place(back_button, 31, 640, 688, "Back")])
        # Each section's own entry, in the free slot right of its original buttons.
        sections = {7: {"page": 1, "keep": ("225", "288", "289"), "selections": 14, "depths": PORT_DEPTHS[:2],
                        "entry": native_button("Caption", "_root.PortOpenSection(1);", "_root.selectorPos=-1;", 250, 36)},
                    8: {"page": 2, "keep": ("168", "306", "307"), "selections": 4, "depths": PORT_DEPTHS[2:4],
                        "entry": native_button("Caption", "_root.PortOpenSection(2);", "_root.selectorPos=-1;", 250, 36)}}
        # The Options rows use a two-frame text/shadow skin, not the APPLY widget.
        options_label = copy.deepcopy(definitions["216"])
        options_label_id = allocate()
        options_label.set("spriteId", str(options_label_id))
        for original in ("213", "214", "215"):
            field = copy.deepcopy(definitions[original])
            field_id = allocate()
            field.set("characterID", str(field_id))
            field.set("variableName", "Caption")
            field.set("initialText", "")
            added.append(field)
            for node in options_label.iter():
                if node.get("characterId") == original: node.set("characterId", str(field_id))
        added.append(options_label)
        options_hit = button("_root.PortOpenAdditional();", "_root.selectorPos=4;")
        hit_records = added[-1].find("characters")
        for record in list(hit_records):
            if record.get("buttonStateHitTest") != "true": hit_records.remove(record)
        options_entry = sprite([place(options_label_id, 1, -24, -16, "Label", sx=.75, sy=.75),
                                place(options_hit, 2, -7.25, .67, sx=532 / 257, sy=44 / 33)])
        options_action = None
        for section in sections.values():
            section["names"], section["action"] = [], None
        frame, frame_start = 1, {1: 0}
        inherited_characters, inherited_names = {}, {}
        for index, tag in enumerate(list(tags)):
            typ = tag.get("type")
            if typ == "ShowFrameTag":
                frame += 1
                frame_start[frame] = index + 1
                continue
            depth = tag.get("depth")
            section = sections.get(frame)
            if frame == 6 and typ == "DoActionTag":
                options_action = index
            if section and typ.startswith("PlaceObject") and tag.get("characterId") not in section["keep"]:
                if not tag.get("characterId") and (frame == 7 or not inherited_names.get(depth)):
                    # A matrix-only move does not rename the inherited Back button in this player.
                    tag.set("characterId", inherited_characters[depth])
                    tag.set("placeFlagHasCharacter", "true")
                if tag.get("characterId"):
                    name = tag.get("name", "PortOriginal" + depth)
                    tag.set("name", name)
                    tag.set("placeFlagHasName", "true")
                else:
                    # Input keeps its inherited, script-named SingleTab rows; hide them by name.
                    name = inherited_names[depth]
                section["names"].append(name)
            if section and typ == "DoActionTag":
                section["action"] = index
            if typ.startswith("RemoveObject"):
                inherited_characters.pop(depth, None)
                inherited_names.pop(depth, None)
            elif typ.startswith("PlaceObject"):
                if tag.get("characterId"):
                    inherited_characters[depth] = tag.get("characterId")
                    inherited_names[depth] = tag.get("name")
                elif tag.get("name"):
                    inherited_names[depth] = tag.get("name")
        if any(s["action"] is None for s in sections.values()) or options_action is None or 9 not in frame_start:
            raise ValueError("Missing original Graphics/Input frame")
        placements = {}
        for frame, section in sections.items():
            placements[frame] = (place(section["entry"], section["depths"][0], 1152, 690, "PortEntry"),
                                 place(panel, section["depths"][1], name="PortPanel"))
        def remove(depth):
            return ET.Element("item", {"type": "RemoveObject2Tag", "depth": str(depth), "forceWriteAsLong": "false"})

        options_placement = place(options_entry, PORT_DEPTHS[4], 640, 560, "PortOptionsEntry")
        insertions = {options_action: [*added, options_placement], frame_start[7]: [remove(PORT_DEPTHS[4])],
                      sections[7]["action"]: list(placements[7]), frame_start[8]: [remove(d) for d in sections[7]["depths"]],
                      sections[8]["action"]: list(placements[8]), frame_start[9]: [remove(d) for d in sections[8]["depths"]]}
        for index, nodes in sorted(insertions.items(), reverse=True):
            for node in reversed(nodes):
                tags.insert(index, node)

        characters = dict(definitions)
        for node in added:
            characters[node.get("spriteId", node.get("buttonId", node.get("characterID")))] = node

        layout = {"entry": placed_bounds(placements[7][0], characters), "input_entry": placed_bounds(placements[8][0], characters),
                  "options_entry": placed_bounds(options_placement, characters), "panel": placed_bounds(placements[7][1], characters)}
        for bounds in layout.values():
            if not (0 <= bounds[0] < bounds[2] <= 25600 and 0 <= bounds[1] < bounds[3] <= 14400):
                raise ValueError("Additional settings leave the original 1280x720 viewport")
        frame, depth_characters = 1, {}
        for node in tags:
            if node.get("type") == "ShowFrameTag":
                frame += 1
            if node.get("type", "").startswith("RemoveObject"):
                depth_characters.pop(node.get("depth"), None)
            elif node.get("type", "").startswith("PlaceObject") and node.get("characterId"):
                depth_characters[node.get("depth")] = node.get("characterId")
            if frame == 6 and node.get("name", "").startswith("Slot_F6_"):
                box = placed_bounds(node, characters)
                entry_box = layout["options_entry"]
                if entry_box[0] < box[2] and entry_box[2] > box[0] and entry_box[1] < box[3] and entry_box[3] > box[1]:
                        raise ValueError(f"Additional settings {layout['options_entry']} overlap original Options row {box}")
            section = sections.get(frame)
            if section and node.get("type", "").startswith("PlaceObject") and node.get("name") in section["names"]:
                placed = copy.deepcopy(node)
                placed.set("characterId", depth_characters[node.get("depth")])
                if placed.find("matrix") is None:
                    continue
                box = placed_bounds(placed, characters)
                entry_box = layout["entry" if frame == 7 else "input_entry"]
                if box[0] == box[2] or box[1] == box[3]:
                    continue
                if entry_box[0] < box[2] and entry_box[2] > box[0] and entry_box[1] < box[3] and entry_box[3] > box[1]:
                    raise ValueError(f"Port entry overlaps an original control on frame {frame}")
        tree.write(stage / "widgets.xml", encoding="utf-8", xml_declaration=True)
        run("-xml2swf", stage / "widgets.xml", stage / "widgets.swf")
        exported = stage / "export/scripts"
        common = (exported / "frame_1/DoAction_2.as").read_text(encoding="utf-8")
        for name in ("UIHighlightUp", "UIHighlightDown"):
            if common.count("function " + name + "(") != 1:
                raise ValueError("Common navigation callback preimage changed: " + name)
            common += "\n_root.PortOriginal_" + name + "=_root." + name + ";\n"
        common += PORT_NAVIGATION + slider_bounds_script(frame_slider_bounds)
        for row, (hit, x, y) in fov_sliders.items():
            for edge, value, origin in zip(("left", "top", "right", "bottom"), placed_bounds(hit, characters, hit_test=True), (x, y, x, y)):
                common += f"_root.PortFovSlider{row}_{edge} = {value / 20 + origin:.9f};\n"
        action("frame_1/DoAction_2.as", common)
        options = (exported / "frame_6/DoAction.as").read_text(encoding="utf-8")
        options = options.replace("function AcceptOrSelect(", "function PortOriginal_AcceptOrSelect(", 1)
        options = options.replace("function OnEnterFrame(", "function PortOriginal_OnEnterFrame(", 1)
        options += ('\nfunction AcceptOrSelect(){ if(_root.selectorPos==4){_root.PortOpenAdditional();}'
                    'else{_root.PortOriginal_AcceptOrSelect();} }\n'
                    'function OnEnterFrame(){ _root.maxSelections=5; '
                    '_root.PortOptionsEntry.Label.Caption=_root.PortRussian?"Дополнительно...":"Additional..."; '
                    '_root.PortOptionsEntry.Label.gotoAndStop(_root.selectorPos==4?2:1); '
                    '_root.PortOriginal_OnEnterFrame(); }\n'
                    '_root.PortFromAdditional=false; _root.maxSelections=5; '
                    '_root.PortOptionsEntry.Label.Caption=_root.PortRussian?"Дополнительно...":"Additional...";\n')
        action("frame_6/DoAction.as", options)
        for release in exported.glob("DefineButton2_*/BUTTONCONDACTION on(*).as"):
            original = release.read_text(encoding="utf-8")
            if release.name == "BUTTONCONDACTION on(press).as" and '_root.guiVal = "SLIDER";' in original:
                rollover = (release.parent / "BUTTONCONDACTION on(rollOver).as").read_text(encoding="utf-8")
                rows = re.findall(r"_root\.selectorPos = (\d+);", rollover)
                if len(rows) != 1:
                    raise ValueError("Original slider press row preimage changed: " + str(release))
                adjusted = responsive_slider_press(original, int(rows[0]))
            elif release.name in ("BUTTONCONDACTION on(release).as", "BUTTONCONDACTION on(rollOut).as"):
                adjusted = responsive_release(original)
            else:
                continue
            if adjusted != original:
                action(release.relative_to(exported), adjusted)
        for frame, section in sections.items():
            body = (exported / f"frame_{frame}/DoAction.as").read_text(encoding="utf-8")
            for name in ("AcceptOrSelect", "CancelOrExit", "UIHighlightLeft", "UIHighlightRight", "OnEnterFrame"):
                needle = "function " + name + "("
                if body.count(needle) != 1:
                    raise ValueError(f"Frame {frame} callback preimage changed: " + name)
                body = body.replace(needle, "function PortOriginal_" + name + "(", 1)
            body += (f'\n_root.PortRussian=_root.PortGameLanguage=="rus";\n_root.PortSectionPage={section["page"]};\n'
                     f"_root.PortOriginalSelections={section['selections']};\n"
                     "_root.PortOriginalNames=" + json.dumps(section["names"]) + ";\n")
            action(f"frame_{frame}/DoAction.as", body + script.read_text(encoding="utf-8"))
        run("-importScript", stage / "widgets.swf", stage / "patched.swf", patches)
        data = (stage / "patched.swf").read_bytes()
        plain = zlib.decompress(data[8:]) if data[:3] == b"CWS" else data[8:]
        if b"PortSettings" not in plain or b"PortOpenAdditional" not in plain or len(data) < source.stat().st_size:
            raise ValueError("Native settings ActionScript import failed")
        output.write_bytes(data)
    receipt = {**identity, "sha256": digest(output), "bytes": output.stat().st_size,
               "editor_assets": editor_assets,
               "widgets": "Original PausePC skin; Options frame 6 Additional catalog, Graphics frame 7 and Input frame 8 pages",
               "callbacks": "Original frame callbacks preserved; port-only FS command uses the existing movie interface",
               "slider_input_bounds_pixels": frame_slider_bounds,
               "port_depths": PORT_DEPTHS,
               "layout_bounds_pixels": {name: [round(x / 20, 3) for x in bounds] for name, bounds in layout.items()}}
    receipt_path.write_text(json.dumps(receipt, indent=2) + "\n", encoding="utf-8")
    return receipt


if __name__ == "__main__":
    print(json.dumps(build(ROOT / "build/native-settings/native-settings-pause.swf"), indent=2))

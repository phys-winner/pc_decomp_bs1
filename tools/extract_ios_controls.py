"""Extract the original control sprites from a local BioShock iOS 1.3.5 IPA."""

import argparse
import hashlib
import json
from pathlib import Path
import struct
from xml.etree import ElementTree
from zipfile import ZipFile
import zlib


APP = "Payload/bioshock.app/"
PACKAGE = APP + "ISEUIHUD.pkg"
SOURCES = {
    PACKAGE: (17320101, "8168961aa0862136303bd211714b8303255208e33baf8f910ecd704b195c90ac"),
    APP + "Config/TA/HUD_PNG.xml": (13786, "896452a8318ca171bb0f2bb89e8e0cc30cb36c9abc862d94ae86f25b62ce243c"),
    APP + "Config/TA/ICONS_PNG.xml": (12486, "65dc179edc099cf4415ee6ce50cbd7a5091be2cedba531f6307b2d72f17270c5"),
}

# Offsets of the top-down BGRA mip data in the pinned package's Texture exports.
ATLASES = {"HUD_PNG": (4198166, 2048, 1024), "ICONS_PNG": (1944, 1024, 1024)}

HUD = {
    "stick": "BIK_HUD_Control01",
    "knob": "BIK_HUD_Control02",
    "crouch": "Bioshock_HUD_Squat",
    "stand": "Bioshock_HUD_Stand",
    "map": "BIK_HUD_MapButton",
    "map_down": "BIK_HUD_MapButtonSelected",
    "pause": "BIK_HUD_PausemenuButton",
    "pause_down": "BIK_HUD_PausemenuButtonSelected",
    "pad": "BIK_HUD_WeaponPad_BG",
    "weapon_mask": "BIK_HUD_WeaponPad_BG_Mask",
    "next": "BIK_HUD_ChangeButton",
    "next_down": "BIK_HUD_ChangeButton_Selected",
    "reload": "BIK_HUD_ReloadButton",
    "reload_down": "BIK_HUD_ReloadButtonSelected",
    "plasmid_reload": "BIK_HUD_MP_ReloadButton",
    "plasmid_reload_down": "BIK_HUD_MP_ReloadButtonSelected",
    "highlight": "BIK_HUD_WeaponButton_HighLight",
    "diary": "BIK_HUD_Aduio",
    "diary_down": "BIK_HUD_AduioPlay",
    "gather": "GatherSaveButton",
    "gather_down": "GatherSaveButton_Selected",
    "use_gear": "Bioshock_WeaponMenu_Gear01_Part02",
}

# The indices are the iOS art IDs reported by bioshock_touch.c.
WEAPONS = (
    "Wrench", "Pistol", "Machine Gun", "ShotGun", "Grenade Launcher", "Chemical Thrower", "Crossbow", "Research Camera",
)
ABILITIES = (
    "Electro Bolt", "Electro Bolt 2", "Electro Bolt 3", "Telekinesis", "Incinerate!", "Incinerate! 2", "Incinerate! 3",
    "Winter Blast", "Winter Blast 2", "Winter Blast 3", "Enrage!", "Security Bullseye", "Target Dummy",
    "Hypnotize Big Daddy", "Hypnotize Big Daddy 2", "Insect Swarm", "Insect Swarm 2", "Insect Swarm 3",
    "Cyclone Trap", "Cyclone Trap 2", "Sonic Boom", "Sonic Boom 2",
)
ICONS = {"hack": "BIK_HUD_BTN_Hacker", "use": "Plasmids_ActionButton", "use_down": "BIK_HUD_BTN_ActionButton",
         **{f"weapon_{index}": name for index, name in enumerate(WEAPONS, 1)},
         **{f"ability_{index}": name for index, name in enumerate(ABILITIES, 1)}}


def png_chunk(kind, contents):
    return struct.pack(">I", len(contents)) + kind + contents + struct.pack(">I", zlib.crc32(kind + contents))


def crop_png(package, offset, atlas_width, rect):
    x, y, width, height = rect
    pixels = bytearray()
    for row in range(y, y + height):
        start = offset + (atlas_width * row + x) * 4
        bgra = package[start:start + width * 4]
        rgba = bytearray(bgra)
        rgba[0::4] = bgra[2::4]
        rgba[2::4] = bgra[0::4]
        pixels.append(0)
        pixels.extend(rgba)
    return (b"\x89PNG\r\n\x1a\n" + png_chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 6, 0, 0, 0))
            + png_chunk(b"IDAT", zlib.compress(pixels, 9)) + png_chunk(b"IEND", b""))


def extract(ipa: Path, destination: Path):
    contents, member_hashes = {}, {}
    with ZipFile(ipa) as source:
        for path, (size, expected) in SOURCES.items():
            member = source.getinfo(path)
            if member.file_size != size:
                raise ValueError(f"Unexpected size of iOS source member {path}")
            data = source.read(member)
            digest = hashlib.sha256(data).hexdigest()
            if digest != expected:
                raise ValueError(f"Unexpected SHA-256 of iOS source member {path}")
            contents[path] = data
            member_hashes[path] = digest

    package = contents[PACKAGE]
    assets = {}
    for atlas, sprites in (("HUD_PNG", HUD), ("ICONS_PNG", ICONS)):
        offset, width, height = ATLASES[atlas]
        end = offset + width * height * 4
        footer = struct.pack("<IIBB", width, height, width.bit_length() - 1, height.bit_length() - 1)
        if package[end:end + len(footer)] != footer:
            raise ValueError(f"Invalid mip footer for {atlas}")
        xml = ElementTree.fromstring(contents[APP + "Config/TA/" + atlas + ".xml"])
        if xml.get("Imagefile") != atlas + ".png":
            raise ValueError(f"Unexpected texture atlas {xml.get('Imagefile')}")
        rectangles = {node.attrib["Name"]: tuple(int(node.attrib[key]) for key in ("XPos", "YPos", "Width", "Height"))
                      for node in xml.findall("Image")}
        for filename, sprite in sprites.items():
            rect = rectangles[sprite]
            x, y, w, h = rect
            if not (0 <= x < x + w <= width and 0 <= y < y + h <= height):
                raise ValueError(f"Sprite {sprite} lies outside {atlas}")
            assets[filename + ".png"] = crop_png(package, offset, width, rect)

    destination.mkdir(parents=True, exist_ok=True)
    for filename, data in assets.items():
        (destination / filename).write_bytes(data)
    return {"source_members": member_hashes, "assets": {name: hashlib.sha256(data).hexdigest() for name, data in assets.items()}}


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("ipa", type=Path)
    parser.add_argument("destination", type=Path)
    args = parser.parse_args()
    print(json.dumps(extract(args.ipa, args.destination), indent=2))

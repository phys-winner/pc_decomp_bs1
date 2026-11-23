"""Adapt pinned iOS filtering, color transfer and axial fog to BioShock PC's shader ABI."""
import hashlib
import json
from pathlib import Path
import re
import struct

ROOT = Path(__file__).resolve().parents[1]
SPK_SHA256 = "ef5eb2b2ee5291a1d9158cda0707c82e6eba9896b5ebd636df8ba93b5634885d"
MOBILE_SPK_SHA256 = "02d8e718ba52326ea537b25959a3b1de95833be1408540a7f8a84aae4f88ae59"
ENGINE_SHA256 = "7f2ee821ad60eaf08322b4bc6ebbf839d7fe010b4d0255ec12b5be4a2966f7f6"
D3DX_SHA256 = "c6b7b2ad7742dde5dd8d1a35fdc1c185e586e551ad9c74d3fb21759cd8ca4da8"
PCS_SHA256 = "953c630fb6d5d0b72ae3a0f162f28b0ddcf17155a82089d8e34ee946868cd27c"
MOBILE_PCS_SHA256 = "c273960c7dae4d7dff902a953f6f4c73c6e700fea35e2efb999569b322427d0a"
REFERENCE_SHA256 = {
    "Filter1PixelShader.msf": "a676f20640a3d68c36d19767004c12b8320e101d173097954ca702aa479dcca6",
    "Filter4PixelShader.msf": "964a5f91e38bf3dc23acd523ab5f03982a5533b985bca1ff4013d7ee81365a1f",
    "Filter16PixelShader.msf": "f1c1cc75a9d0680c126cd3fe443cf26a8a488fe0d9003b9140ece8b5d3254908",
    "DOFAndBloomGatherPixelShader.msf": "2e1b78eeb18959a28e5ead03267517d5ce8fa902fb019eead77a77e089997065",
    "Prefix_Common.msf": "46ff949ac047a3440428e1facb7346f6584e8b4d37adb9eb8c1e4ac7befd6fd4",
    "Prefix_PixelShader.msf": "46d3679c48bd55d2a87a6906dd5f44b092a5592b2b73a9a707ed7be122b2e3a1",
    "FogApplyPixelShader.msf": "57ed214b57ad0eddc51636b4b7cf49636139a8978b847104ecfc29645ef6eea8",
}


def checked(path, expected):
    data = path.read_bytes()
    if hashlib.sha256(data).hexdigest() != expected:
        raise ValueError(f"Mobile shader input SHA256 mismatch: {path}")
    return data


def unpack(data):
    if len(data) < 8 or struct.unpack_from("<2I", data) != (1, 40):
        raise ValueError("Unsupported BioShock shader archive")
    rows, position = [], 8
    for _ in range(40):
        if position + 12 > len(data):
            raise ValueError("Truncated shader archive record")
        flags, timestamp, chars = struct.unpack_from("<3I", data, position)
        position += 12
        if not 1 <= chars <= 256 or position + chars * 2 + 4 > len(data):
            raise ValueError("Invalid shader archive name")
        name = data[position:position + chars * 2].decode("utf-16le")
        position += chars * 2
        size = struct.unpack_from("<I", data, position)[0]
        position += 4
        if position + size > len(data):
            raise ValueError("Truncated shader archive source")
        rows.append((name, flags, timestamp, data[position:position + size]))
        position += size
    if position != len(data) or len({row[0] for row in rows}) != 40:
        raise ValueError("Invalid shader archive length or duplicate name")
    return rows


def pack(rows):
    output = bytearray(struct.pack("<2I", 1, len(rows)))
    for name, flags, timestamp, source in rows:
        encoded = name.encode("utf-16le")
        output += struct.pack("<3I", flags, timestamp, len(encoded) // 2) + encoded
        output += struct.pack("<I", len(source)) + source
    return bytes(output)


def replace_once(source, before, after):
    if source.count(before) != 1:
        raise ValueError(f"Mobile shader preimage mismatch: {before[:80]!r}")
    return source.replace(before, after, 1)


def adapt(data, references):
    sources = {name: value.decode("utf-8") for name, value in references.items()}
    prefix = sources["Prefix_PixelShader.msf"]
    if "#define HANDLE_SRGB_WRITE(Color) sqrt( Color )" not in prefix:
        raise ValueError("Missing pinned mobile sRGB approximation")
    gather = sources["DOFAndBloomGatherPixelShader.msf"]
    if "max( InSceneColor.r, max( InSceneColor.g, InSceneColor.b ) )" not in gather:
        raise ValueError("Missing pinned mobile bloom luminance")
    if "highp float distance = screenData.w;" not in sources["FogApplyPixelShader.msf"]:
        raise ValueError("Missing pinned mobile axial fog distance")
    filter4 = sources["Filter4PixelShader.msf"]
    terms = re.findall(r"texture2D\(TextureBase,OffsetUVs\[(\d)\]\.([xy]+)\)\) \* SampleWeights4\[(\d)\]", filter4)
    if terms != [("0", "xy", "0"), ("1", "yx", "1"), ("2", "xy", "2"), ("3", "yx", "3")]:
        raise ValueError("Unexpected mobile four-tap filter")
    # iOS's odd vec2 varyings are yx-packed; PC packs both taps in vec4.xyyx.
    mobile4 = "\n".join(
        f"\tSum += BlurTransfer(tex2D(s_buffer,Interpolants.OffsetUVs[{int(i) // 2}].{'xy' if int(i) % 2 == 0 else 'wz'})) * SampleWeights[{weight}];"
        for i, _, weight in terms)
    changed, output = [], []
    for name, flags, timestamp, raw in unpack(data):
        source = raw.decode("utf-8")
        if name == "ImageSpace.hlsl":
            source = replace_once(source, "pixel.rgb = pow(pixel.rgb, 1.f/2.2f);",
                "// iOS Prefix_PixelShader.msf: USE_SRGB_APPROXIMATION.\n\t\tpixel.rgb = sqrt(pixel.rgb);")
            start = source.index("\tfor(SampleIndex = 0;SampleIndex < NUM_SAMPLES - 1;SampleIndex += 2)", source.index("half4 BlurPixelShader("))
            end = source.index("\n\t// Apply PWL", start)
            original = source[start:end]
            filters = "#if NUM_SAMPLES == 1\n\t// iOS Filter1, retaining the PC transfer and weight contract.\n\tSum = BlurTransfer(tex2D(s_buffer,Interpolants.OffsetUVs[0].xy)) * SampleWeights[0];\n"
            filters += "#elif NUM_SAMPLES == 4\n\t// iOS Filter4 with packed PC interpolants and original weights.\n" + mobile4 + "\n#else\n"
            filters += "\t// iOS Filter16 uses this paired-tap loop; retain arbitrary PC tap counts.\n" + original + "\n#endif\n"
            source = source[:start] + filters + source[end:]
            source = replace_once(source, "#if (TRANSFER_TYPE == BT_VarianceShadowGather)",
                "#if (TRANSFER_TYPE == BT_LuminancePWL)\n\t{\n"
                "\t\t// iOS bloom gather thresholds each source tap before averaging; retain the PC PWL curve and alpha.\n"
                "\t\thalf luminance = max(0.00001f, max(sample.r, max(sample.g, sample.b)));\n"
                "\t\thalf bloom_luminance = ComputePWL(luminance);\n"
                "\t\tsample.rgb *= (bloom_luminance/luminance);\n\t}\n"
                "#elif (TRANSFER_TYPE == BT_VarianceShadowGather)")
            source = replace_once(source,
                "\t// Apply PWL to final output, rather than per sample\n"
                "#if (TRANSFER_TYPE == BT_LuminancePWL)\n\t{\t\n"
                "\t\t// NOTE: max clamp here is to prevent NaN in this calcs when luminance is close to 0\n"
                "\t\thalf luminance = max(0.00001f, dot(Sum.rgb, luminanceWeights));\n"
                "\t\thalf bloom_luminance = ComputePWL(luminance);\n"
                "\t\tSum.rgb *= (bloom_luminance/luminance);\n\t}\n#endif\n", "")
        elif name == "ScreenTransfer.hlsl":
            source = replace_once(source, "static const bool UseRadialFog = 1;", "static const bool UseRadialFog = 0;")
        if source.encode("utf-8") != raw:
            changed.append(name)
            # SPK entries store source timestamps, not CRCs. Use a fixed new revision.
            output.append((name, flags, 0x7FFFFFFE, source.encode("utf-8")))
        else:
            output.append((name, flags, timestamp, raw))
    if changed != ["ImageSpace.hlsl", "ScreenTransfer.hlsl"]:
        raise ValueError(f"Unexpected mobile shader changes: {changed}")
    return pack(output)


def unpack_cache(data):
    """Read the engine's version-13, uncompressed D3D9 PCS without reserializing records."""
    if len(data) < 4 or struct.unpack_from("<I", data)[0] != 13:
        raise ValueError("Unsupported BioShock PCS version")
    rows, keys, position = [], set(), 4
    while position < len(data):
        start = position
        first = data[position]
        position += 1
        count, more, shift = first & 63, first & 64, 6
        if first & 128:
            raise ValueError("Unexpected PCS string encoding")
        while more:
            if position >= len(data) or shift > 27:
                raise ValueError("Invalid PCS compact string length")
            part = data[position]
            position += 1
            count |= (part & 127) << shift
            more, shift = part & 128, shift + 7
        if not 1 <= count <= 2048 or position + count * 2 + 24 > len(data):
            raise ValueError("Invalid PCS shader key")
        name = data[position:position + count * 2].decode("utf-16le")
        position += count * 2
        if not name.endswith("\0") or "\0" in name[:-1]:
            raise ValueError("Invalid PCS shader key terminator")
        name = name[:-1]
        key = data[position:position + 16]
        size, timestamp = struct.unpack_from("<II", data, position + 16)
        position += 24
        if not size or size % 4 or size > len(data) - position:
            raise ValueError("Invalid PCS bytecode size")
        identity = name.casefold(), key
        if identity in keys:
            raise ValueError("Duplicate PCS shader key")
        keys.add(identity)
        code = data[position:position + size]
        position += size
        rows.append((name, key, timestamp, code, data[start:position]))
    return rows


def ctab_names(code):
    """Validate a real D3DX33 SM3 constant table; return global and struct-member names."""
    if len(code) < 8 or len(code) % 4:
        raise ValueError("Invalid shader bytecode length")
    words = struct.unpack(f"<{len(code) // 4}I", code)
    version = words[0]
    if version not in (0xFFFE0300, 0xFFFF0300):
        raise ValueError("Unknown shader profile")
    tables, position, ended = [], 1, False
    while position < len(words):
        token = words[position]
        opcode = token & 65535
        if opcode == 0xFFFF:
            if token != 0xFFFF or position != len(words) - 1:
                raise ValueError("Invalid shader END token")
            ended = True
            break
        if opcode == 0xFFFE:
            count = (token >> 16) & 32767
            if not count or position + 1 + count > len(words):
                raise ValueError("Invalid shader comment")
            if words[position + 1] == 0x42415443:  # CTAB, not an arbitrary string match.
                tables.append(code[(position + 2) * 4:(position + 1 + count) * 4])
            position += 1 + count
        else:
            count = (token >> 24) & 15
            if (not count and opcode not in (0, 28, 29, 39, 42, 43, 44)) or position + 1 + count > len(words):
                raise ValueError("Invalid shader instruction length")
            position += 1 + count
    if not ended or len(tables) != 1:
        raise ValueError("Missing or ambiguous shader CTAB")
    data = tables[0]

    def bound(offset, size):
        if not 0 <= offset <= len(data) or not 0 <= size <= len(data) - offset:
            raise ValueError("CTAB offset outside comment")

    def string(offset):
        bound(offset, 1)
        end = data.find(b"\0", offset)
        if end < 0:
            raise ValueError("Unterminated CTAB string")
        return data[offset:end].decode("ascii")

    bound(0, 28)
    size, creator, table_version, count, info, _, target = struct.unpack_from("<7I", data)
    expected_target = "vs_3_0" if version == 0xFFFE0300 else "ps_3_0"
    if (size != 28 or table_version != version or count > 512 or string(target) != expected_target
            or string(creator) != "Microsoft (R) D3DX9 Shader Compiler "):
        raise ValueError("Unknown CTAB compiler or header")
    bound(info, 20 * count)
    names, globals_seen = set(), set()

    def identifier(offset):
        name = string(offset)
        if not re.fullmatch(r"[A-Za-z_][A-Za-z_0-9]*", name):
            raise ValueError("Unknown CTAB constant alias")
        names.add(name)
        return name

    def type_info(offset, depth=0):
        if depth > 16:
            raise ValueError("Recursive CTAB type")
        bound(offset, 16)
        kind, scalar, rows, columns, elements, members, member_info = struct.unpack_from("<6HI", data, offset)
        # Struct columns describe the aggregate and legitimately exceed four.
        if kind > 5 or scalar > 19 or not elements or (kind != 5 and (rows > 4 or columns > 4)):
            raise ValueError("Unknown CTAB type")
        if members:
            if kind != 5 or members > 512:
                raise ValueError("Invalid CTAB struct")
            bound(member_info, 8 * members)
            for index in range(members):
                name, child = struct.unpack_from("<II", data, member_info + 8 * index)
                identifier(name)
                type_info(child, depth + 1)

    for index in range(count):
        name, register_set, register, registers, _, type_offset, default = struct.unpack_from("<I4H2I", data, info + 20 * index)
        name = identifier(name)
        if name in globals_seen or register_set > 3 or not registers or register + registers > 4096:
            raise ValueError("Invalid or duplicate CTAB constant")
        globals_seen.add(name)
        type_info(type_offset)
        if default:
            bound(default, registers * (4 if register_set == 0 else 16))
    return names


def filter_cache(data, sources):
    """Keep only shaders whose output cannot depend on the changed fog-distance branch."""
    kept, counts = [], {"ImageSpace": 0, "fogTransform": 0, "invalid_or_unknown": 0}
    source_names = sorted(sources, key=len, reverse=True)
    for name, _, _, code, raw in unpack_cache(data):
        rest, modules = name, []
        while True:
            module = next((value for value in source_names if rest.startswith(value)), None)
            if module is None:
                break
            modules.append(module)
            rest = rest[len(module):]
        if "ImageSpace.hlsl" in modules:
            counts["ImageSpace"] += 1
            continue
        try:
            if (not modules or not re.fullmatch(r"[A-Za-z_][A-Za-z_0-9]*", rest)
                    or not re.search(r"\b" + re.escape(rest) + r"\s*\(", sources[modules[0]])):
                raise ValueError("Unknown PCS shader source or entry")
            names = ctab_names(code)
        except (ValueError, UnicodeError, struct.error):
            counts["invalid_or_unknown"] += 1
            continue
        if "fogTransform" in names:
            counts["fogTransform"] += 1
        else:
            kept.append(raw)
    return struct.pack("<I", 13) + b"".join(kept), {"kept": len(kept), "excluded": counts}


def build_cache(output, mobile_spk):
    """The certificate is specific to this engine, archive delta and original PCS."""
    original = Path(json.loads((ROOT / "config/inputs.json").read_text())["source_root"]) / "Builds/Release"
    checked(original / "Bioshock.exe", ENGINE_SHA256)
    checked(original / "d3dx9_33.dll", D3DX_SHA256)
    data = checked(original / "ShaderCache.pcs", PCS_SHA256)
    before = checked(original / "shaders.spk", SPK_SHA256)
    if hashlib.sha256(mobile_spk).hexdigest() != MOBILE_SPK_SHA256:
        raise ValueError("Mobile PCS certificate does not cover this SPK")
    source_rows, mobile_rows = unpack(before), unpack(mobile_spk)
    for source, mobile in zip(source_rows, mobile_rows):
        name = source[0]
        if name == "ScreenTransfer.hlsl":
            expected = source[3].replace(b"static const bool UseRadialFog = 1;", b"static const bool UseRadialFog = 0;")
            if mobile[:2] != source[:2] or mobile[2] != 0x7FFFFFFE or mobile[3] != expected:
                raise ValueError("Mobile PCS certificate requires only the axial-fog switch")
        elif name != "ImageSpace.hlsl" and source != mobile:
            raise ValueError("Mobile PCS certificate does not cover other shader changes")
    sources = {name: raw.decode("utf-8") for name, _, _, raw in source_rows}
    result, counts = filter_cache(data, sources)
    digest = hashlib.sha256(result).hexdigest()
    if (digest != MOBILE_PCS_SHA256 or counts != {"kept": 15535, "excluded": {
            "ImageSpace": 220, "fogTransform": 630, "invalid_or_unknown": 0}}):
        raise ValueError("Mobile PCS certificate result mismatch")
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_bytes(result)
    receipt = {"original_sha256": PCS_SHA256, "engine_sha256": ENGINE_SHA256, "compiler_sha256": D3DX_SHA256,
               "original_spk_sha256": SPK_SHA256, "mobile_spk_sha256": MOBILE_SPK_SHA256,
               "sha256": digest, "bytes": len(result), "format": "BioShock D3D9 PCS version 13",
               **counts, "cache_directory": "ShaderCache-mobile-v3",
               "certificate": "ImageSpace excluded; validated original D3DX33 CTAB without fogTransform; engine never specializes that uniform",
               "preserved": "Original record bytes, key, timestamp, constant bindings and bytecode",
               "hardware_read_verified": False}
    output.with_suffix(".json").write_text(json.dumps(receipt, indent=2) + "\n")
    return receipt


def build(output):
    original = Path(json.loads((ROOT / "config/inputs.json").read_text())["source_root"])
    source = checked(original / "Builds/Release/shaders.spk", SPK_SHA256)
    references = {name: checked(ROOT / "reference/mobile-shaders" / name, digest)
                  for name, digest in REFERENCE_SHA256.items()}
    result = adapt(source, references)
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_bytes(result)
    receipt = {"original_sha256": SPK_SHA256, "reference_sha256": REFERENCE_SHA256,
               "sha256": hashlib.sha256(result).hexdigest(), "bytes": len(result),
               "changed_entries": ["ImageSpace.hlsl", "ScreenTransfer.hlsl"],
               "stages": ["1/4/16-tap filtering with PC transfer/weights", "per-source-tap mobile peak-channel bloom gather with PC PWL and alpha", "mobile sRGB square-root output", "mobile axial fog with PC depth decoding"],
               "bloom_scope": "BT_LuminancePWL only; PC PWL constants/threshold and RGBA sample weights retained; sample alpha unchanged",
               "cache_directory": "ShaderCache-mobile-v3",
               "fog_scope": "Shared computeFog uses view depth, including vertex fog, FogApply/FogBlend and Fluid/PondScum; viewport-edge fog differs from radial distance",
               "unchanged": "Original PC material records, shadows, depth/alpha, DOF, sampler/uniform/vertex ABI and simulation"}
    output.with_suffix(".json").write_text(json.dumps(receipt, indent=2) + "\n")
    return receipt


if __name__ == "__main__":
    output = ROOT / "build/mobile-shaders/mobile-shaders.spk"
    print(json.dumps({"mobile_shaders": build(output),
                      "mobile_shader_cache": build_cache(output.with_name("mobile-shader-cache.pcs"), output.read_bytes())}, indent=2))

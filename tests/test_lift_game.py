import struct
import sys
import tempfile
import unittest
from pathlib import Path
from types import SimpleNamespace
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
import lift_game
from pe.pe_analyze import Section


class LiftGameTest(unittest.TestCase):
    def test_immediate_data_is_not_a_callback_entry(self):
        text = b"token\0\0\0version token\0\0\0token '%s'\0\0\0integer constant\0\0\0\0float constant\0\0\0"
        functions, rejected, _ = self.collect(text)
        self.assertEqual(set(functions), {0x40001000, 0x40001100})
        self.assertNotIn(0x40001020, functions)
        self.assertEqual(rejected, [])

    def test_ida_data_excludes_a_decodable_pointer_table(self):
        text = bytes.fromhex(
            "41 74 74 72 50 61 72 61 6d 73 00 00 f0 c5 01 20 "
            "f8 c5 01 20 04 c6 01 20 a4 cb 01 20 84 13 04 20 "
            "1c c6 01 20 48 cb 01 20 90 13 04 20 98 13 04 20 "
            "a0 13 04 20 74 65 63 68 6e 69 71 75 65 31 30 00")
        functions, rejected, decoder = self.collect(text, {0x40001020})
        self.assertTrue(decoder.probes_as_function_body(0x40001020))
        self.assertEqual(set(functions), {0x40001000, 0x40001100})
        self.assertNotIn(0x40001020, functions)
        self.assertEqual(rejected, [])

    def test_large_d3dx_functions_fit_the_decode_limit(self):
        binary = ROOT / "generated/input/d3dx9_33.dll"
        if not binary.exists():
            self.skipTest("Prepared d3dx9_33.dll is unavailable")
        info = lift_game.analyze_pe(str(binary))
        decoder = lift_game.BoundedDisassembler(binary.read_bytes(), info.image_base, info.sections)
        for address, windows, blocks, instructions in (
                (0x2028DEEE, 5950, 2975, 20439), (0x200C54F9, 5140, 2570, 39579)):
            with self.subTest(address=hex(address)):
                function = decoder.disassemble_function(address, {})
                self.assertEqual((decoder._windows, len(function.blocks), function.num_instructions),
                                 (windows, blocks, instructions))
                self.assertLessEqual(decoder._windows, decoder.max_windows)

    def test_d3dx_initializer_callbacks_are_dispatched(self):
        import json
        import pefile
        report_path = ROOT / "generated/d3dx9_33/report.json"
        if not report_path.exists():
            self.skipTest("D3DX generation report is unavailable")
        report = json.loads(report_path.read_text())
        pe = pefile.PE(str(ROOT / "generated/input/d3dx9_33.dll"))
        self.assertEqual(struct.unpack("<II", pe.get_data(0x1494, 8)), (0x2026631A, 0x202676B8))
        self.assertEqual(pe.get_data(0x26631A, 5), b"\x68\x80\x00\x00\x00")
        self.assertIn("0x2026631a", report["supplemental_entries"])
        self.assertIn("0x2026631a", report["dispatch_entries"])
        self.assertIn("0x202676b8", report["dispatch_entries"])
        self.addCleanup(pe.close)
        initializers = set(struct.unpack("<90I", pe.get_data(0x1324, 0x168)))
        initializers.discard(0)
        self.assertEqual(len(initializers), 89)
        self.assertLessEqual(initializers, {int(address, 0) for address in report["dispatch_entries"]})
        self.assertLessEqual(initializers, {int(address, 0) for address in report["supplemental_entries"]})

    def test_d3dx_deflate_callbacks_are_dispatched(self):
        import json
        import pefile
        report_path = ROOT / "generated/d3dx9_33/report.json"
        if not report_path.exists():
            self.skipTest("D3DX generation report is unavailable")
        report = json.loads(report_path.read_text())
        pe = pefile.PE(data=(ROOT / "generated/input/d3dx9_33.dll").read_bytes())
        callbacks = [pe.get_dword_at_rva(0x29AA0 + offset) for offset in range(0, 0x78, 0xC)]
        self.assertEqual(len(callbacks), 10)
        self.assertEqual(set(callbacks), {0x202551CB, 0x202552F5, 0x202555A5})
        self.assertEqual(pe.get_data(0x254D32, 6), b"\xff\x90\xa0\x9a\x02\x20")
        self.assertLessEqual(set(callbacks), {int(address, 0) for address in report["dispatch_entries"]})

    def test_d3dx_parser_switch_table_is_complete(self):
        import hashlib
        import json
        import pefile
        modules = json.loads((ROOT / "config/modules.json").read_text())["modules"]
        spec = next(row for row in modules if row["name"] == "d3dx9_33")
        raw = (ROOT / "generated/input/d3dx9_33.dll").read_bytes()
        info = lift_game.analyze_pe(str(ROOT / "generated/input/d3dx9_33.dll"))
        pe = pefile.PE(data=raw)
        expected = {0x20278661: 0x202785A5, 0x20276A2C: 0x202760B0,
                    0x20172106: 0x201720B7, 0x2017340B: 0x20173246}
        self.assertEqual({int(row["address"], 0) for row in spec["jump_tables"]}, set(expected))
        for table in spec["jump_tables"]:
            with self.subTest(table=table["address"]):
                address = int(table["address"], 0)
                self.assertEqual(table["first_index"], 0)
                self.assertEqual(hashlib.sha256(pe.get_data(address - info.image_base, table["count"] * 4)).hexdigest(), table["sha256"])
                decoder = lift_game.BoundedDisassembler(raw, info.image_base, info.sections,
                    jump_tables={address: (table["first_index"], table["count"])})
                function = decoder.disassemble_function(int(table["owner"], 0), {})
                self.assertIn(expected[address], function.jump_targets)
                self.assertIn(expected[address], function.blocks)
                self.assertGreater(len(function.blocks), 256)

    @staticmethod
    def collect(text, excluded=()):
        base = 0x40000000
        string = base + 0x1020
        callback = base + 0x1100
        raw = bytearray(0x1000)
        raw[:11] = b"\x68" + struct.pack("<I", string) + b"\x68" + struct.pack("<I", callback) + b"\xC3"
        raw[0x20:0x20 + len(text)] = text
        raw[0x100] = 0xC3
        info = SimpleNamespace(image_base=base, entry_point_rva=0x1000, sections=[
            Section(".text", 0x1000, len(raw), 0, len(raw), 0x60000020),
        ])
        with tempfile.TemporaryDirectory() as directory:
            binary = Path(directory) / "image.dll"
            binary.write_bytes(raw)
            with patch.object(lift_game, "analyze_pe", return_value=info), patch.object(lift_game, "build_iat_map", return_value={}):
                _, _, _, functions, rejected = lift_game.collect_functions(
                    binary, [], compact=False, excluded_entries=excluded)
        decoder = lift_game.BoundedDisassembler(raw, base, info.sections)
        return functions, rejected, decoder


if __name__ == "__main__":
    unittest.main()

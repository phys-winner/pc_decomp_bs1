"""Validate BioShock's checked controller/mouse seam and real host ownership."""
import hashlib
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

import pefile
from capstone import Cs, CS_ARCH_X86, CS_MODE_32

from tests.host_environment import CLANG, environment

ROOT = Path(__file__).resolve().parents[1]


class BioshockHybridInputTest(unittest.TestCase):
    def test_checked_original_branch_and_canonical_emission(self):
        # The vendor lifter loads its own package named tools. Keep that import
        # and its sys.path changes out of the shared unittest interpreter.
        subprocess.run([sys.executable, "-B", "-c",
                        "from tests.test_bioshock_hybrid_input import BioshockHybridInputTest; "
                        "BioshockHybridInputTest('test_checked_original_branch_and_canonical_emission')"
                        "._check_original_branch_and_canonical_emission()"],
                       cwd=ROOT, check=True, timeout=30)

    def _check_original_branch_and_canonical_emission(self):
        sys.path.insert(0, str(ROOT / "tools"))
        from lift_game import CheckedLifter
        from disasm.disasm32 import Instruction

        def instruction(decoded):
            return Instruction(decoded.address, decoded.size, decoded.mnemonic, decoded.op_str,
                               decoded.bytes, decoded.operands)

        config = json.loads((ROOT / "config/inputs.json").read_text(encoding="utf-8"))
        raw = (Path(config["source_root"]) / "Builds/Release/Bioshock.exe").read_bytes()
        self.assertEqual(hashlib.sha256(raw).hexdigest(),
                         "7f2ee821ad60eaf08322b4bc6ebbf839d7fe010b4d0255ec12b5be4a2966f7f6")
        pe = pefile.PE(data=raw)
        decoder = Cs(CS_ARCH_X86, CS_MODE_32)
        decoder.detail = True
        address = 0x10DA3EE2
        code = pe.get_data(address - pe.OPTIONAL_HEADER.ImageBase, 4)
        self.assertEqual(code.hex(), "3bdf754e")
        instructions = [instruction(decoded) for decoded in decoder.disasm(code, address)]
        lifter = CheckedLifter()
        lifter.lift_instruction(instructions[0])
        state, sequence = lifter._flag_state, lifter._flag_seq
        emission = lifter.lift_instruction(instructions[1])
        self.assertEqual((lifter._flag_state, lifter._flag_seq), (state, sequence))
        self.assertEqual(len(emission), 1)
        self.assertIn("CMP_NE(_flag_a, _flag_b) && !void_input_touch_pc_mouse()", emission[0])
        self.assertNotIn("RECOMP_FLAGS", emission[0])
        source = (ROOT / "generated/bioshock/functions_0203.c").read_text(encoding="utf-8")
        self.assertEqual(source.count("    " + emission[0] + "\n"), 1)
        self.assertEqual(source.count("void_input_touch_pc_mouse"), 2)  # One local declaration and call.
        for changed in ("744e", "754f", "0f854e000000"):
            with self.subTest(changed=changed), self.assertRaisesRegex(ValueError, "gate bytes changed"):
                lifter.lift_instruction(instruction(next(decoder.disasm(bytes.fromhex(changed), 0x10DA3EE4))))
        other = lifter.lift_instruction(instruction(next(decoder.disasm(bytes.fromhex("754e"), 0x10DA3EE5))))
        self.assertNotIn("void_input_touch_pc_mouse", "\n".join(other))

    def test_production_host_gate_movement_and_cancellation(self):
        env, linker = environment("x64")
        with tempfile.TemporaryDirectory(prefix="bioshock-hybrid-input-") as directory:
            out = Path(directory)
            for command in (
                [str(CLANG), "/nologo", "/O2", "/std:c11", "/D_CRT_SECURE_NO_WARNINGS",
                 "/clang:--target=x86_64-pc-windows-msvc", "/FI" + str(ROOT / "tests/touch_host_win.h"),
                 "/c", str(ROOT / "tests/test_touch_native.c"),
                 "/Fo" + str(out / "test.obj")],
                [str(linker), "/nologo", str(out / "test.obj"), "/OUT:" + str(out / "test.exe")],
                [str(out / "test.exe")],
            ):
                subprocess.run(command, cwd=ROOT, env=env, check=True, timeout=60)


if __name__ == "__main__":
    unittest.main()

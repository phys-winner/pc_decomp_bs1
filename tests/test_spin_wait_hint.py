"""Checked lifter seam for the render thread's worker spin (0x10D74FF9).

The original D3D render device hands dynamic-resource jobs to three worker
threads and spins while all three are busy. The canonical generator turns only
that back edge into `RECOMP_SPIN_WAIT(); goto L_10D74FF0;`, keeping the branch
condition and target, and refuses any change of the original bytes. The
generated file must carry exactly the generator's emission.
"""
import hashlib
import json
from pathlib import Path
import subprocess
import sys
import unittest

import pefile
from capstone import Cs, CS_ARCH_X86, CS_MODE_32

ROOT = Path(__file__).resolve().parents[1]


class SpinWaitHintTest(unittest.TestCase):
    def test_checked_branch_and_canonical_emission(self):
        # The vendor lifter loads its own package named tools: isolate it.
        subprocess.run([sys.executable, "-B", "-c",
                        "from tests.test_spin_wait_hint import SpinWaitHintTest; "
                        "SpinWaitHintTest('test_checked_branch_and_canonical_emission')._check()"],
                       cwd=ROOT, check=True, timeout=60)

    def _check(self):
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
        address = 0x10D74FF6
        code = pe.get_data(address - pe.OPTIONAL_HEADER.ImageBase, 5)
        self.assertEqual(code.hex(), "83f8037df5")   # cmp eax, 3; jge 0x10d74ff0
        instructions = [instruction(decoded) for decoded in decoder.disasm(code, address)]
        lifter = CheckedLifter()
        lifter.lift_instruction(instructions[0])
        state, sequence = lifter._flag_state, lifter._flag_seq
        emission = lifter.lift_instruction(instructions[1])
        self.assertEqual((lifter._flag_state, lifter._flag_seq), (state, sequence))
        self.assertEqual(emission, ["if (CMP_GE(_flag_a, _flag_b)) { RECOMP_SPIN_WAIT(); goto L_10D74FF0; } "
                                    "/* 0x10D74FF9: jge 0x10d74ff0; all workers busy: offer the machine */"])
        source = (ROOT / "generated/bioshock/functions_0196.c").read_text(encoding="utf-8")
        self.assertEqual(source.count("    " + emission[0] + "\n"), 1)
        self.assertEqual(source.count("RECOMP_SPIN_WAIT"), 1)
        self.assertNotIn("goto L_10D74FF0; /* 0x10D74FF9: jge 0x10d74ff0 */", source)
        # Any other encoding at this address is refused rather than silently patched.
        for changed in ("7df4", "7cf5", "0f8df1ffffff"):
            with self.subTest(changed=changed), self.assertRaisesRegex(ValueError, "worker spin"):
                lifter.lift_instruction(instruction(next(decoder.disasm(bytes.fromhex(changed), 0x10D74FF9))))
        # The same branch elsewhere is lifted unchanged.
        other = lifter.lift_instruction(instruction(next(decoder.disasm(bytes.fromhex("7df5"), 0x10D74FFA))))
        self.assertNotIn("RECOMP_SPIN_WAIT", "\n".join(other))
        runtime = (ROOT / "android/runtime/recomp_types.h").read_text(encoding="utf-8")
        self.assertIn("#define RECOMP_SPIN_WAIT() do { \\\n    if (recomp_spin_hook) { RECOMP_REGS_OUT(); "
                      "recomp_spin_hook(); RECOMP_REGS_IN(); } \\\n} while (0)", runtime)


if __name__ == "__main__":
    unittest.main()

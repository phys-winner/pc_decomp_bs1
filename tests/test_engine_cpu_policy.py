"""Checked lifter seam for appInit's engine CPU count store (0x109522B1).

Only the stored value passes through the host policy; the instruction bytes are
pinned, the register keeps its GetSystemInfo value, and the policy defaults to
the engine's original single-core configuration with a QA override.
"""
import hashlib
import json
from pathlib import Path
import re
import subprocess
import sys
import unittest

ROOT = Path(__file__).resolve().parents[1]
EMISSION = ("{ extern uint32_t void_bioshock_engine_cpu_count(uint32_t); "
            "MEM32(0x11129754) = void_bioshock_engine_cpu_count(ecx); } "
            "/* 0x109522B1: mov dword ptr [0x11129754], ecx; engine CPU count policy */")


class EngineCpuPolicyTest(unittest.TestCase):
    def test_checked_store_and_host_policy(self):
        subprocess.run([sys.executable, "-B", "-c",
                        "from tests.test_engine_cpu_policy import EngineCpuPolicyTest; "
                        "EngineCpuPolicyTest('test_checked_store_and_host_policy')._check()"],
                       cwd=ROOT, check=True, timeout=60)

    def _check(self):
        sys.path.insert(0, str(ROOT / "tools"))
        from lift_game import CheckedLifter
        from disasm.disasm32 import Instruction
        import pefile
        from capstone import Cs, CS_ARCH_X86, CS_MODE_32
        config = json.loads((ROOT / "config/inputs.json").read_text(encoding="utf-8"))
        raw = (Path(config["source_root"]) / "Builds/Release/Bioshock.exe").read_bytes()
        self.assertEqual(hashlib.sha256(raw).hexdigest(), "7f2ee821ad60eaf08322b4bc6ebbf839d7fe010b4d0255ec12b5be4a2966f7f6")
        pe = pefile.PE(data=raw)
        code = pe.get_data(0x109522AE - pe.OPTIONAL_HEADER.ImageBase, 9)
        self.assertEqual(code.hex(), "8b4dc8890d54971211")  # mov ecx,[ebp-38h]; mov [11129754h],ecx
        decoder = Cs(CS_ARCH_X86, CS_MODE_32)
        decoder.detail = True
        def instruction(decoded):
            return Instruction(decoded.address, decoded.size, decoded.mnemonic, decoded.op_str, decoded.bytes, decoded.operands)
        decoded = [instruction(d) for d in decoder.disasm(code, 0x109522AE)]
        lifter = CheckedLifter()
        self.assertEqual(lifter.lift_instruction(decoded[0]), ["ecx = MEM32(ebp + (-0x38)); /* 0x109522AE: mov ecx, dword ptr [ebp - 0x38] */"])
        self.assertEqual(lifter.lift_instruction(decoded[1]), [EMISSION])
        source = (ROOT / "generated/bioshock/functions_0022.c").read_text(encoding="utf-8")
        self.assertEqual(source.count("    " + EMISSION + "\n"), 1)
        with self.assertRaisesRegex(ValueError, "CPU count store bytes"):
            lifter.lift_instruction(instruction(next(decoder.disasm(bytes.fromhex("890d58971211"), 0x109522B1))))
        host = (ROOT / "android/host/core.c").read_text(encoding="utf-8")
        body = host[host.index("uint32_t void_bioshock_engine_cpu_count(uint32_t processors)"):]
        body = body[:body.index("\n}\n") + 3]
        self.assertIn("uint32_t engine = single && *single == '0' ? processors : 1u;", body)
        self.assertIn("return engine;", body)
        flags = (ROOT / "android/host/android_main.c").read_text(encoding="utf-8")
        self.assertTrue(re.search(r'"BIOSHOCK_ENGINE_SINGLE_CORE"', flags))


if __name__ == "__main__":
    unittest.main()

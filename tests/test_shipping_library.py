"""Real strip of the built game: preserve symbols/ABI and reject changed code."""
import hashlib
import os
from pathlib import Path
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
from build_apk import stage_shipping_library
from elf_identity import assert_runtime_identity


class ShippingLibraryTest(unittest.TestCase):
    def test_real_strip_preserves_runtime_and_rejects_forged_payload(self):
        library = ROOT / "build/android/libbioshock.so"
        strip = Path(os.environ["LOCALAPPDATA"]) / "Android/Sdk/ndk/27.2.12479018/toolchains/llvm/prebuilt/windows-x86_64/bin/llvm-strip.exe"
        if not library.is_file():
            self.skipTest("Requires the built game library for the real strip regression")
        source = library.read_bytes()
        source_sha = hashlib.sha256(source).hexdigest()
        with tempfile.TemporaryDirectory(prefix="bioshock-shipping-") as temporary:
            destination = Path(temporary)
            shipped, receipt = stage_shipping_library(library, destination, strip)
            data = shipped.read_bytes()
            self.assertEqual(receipt["source_library_sha256"], source_sha)
            self.assertEqual(receipt["symbols_sha256"], source_sha)
            self.assertEqual(receipt["library_sha256"], hashlib.sha256(data).hexdigest())
            self.assertEqual(Path(receipt["symbols_library"]).read_bytes(), source)
            self.assertLess(len(data), len(source))
            identity = assert_runtime_identity(source, data)
            self.assertEqual(identity["allocated_sha256"], receipt["allocated_identity"])
            self.assertTrue(identity["jni_exports"])
            self.assertTrue(identity["has_symbols"])
            forged = bytearray(data)
            text = next(section for section in identity["allocated_sections"] if section["name"] == ".text")
            forged[text["offset"] + text["size"] // 2] ^= 1
            with self.assertRaisesRegex(ValueError, "runtime payload or ABI differs"):
                assert_runtime_identity(source, bytes(forged))
        self.assertEqual(hashlib.sha256(library.read_bytes()).hexdigest(), source_sha)
        print("PASS real shipping strip: exact unstripped archive, full runtime/ALLOC/JNI parity, shipped receipt SHA and rejection of forged code")


if __name__ == "__main__":
    unittest.main()

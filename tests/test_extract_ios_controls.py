import hashlib
from pathlib import Path
import struct
import tempfile
import unittest
from unittest.mock import patch
from zipfile import ZipFile
import zlib

from tools import extract_ios_controls as controls


class OriginalIosAssetsTest(unittest.TestCase):
    def test_crop_uses_top_left_coordinates_and_preserves_alpha(self):
        atlas = b"offset" + bytes(range(24))  # 3 × 2 BGRA pixels
        image = controls.crop_png(atlas, 6, 3, (1, 0, 1, 2))
        self.assertEqual(image[:8], b"\x89PNG\r\n\x1a\n")
        self.assertEqual(struct.unpack_from(">II", image, 16), (1, 2))
        size, kind = struct.unpack_from(">I4s", image, 33)
        self.assertEqual(kind, b"IDAT")
        self.assertEqual(zlib.decompress(image[41:41 + size]), bytes((0, 6, 5, 4, 7, 0, 18, 17, 16, 19)))

    def test_same_size_changed_source_is_rejected_before_writing(self):
        with tempfile.TemporaryDirectory() as temp:
            directory = Path(temp)
            ipa = directory / "original.ipa"
            original = b"original"
            sources = {controls.PACKAGE: (len(original), hashlib.sha256(original).hexdigest())}
            with ZipFile(ipa, "w") as source:
                source.writestr(controls.PACKAGE, b"modified")
            destination = directory / "output"
            with patch.object(controls, "SOURCES", sources), self.assertRaisesRegex(ValueError, "Unexpected SHA-256"):
                controls.extract(ipa, destination)
            self.assertFalse(destination.exists())


if __name__ == "__main__":
    unittest.main()

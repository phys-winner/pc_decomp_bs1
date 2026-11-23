"""Verify ZIP64 metadata for joining stored OBBs without duplicating payload."""
from pathlib import Path
import struct
import tempfile
import unittest
import zipfile

from tools import obb_stored_merge


class ObbStoredMergeTest(unittest.TestCase):
    def test_joined_zip_reads_all_entries_and_zip64_offsets(self):
        with tempfile.TemporaryDirectory(prefix='bioshock-obb-merge-', dir=Path('build')) as temporary:
            root = Path(temporary)
            main, patch = root / 'main.obb', root / 'patch.obb'
            with zipfile.ZipFile(main, 'w') as z:
                z.writestr('game/Builds/Release/Bioshock.exe', b'MZ' + bytes(1000), zipfile.ZIP_STORED)
            with zipfile.ZipFile(patch, 'w') as z:
                z.writestr('game/Content/System/Startup.ini', b'[Startup]\r\n', zipfile.ZIP_STORED)
                z.writestr('game/Content/BulkContent/data.bin', bytes(range(256)) * 20, zipfile.ZIP_STORED)
            data, receipt = obb_stored_merge.metadata(main, patch)
            combined = main.read_bytes() + patch.read_bytes() + data
            self.assertEqual(len(combined), receipt['combined_bytes'])
            joined = root / 'joined.zip'
            joined.write_bytes(combined)
            with zipfile.ZipFile(joined) as z:
                self.assertEqual(len(z.infolist()), 3)
                self.assertEqual(z.read('game/Builds/Release/Bioshock.exe'), b'MZ' + bytes(1000))
                self.assertEqual(z.read('game/Content/System/Startup.ini'), b'[Startup]\r\n')
                self.assertEqual(z.read('game/Content/BulkContent/data.bin'), bytes(range(256)) * 20)
                self.assertIsNone(z.testzip())
            extra = obb_stored_merge.zip64_extra(b'', 0x100000123, False)
            self.assertEqual(extra, struct.pack('<HHQ', 1, 8, 0x100000123))
            original = obb_stored_merge.zip64_extra(struct.pack('<HHQ', 1, 8, 0x100), 0x100000123, True)
            self.assertEqual(original, struct.pack('<HHQ', 1, 8, 0x100000123))


if __name__ == '__main__':
    unittest.main()

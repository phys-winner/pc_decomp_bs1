"""Check nested ZIP64 and physical alignment of a whole OBB in a split APK."""
from pathlib import Path
import tempfile
import zipfile
import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from tools.build_data_obb import data_offset
from tools.build_obb_splits import add_obb_asset

ROOT = Path(__file__).resolve().parents[1]


def main():
    with tempfile.TemporaryDirectory(prefix="bioshock-split-test-", dir=ROOT / "build") as temporary:
        root = Path(temporary)
        obb = root / "main.1.org.bioshock.port.obb"
        payload = b"BioShock\0" * 1000
        with zipfile.ZipFile(obb, "w", compression=zipfile.ZIP_STORED) as archive:
            name = "game/Content/test.bin"
            info = zipfile.ZipInfo(name, (2007, 8, 21, 0, 0, 0))
            info.compress_type = zipfile.ZIP_STORED
            info.extra = bytes((-30 - len(name)) % 16384)
            archive.writestr(info, payload)
        apk = root / "main.apk"
        with zipfile.ZipFile(apk, "w", compression=zipfile.ZIP_STORED) as archive:
            archive.writestr("AndroidManifest.xml", b"manifest")
        asset_offset = add_obb_asset(apk, obb, obb.name)
        with zipfile.ZipFile(apk) as outer:
            assert obb.name not in outer.namelist()
            asset = outer.getinfo("assets/" + obb.name)
            assert data_offset(asset, outer) == asset_offset
            assert outer.read(asset) == obb.read_bytes()
        import struct
        assert asset.flag_bits & 8 == 0
        with apk.open("rb") as stream:
            stream.seek(asset.header_offset + 18)
            assert struct.unpack("<II", stream.read(8)) == (obb.stat().st_size, obb.stat().st_size)
        with zipfile.ZipFile(obb) as inner:
            entry = inner.getinfo("game/Content/test.bin")
            assert (asset_offset + data_offset(entry, inner)) % 16384 == 0
        print("PASS split nested ZIP64, byte-identical OBB and 16384 physical alignment")


if __name__ == "__main__":
    main()

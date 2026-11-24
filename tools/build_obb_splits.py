"""Package whole direct-read OBBs as aligned asset-only APK splits."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import xml.etree.ElementTree as ET
import zipfile

try:
    from .build_data_obb import data_offset, sha
except ImportError:
    from build_data_obb import data_offset, sha

ROOT = Path(__file__).resolve().parents[1]
ALIGNMENT = 16384


class SplitZipFile(zipfile.ZipFile):
    def _write_end_record(self):
        import struct
        for info in self.filelist:
            if max(info.file_size, info.compress_size, info.header_offset) >= 4 * 1024 ** 3:
                raise ValueError(f"Ordinary APK entry fields required: {info.filename}")
            date = (info.date_time[0] - 1980) << 9 | info.date_time[1] << 5 | info.date_time[2]
            time = info.date_time[3] << 11 | info.date_time[4] << 5 | info.date_time[5] // 2
            name, flags = info._encodeFilenameFlags()
            self.fp.write(struct.pack("<I6H3I5H2I", 0x02014B50,
                info.create_version | info.create_system << 8, info.extract_version,
                flags, info.compress_type, time, date, info.CRC, info.compress_size,
                info.file_size, len(name), len(info.extra), len(info.comment), 0,
                info.internal_attr, info.external_attr, info.header_offset))
            self.fp.write(name)
            self.fp.write(info.extra)
            self.fp.write(info.comment)
        end = self.fp.tell()
        count = len(self.filelist)
        size = end - self.start_dir
        offset = self.start_dir
        zip64 = count > zipfile.ZIP_FILECOUNT_LIMIT or size > zipfile.ZIP64_LIMIT or offset > zipfile.ZIP64_LIMIT
        if zip64:
            self.fp.write(struct.pack("<IQHHIIQQQQ", 0x06064B50, 44, 45, 45, 0, 0, count, count, size, offset))
            self.fp.write(struct.pack("<IIQI", 0x07064B50, 0, end, 1))
        self.fp.write(struct.pack("<I4H2IH", 0x06054B50, 0, 0,
            min(count, 0xFFFF), min(count, 0xFFFF), min(size, 0xFFFFFFFF), min(offset, 0xFFFFFFFF), len(self._comment)))
        self.fp.write(self._comment)
        if self.mode == "a":
            self.fp.truncate()
        self.fp.flush()


def add_obb_asset(apk, obb, asset_name, alignment=ALIGNMENT):
    import zlib
    if not asset_name.startswith("assets/"):
        asset_name = "assets/" + asset_name
    size = obb.stat().st_size
    if size >= 4 * 1024 ** 3:
        raise ValueError("An ordinary local ZIP entry cannot hold this OBB")
    crc = 0
    with obb.open("rb") as source:
        for chunk in iter(lambda: source.read(8 * 1024 * 1024), b""):
            crc = zlib.crc32(chunk, crc)
    with SplitZipFile(apk, "a", compression=zipfile.ZIP_STORED, allowZip64=True) as archive:
        if asset_name in archive.namelist():
            raise ValueError(f"Duplicate asset: {asset_name}")
        info = zipfile.ZipInfo(asset_name, (2007, 8, 21, 0, 0, 0))
        info.compress_type = zipfile.ZIP_STORED
        info.create_system = 0
        info.external_attr = 0o100444 << 16
        info.file_size = info.compress_size = size
        info.CRC = crc
        info.header_offset = archive.start_dir
        info.extra = bytes((-archive.start_dir - 30 - len(asset_name.encode("utf-8"))) % alignment)
        target = archive.fp
        target.write(info.FileHeader(zip64=False))
        with obb.open("rb") as source:
            shutil.copyfileobj(source, target, 8 * 1024 * 1024)
        archive.filelist.append(info)
        archive.NameToInfo[info.filename] = info
        archive.start_dir = archive.fp.tell()
        archive._didModify = True
        offset = data_offset(info, archive)
    if offset % alignment:
        raise ValueError(f"Nested OBB is not {alignment}-byte aligned: {asset_name} offset={offset} header={info.header_offset} extra={len(info.extra)} flags={info.flag_bits}")
    return offset


def build_split(obb, output, split_name, base_manifest, tools, android, signer, certificate, key):
    output.parent.mkdir(parents=True, exist_ok=True)
    namespace = "{http://schemas.android.com/apk/res/android}"
    version_code = base_manifest.attrib[namespace + "versionCode"]
    version_name = base_manifest.attrib[namespace + "versionName"]
    with tempfile.TemporaryDirectory(prefix="bioshock-split-", dir=output.parent) as temporary:
        stage = Path(temporary)
        manifest = stage / "AndroidManifest.xml"
        manifest.write_text(f'''<?xml version="1.0" encoding="utf-8"?>
<manifest xmlns:android="http://schemas.android.com/apk/res/android" package="org.bioshock.port"
    android:versionCode="{version_code}" android:versionName="{version_name}" split="{split_name}" android:isFeatureSplit="false">
  <uses-sdk android:minSdkVersion="30" android:targetSdkVersion="35" />
  <application android:hasCode="false" />
</manifest>
''')
        unsigned = stage / "unsigned.apk"
        subprocess.run([str(tools / "aapt2.exe"), "link", "-I", str(android), "--manifest", str(manifest), "-o", str(unsigned)], check=True)
        offset = add_obb_asset(unsigned, obb, obb.name)
        java = Path(os.environ["JAVA_HOME"]) / "bin/java.exe"
        subprocess.run([str(java), "-jar", str(signer), "sign", "--key", str(key), "--cert", str(certificate),
            "--v1-signing-enabled", "true", "--v2-signing-enabled", "false", "--v3-signing-enabled", "false",
            "--out", str(output), str(unsigned)], check=True)
        subprocess.run([str(java), "-jar", str(signer), "verify", "--verbose", str(output)], check=True)
    receipt = {"apk": str(output), "split": split_name, "sha256": sha(output),
               "nested_obb": obb.name, "nested_sha256": sha(obb), "asset_offset": offset, "alignment": ALIGNMENT}
    output.with_suffix(".json").write_text(json.dumps(receipt, indent=2) + "\n")
    return receipt


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--obb-directory", type=Path, default=ROOT / "build/obb-16k")
    parser.add_argument("--output", type=Path, default=ROOT / "build/splits")
    args = parser.parse_args()
    sdk = Path(os.environ.get("ANDROID_SDK_ROOT", Path.home() / "AppData/Local/Android/Sdk"))
    machine = json.loads((Path.home() / ".agents/skills/adapt-arm32-game/machine_config.json").read_text())
    signing = machine["signer"]
    signer_dir = Path(signing["dir"])
    certificate = signer_dir / signing["cert"]
    key = signer_dir / signing["key"]
    expected = signing["expected_cert_sha256"]
    import ssl
    if hashlib.sha256(ssl.PEM_cert_to_DER_cert(certificate.read_text())).hexdigest() != expected:
        parser.error("Signing certificate mismatch")
    base_manifest = ET.parse(ROOT / "android/app/AndroidManifest.xml").getroot()
    receipts = []
    for part, split in (("main", "obb_main"), ("patch", "obb_patch")):
        obb = args.obb_directory / f"{part}.1.org.bioshock.port.obb"
        if not obb.is_file():
            parser.error(f"Missing OBB: {obb}")
        receipt = build_split(obb, args.output / f"{part}.apk", split, base_manifest,
            sdk / "build-tools/36.0.0", sdk / "platforms/android-36/android.jar",
            signer_dir / signing["apksigner"], certificate, key)
        receipts.append(receipt)
    print(json.dumps(receipts, indent=2))


if __name__ == "__main__":
    main()

"""Compile and run production zipmount against a stored ZIP and ZMI1 fixture."""
import importlib.util
from pathlib import Path
import struct
import subprocess
import tempfile
import unittest
import zipfile
import zlib

ROOT = Path(__file__).resolve().parents[1]


def data_offset(info):
    return info.header_offset + 30 + len(info.filename.encode()) + len(info.extra)


class ZipmountTest(unittest.TestCase):
    def test_stored_reads_sidecar_listing_and_bad_archive(self):
        spec = importlib.util.spec_from_file_location('zipmount_host_env', ROOT / 'tests/host_environment.py')
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
        env, linker = module.environment('x64')
        with tempfile.TemporaryDirectory(prefix='bioshock-zipmount-', dir=ROOT / 'build') as temporary:
            folder = Path(temporary)
            archive = folder / 'stored.zip'
            big = bytes((i * 73 + 19) & 255 for i in range(300000))
            with zipfile.ZipFile(archive, 'w') as z:
                z.writestr('data/big.bin', big, zipfile.ZIP_STORED)
                z.writestr('data/small.txt', b'small file', zipfile.ZIP_STORED)
                z.writestr('data/sub/x.bin', b'x' * 1000, zipfile.ZIP_STORED)
                z.writestr('packed.txt', b'compressed ' * 100, zipfile.ZIP_DEFLATED)
            records = []
            with zipfile.ZipFile(archive) as z:
                for info in z.infolist():
                    flags = 1 if info.compress_type else 0
                    records.append((info.filename, data_offset(info), info.file_size, info.CRC, flags, 0))
            records.sort(key=lambda row: row[0].casefold())
            index = folder / 'mount.zmi'
            with index.open('wb') as f:
                f.write(struct.pack('<4sIQ', b'ZMI1', len(records), archive.stat().st_size) + bytes(32))
                for name, offset, size, crc, flags, timestamp in records:
                    encoded = name.encode()
                    f.write(struct.pack('<QQIIQH', offset, size, crc, flags, timestamp, len(encoded)) + encoded)
            obj, binary = folder / 'test.obj', folder / 'test.exe'
            common = [str(module.CLANG), '/nologo', '/O2', '/W4', '/std:c11', '/D_CRT_SECURE_NO_WARNINGS',
                      '/clang:--target=x86_64-pc-windows-msvc']
            for command in (common + ['/c', 'android/host/zipmount.c', '/Fo' + str(folder / 'zipmount.obj')],
                            common + ['/c', 'tests/test_zipmount.c', '/Fo' + str(obj)],
                            [str(linker), '/nologo', str(folder / 'zipmount.obj'), str(obj), '/OUT:' + str(binary)]):
                result = subprocess.run(command, cwd=ROOT, env=env, capture_output=True, text=True, timeout=120)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            result = subprocess.run([str(binary), str(archive), str(index)], capture_output=True, text=True, timeout=60)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIn('11 checks, 0 failed', result.stdout)
            bad = folder / 'bad.zmi'
            bad.write_bytes(index.read_bytes())
            with bad.open('r+b') as f:
                f.seek(8)
                f.write(struct.pack('<Q', 12345))
            result = subprocess.run([str(binary), str(archive), str(bad)], capture_output=True, text=True, timeout=60)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn('the index opens for its archive', result.stdout)


if __name__ == '__main__':
    unittest.main()

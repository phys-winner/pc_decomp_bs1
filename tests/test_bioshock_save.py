"""Check the production .bsb validator with small synthetic PC saves."""
import os
from pathlib import Path
import shlex
import shutil
import struct
import subprocess
import tempfile
import unittest
import zlib


ROOT = Path(__file__).resolve().parents[1]
HEADER_SIZE = 0x2028


def production_function(source, signature):
    start = source.index(signature)
    end = source.index("{", start) + 1
    depth = 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end]


def compact(value):
    output = bytearray([value & 0x3f])
    value >>= 6
    if value:
        output[0] |= 0x40
    while value:
        byte = value & 0x7f
        value >>= 7
        output.append(byte | (0x80 if value else 0))
    return bytes(output)


def save_string(value):
    data = (value + "\0").encode("utf-16le")
    return compact(len(data) // 2) + data


def png_chunk(kind, data):
    return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data))


THUMBNAIL = (b"\x89PNG\r\n\x1a\n" + png_chunk(b"IHDR", struct.pack(">2I5B", 1, 1, 8, 2, 0, 0, 0))
             + png_chunk(b"IDAT", zlib.compress(b"\0\xff\x80\x40")) + png_chunk(b"IEND", b""))


def make_save(*, thumbnail=THUMBNAIL, name="QuickSave", title="Medical Pavilion", compressed=True):
    header = struct.pack("<4sIIQI", b"RGMH", 1, HEADER_SIZE, 0, len(thumbnail)).ljust(HEADER_SIZE, b"\0")
    metadata = save_string(name) + save_string(title) + b"\x01" + save_string("mainSave.bsg")
    payload = b"\x10" * 0x8000
    package_header = struct.pack("<9I", 0x9e2a83c1, 0x38008d, 0x760001 if compressed else 1,
                                 1, 64, 1, 65, 1, 66).ljust(64, b"\0")
    if compressed:
        packed = zlib.compress(payload)
        payload = struct.pack("<I", len(packed)) + packed
    package = package_header + payload
    return header + thumbnail + metadata + compact(len(package)) + package


class BioshockSaveTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.runner = []
        if os.name == "nt":
            if not shutil.which("wsl"):
                raise unittest.SkipTest("requires a POSIX C compiler with zlib (WSL on Windows)")
            cls.runner = ["wsl", "--exec"]
        compiler = shlex.split(os.environ.get("CC", "cc")) if not cls.runner else ["cc"]
        check = subprocess.run(cls.runner + compiler + ["--version"], cwd=ROOT, capture_output=True, timeout=30)
        if check.returncode:
            raise unittest.SkipTest("requires a POSIX C compiler with zlib (WSL on Windows)")
        output_root = ROOT / "build" / "tests"
        output_root.mkdir(parents=True, exist_ok=True)
        folder = tempfile.TemporaryDirectory(prefix="save-", dir=output_root)
        cls.addClassCleanup(folder.cleanup)
        cls.folder = Path(folder.name).relative_to(ROOT)
        cls.binary = (cls.folder / "check-save").as_posix()
        result = subprocess.run(cls.runner + compiler + [
            "-std=gnu11", "-Wall", "-Wextra", "-Werror", "tests/test_bioshock_save.c",
            "android/host/bioshock_save.c", "-lz", "-o", cls.binary,
        ], cwd=ROOT, capture_output=True, text=True, errors="replace", timeout=60)
        if result.returncode:
            raise AssertionError(result.stdout + result.stderr)

    def check_saves(self, cases):
        files = []
        for name, (data, _) in cases.items():
            path = self.folder / (name + ".bsb")
            (ROOT / path).write_bytes(data)
            files.append(path.as_posix())
        result = subprocess.run(self.runner + [self.binary, *files], cwd=ROOT, capture_output=True,
                                text=True, errors="replace", timeout=30)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stdout.splitlines(), [expected for _, expected in cases.values()])

    def test_valid_pc_saves(self):
        self.check_saves({
            "compressed": (make_save(), "valid"),
            "uncompressed-no-screenshot": (make_save(thumbnail=b"", compressed=False), "valid"),
            "unicode-and-two-byte-count": (make_save(name="Сохранение", title="М" * 100), "valid"),
        })

    def test_damage_before_and_after_menu_metadata(self):
        valid = make_save()
        metadata = HEADER_SIZE + len(THUMBNAIL)
        broken_png = bytearray(valid)
        broken_png[HEADER_SIZE + 18] ^= 1
        self.check_saves({
            "empty": (b"", "truncated header"),
            "partial-fixed-header": (valid[:4096], "truncated header"),
            "partial-thumbnail": (valid[:metadata - 2], "truncated thumbnail or metadata"),
            "bad-png-crc": (broken_png, "invalid thumbnail"),
            "partial-metadata": (valid[:metadata + 4], "invalid save names"),
            "oversized-title": (make_save(title="x" * 128), "invalid save names"),
            "invalid-compact": (valid[:metadata] + b"\x40\x80\x80\x80\x10" + valid[metadata:],
                                "invalid save names"),
            "invalid-name": (make_save(name="../QuickSave"), "invalid save names"),
            "partial-package": (valid[:-1], "truncated or invalid package"),
            "zeroed-tail": (valid[:-16] + bytes(16), "truncated or invalid package"),
        })

    def test_production_atomic_writer_and_quarantine(self):
        source = (ROOT / "android/host/files.c").read_text(encoding="utf-8")
        signatures = ["static int under(", "static int profile_save_path(",
                      "static int save_guard_enabled(", "static int save_validate_fd(",
                      "static int save_same_file(", "static int save_quarantine(",
                      "static int save_skip_damaged(", "static int save_finish(",
                      "static uint32_t create_save_writer(", "WINAPI_EXPORT(K32, WriteFile, 5)"]
        code = "extern const char *void_bioshock_save_damage(FILE *, uint64_t, int *);\n"
        code += "\n".join(production_function(source, signature) for signature in signatures)
        (ROOT / self.folder / "save-io-production.inc").write_bytes(code.encode("utf-8"))
        fixture = self.folder / "io-valid.bsb"
        (ROOT / fixture).write_bytes(make_save())
        data = self.folder / "profile"
        (ROOT / data).mkdir()
        binary = (self.folder / "save-io").as_posix()
        compiler = shlex.split(os.environ.get("CC", "cc")) if not self.runner else ["cc"]
        command = self.runner + compiler + [
            "-std=gnu11", "-Wall", "-Wextra", "-Werror", "-pthread", "-I" + self.folder.as_posix(),
            "tests/test_bioshock_save_io.c", "android/host/bioshock_save.c", "-lz", "-o", binary,
        ]
        result = subprocess.run(command, cwd=ROOT, capture_output=True, text=True, errors="replace", timeout=60)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        result = subprocess.run(self.runner + [binary, data.as_posix(), fixture.as_posix()],
                                cwd=ROOT, capture_output=True, text=True, errors="replace", timeout=30)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("PASS production save I/O", result.stdout)

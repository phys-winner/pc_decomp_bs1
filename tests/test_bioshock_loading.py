"""Check the exact pending-content seam and bounded, state-preserving backoff."""
from pathlib import Path
import re
import subprocess
import sys
import unittest

import pefile

from tests.host_environment import CLANG, environment

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / "build/tests/bioshock-loading"

HARNESS = r'''
#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
static uint8_t data[4096], code[59];
static uint32_t queue_root;
static int owned = 1, enabled = 1, begins, ends, sleeps, interrupted;
static uint8_t registers[256], saved_registers[256];
static void *pointer(uint32_t address) {
    if (address >= 0x10000 && address < 0x11000) return data+address-0x10000;
    if (address >= 0x10B65469 && address < 0x10B65469+sizeof code) return code+address-0x10B65469;
    assert(address == 0x1128F03C); return &queue_root;
}
#define GPTR(a) pointer((uint32_t)(a))
#define MEM32(a) (*(uint32_t *)pointer((uint32_t)(a)))
static int void_machine_is_owned(void) { return owned; }
static int void_vm_readable(uint32_t address, size_t size) {
    return (address >= 0x10000 && address <= 0x11000 && size <= 0x11000-address) ||
        (address >= 0x10B65469 && address <= 0x10B65469+sizeof code && size <= 0x10B65469+sizeof code-address) ||
        (address == 0x1128F03C && size == 4);
}
static const char *fixture_getenv(const char *name) { assert(!strcmp(name, "BIOSHOCK_LOADING_BACKOFF")); return enabled ? NULL : "0"; }
#define getenv fixture_getenv
static void void_log(const char *format, ...) { (void)format; }
static void void_blocking_begin(void) {
    assert(owned && begins == ends); ++begins; owned = 0;
    memcpy(saved_registers, registers, sizeof registers); memset(registers, 0xde, sizeof registers);
}
static void void_blocking_end(void) {
    assert(!owned && begins == ends+1); ++ends; owned = 1;
    memcpy(registers, saved_registers, sizeof registers);
}
static int fixture_nanosleep(const struct timespec *delay, struct timespec *remainder) {
    assert(!owned && delay->tv_sec == 0 && delay->tv_nsec <= 50000 && delay->tv_nsec > 0);
    ++sleeps;
    if (interrupted) { interrupted = 0; *remainder = *delay; remainder->tv_nsec /= 2; errno = EINTR; return -1; }
    return 0;
}
#define nanosleep fixture_nanosleep
/* ACTUAL */
static void reset(void) {
    memset(&content_poll, 0, sizeof content_poll); memset(data, 0, sizeof data);
    memcpy(code, content_poll_preimage, sizeof code); content_backoff = -1;
    owned = enabled = 1; begins = ends = sleeps = interrupted = 0;
    queue_root = 0x10300;
    MEM32(0x10100+12) = 0x10200;
    MEM32(0x10300+4) = 0x10400; MEM32(0x10300+8) = 2; MEM32(0x10300+12) = 4;
    MEM32(0x10400) = 0x10500;
    memset(registers, 0xa5, sizeof registers);
}
static void invoke(void) {
    uint8_t before[sizeof registers]; memcpy(before, registers, sizeof before);
    void_bioshock_content_poll(0x10100);
    assert(!memcmp(before, registers, sizeof before) && begins == ends && owned);
}
static void warmup(void) { for (unsigned i = 0; i < 8; ++i) invoke(); }
int main(void) {
    reset(); warmup(); assert(!begins); invoke(); assert(begins == 1 && sleeps == 1);
    interrupted = 1; invoke(); assert(begins == 2 && sleeps == 3);
    MEM32(0x10400) = 0x10540; int previous = begins;
    warmup(); assert(begins == previous); invoke(); assert(begins == previous+1);
    MEM32(0x10300+8) = 1; previous = begins;
    warmup(); assert(begins == previous); invoke(); assert(begins == previous+1);
    const uint32_t blocked_fields[] = {0x10100+24, 0x10100+40, 0x10200+92};
    for (unsigned i = 0; i < sizeof blocked_fields/sizeof *blocked_fields; ++i) {
        MEM32(blocked_fields[i]) = 1; previous = begins; warmup(); assert(begins == previous);
        MEM32(blocked_fields[i]) = 0; warmup(); assert(begins == previous);
    }
    MEM32(0x10300+8) = 0; MEM32(0x10300+4) = 0; previous = begins;
    warmup(); assert(begins == previous); invoke(); assert(begins == previous+1);
    previous = begins;
    MEM32(0x10300+8) = 5; warmup(); assert(begins == previous);
    MEM32(0x10300+8) = 1; MEM32(0x10300+4) = 0xfffffff0u; warmup(); assert(begins == previous);
    reset(); MEM32(0x10100+12) = 0; queue_root = 0;
    warmup(); assert(!begins); invoke(); assert(begins == 1);
    reset(); enabled = 0; warmup(); warmup(); assert(!begins && content_backoff == 0);
    reset(); code[0] ^= 1; warmup(); warmup(); assert(!begins && content_backoff == 0);
    reset(); owned = 0; void_bioshock_content_poll(0x10100); assert(content_backoff == -1 && !begins);
    puts("PASS BioShock content backoff: exact idle seam, empty/pending queues, missing manager/queue, progress/cancel resets, bounded EINTR wait, registers retained and legacy switch");
    return 0;
}
'''


class BioShockLoadingTest(unittest.TestCase):
    def test_bulk_prefetch_uses_original_registration_ranges(self):
        from tools.build_android import SEAMS

        image = pefile.PE(str(ROOT / "generated/input/Bioshock.exe"), fast_load=True)
        source = (ROOT / "generated/bioshock/functions_0107.c").read_text(encoding="utf-8")
        targets = {0x10B64EBB: 0x10EC9E50, 0x10B64F10: 0x10953100}
        for address, before, after in (seam for seam in SEAMS if seam[0] in targets):
            call = image.get_data(address - image.OPTIONAL_HEADER.ImageBase, 5)
            self.assertEqual(call[0], 0xE8)
            self.assertEqual(address + 5 + int.from_bytes(call[1:], "little", signed=True), targets.pop(address))
            self.assertEqual(source.count(before), 1)
            self.assertIn("void_bioshock_prefetch_bulk(MEM32(esp), MEM32(esp + 20), MEM32(esp + 24))", after)
            self.assertTrue(after.endswith(before))
        self.assertFalse(targets)

    def test_production_backoff(self):
        source = (ROOT / "android/host/bioshock_loading.c").read_text(encoding="utf-8")
        preimage = bytes(int(value, 16) for value in re.findall(r"0x([0-9a-f]{2})", source.split("content_poll_preimage[] = {", 1)[1].split("};", 1)[0]))
        image = (ROOT / "generated/input/Bioshock.exe").read_bytes()
        pe = pefile.PE(data=image, fast_load=True)
        self.assertEqual(preimage, pe.get_data(0x10B65469-pe.OPTIONAL_HEADER.ImageBase, len(preimage)))
        self.assertEqual(len(preimage), 59)
        OUT.mkdir(parents=True, exist_ok=True)
        probe = OUT / "probe.c"
        actual = source.replace('#include "host.h"', '').replace('#include <errno.h>', '').replace('#include <time.h>', '')
        probe.write_text(HARNESS.replace("/* ACTUAL */", actual), encoding="utf-8")
        env, linker = environment("x64")
        obj, binary = OUT / "probe.obj", OUT / "probe.exe"
        for command in ([str(CLANG), "/nologo", "/O2", "/W4", "/WX", "/std:c11",
                         "/clang:--target=x86_64-pc-windows-msvc", "/c", str(probe), "/Fo" + str(obj)],
                        [str(linker), "/nologo", str(obj), "/OUT:" + str(binary)], [str(binary)]):
            result = subprocess.run(command, cwd=ROOT, env=env, capture_output=True, text=True, timeout=60)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        result = subprocess.run([str(CLANG.with_name("clang.exe")), "--target=aarch64-linux-android29", "-O2",
                                 "-std=gnu11", str(probe), "-Wl,-z,max-page-size=16384", "-o", str(OUT / "test-bioshock-loading")],
                                capture_output=True, text=True, timeout=60)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_checked_lifter_hook(self):
        subprocess.run([sys.executable, "-B", "-c",
                        "from tests.test_bioshock_loading import BioShockLoadingTest; "
                        "BioShockLoadingTest('test_checked_lifter_hook')._check_lifter()"],
                       cwd=ROOT, check=True, timeout=60)

    def _check_lifter(self):
        sys.path.insert(0, str(ROOT / "tools"))
        from lift_game import CheckedLifter
        from disasm.disasm32 import Instruction
        from capstone import Cs, CS_ARCH_X86, CS_MODE_32
        decoder = Cs(CS_ARCH_X86, CS_MODE_32); decoder.detail = True
        def decode(code, address=0x10B65469):
            i = next(decoder.disasm(bytes.fromhex(code), address))
            return Instruction(i.address, i.size, i.mnemonic, i.op_str, i.bytes, i.operands)
        emission = CheckedLifter().lift_instruction(decode("8b460c"))
        self.assertEqual(emission[1], "eax = MEM32(esi + 0xC); /* 0x10B65469: mov eax, dword ptr [esi + 0xc] */")
        self.assertEqual(emission[0], "{ extern void void_bioshock_content_poll(uint32_t); RECOMP_FLAGS_OUT(); RECOMP_REGS_OUT(); void_bioshock_content_poll(esi); RECOMP_REGS_IN(); RECOMP_FLAGS_IN(); }")
        for changed in ("8b460d", "8b4e0c"):
            with self.assertRaisesRegex(ValueError, "idle content poll"):
                CheckedLifter().lift_instruction(decode(changed))
        self.assertNotIn("void_bioshock_content_poll", "\n".join(CheckedLifter().lift_instruction(decode("8b460c", 0x10B6546A))))
        source = (ROOT / "generated/bioshock/functions_0107.c").read_text(encoding="utf-8")
        self.assertEqual(source.count("\n".join("    " + line for line in emission)), 1)


if __name__ == "__main__":
    unittest.main()

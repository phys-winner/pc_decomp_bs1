"""Differentiate NotifyMouseState publication from the original Flash input tick."""
import hashlib
import json
from pathlib import Path
import struct
import unittest

import pefile
from unicorn import Uc, UC_ARCH_X86, UC_MODE_32, UC_HOOK_CODE
from unicorn.x86_const import UC_X86_REG_EAX, UC_X86_REG_ECX, UC_X86_REG_ESP, UC_X86_REG_EIP

ROOT = Path(__file__).resolve().parents[1]
MOVIE, CHILD, CHILD_VT, BUTTON, META, BUTTON_VT = (0x200000 + i * 0x1000 for i in range(6))
STACK, STOP, HIT, CHILD_ADVANCE, BUTTON_EVENT, MENU_TRACK = 0x108000, 0x50002000, 0x50000100, 0x50000200, 0x50000300, 0x50000400


class OriginalMouse:
    def __init__(self, pe):
        self.vm = Uc(UC_ARCH_X86, UC_MODE_32)
        base = pe.OPTIONAL_HEADER.ImageBase
        self.vm.mem_map(base, pe.OPTIONAL_HEADER.SizeOfImage)
        self.vm.mem_write(base, pe.get_memory_mapped_image())
        for address, size in ((0, 0x1000), (0x100000, 0x10000), (0x200000, 0x6000), (0x50000000, 0x3000)):
            self.vm.mem_map(address, size)
        self.words(0, 0xDEADBEEF)
        self.words(MOVIE + 12, CHILD)
        self.words(CHILD, CHILD_VT)
        self.words(CHILD_VT + 0xDC, HIT)
        self.words(CHILD_VT + 0x10, CHILD_ADVANCE)
        self.vm.mem_write(MOVIE + 0x74, b"\1")  # Already initialized original movie root.
        self.words(BUTTON, BUTTON_VT, META)
        self.words(BUTTON_VT + 0xE0, MENU_TRACK)
        self.words(BUTTON_VT + 0xE4, BUTTON_EVENT)
        self.events = []
        self.vm.hook_add(UC_HOOK_CODE, self.boundary)

    def word(self, address):
        return struct.unpack("<I", self.vm.mem_read(address, 4))[0]

    def words(self, address, *values):
        self.vm.mem_write(address, struct.pack("<" + "I" * len(values), *values))

    def finish(self, popped, result=0):
        stack = self.vm.reg_read(UC_X86_REG_ESP)
        self.vm.reg_write(UC_X86_REG_EAX, result)
        self.vm.reg_write(UC_X86_REG_EIP, self.word(stack))
        self.vm.reg_write(UC_X86_REG_ESP, stack + 4 + popped)

    def boundary(self, vm, address, size, data):
        stack, this = vm.reg_read(UC_X86_REG_ESP), vm.reg_read(UC_X86_REG_ECX)
        if address == 0x10C12330:  # Load an object from the supplied weak-reference fixture.
            out = self.word(stack + 4)
            self.words(out, self.word(this))
            self.finish(4, out)
        elif address in (0x10BF38F0, 0x10BF3900):  # Reference counters only.
            self.finish(0)
        elif address == 0x10C11E00:  # Weak-reference assignment between local fixtures.
            self.words(this, self.word(self.word(stack + 4)))
            self.finish(4, this)
        elif address == 0x10BF7410:  # Weak-reference assignment from an object pointer.
            self.words(this, self.word(stack + 4))
            self.finish(4, this)
        elif address == HIT:  # Original root asks its display tree for the topmost button.
            self.finish(8, BUTTON)
        elif address == MENU_TRACK:
            self.finish(0, 0)
        elif address == CHILD_ADVANCE:
            self.finish(4)
        elif address == BUTTON_EVENT:
            assert this == BUTTON
            self.events.append(self.word(stack + 4) & 255)
            self.finish(4)

    def call(self, entry, *args):
        self.words(STACK, STOP, *args)
        self.vm.reg_write(UC_X86_REG_ESP, STACK)
        self.vm.reg_write(UC_X86_REG_ECX, MOVIE)
        self.vm.emu_start(entry, STOP, count=20000)
        assert self.vm.reg_read(UC_X86_REG_EIP) == STOP
        assert self.vm.reg_read(UC_X86_REG_ESP) == STACK + 4 + 4 * len(args)
        assert self.word(0) == 0xDEADBEEF

    def notify(self, x, y, down):
        self.call(0x10BF6330, x, y, down)

    def advance(self):
        delta = struct.unpack("<I", struct.pack("<f", 1 / 60))[0]
        self.call(0x10BF7A30, delta)

    def processed(self):
        return bytes(self.vm.mem_read(MOVIE + 0x70, 1))[0]


class FlashMouseAckTest(unittest.TestCase):
    def test_original_drag_releases_source_outside_at_destination(self):
        config = json.loads((ROOT / "config/inputs.json").read_text(encoding="utf-8"))
        raw = (Path(config["source_root"]) / "Builds/Release/Bioshock.exe").read_bytes()
        self.assertEqual(hashlib.sha256(raw).hexdigest(), "7f2ee821ad60eaf08322b4bc6ebbf839d7fe010b4d0255ec12b5be4a2966f7f6")
        second = 0x206000

        class DragMouse(OriginalMouse):
            def __init__(self, pe):
                super().__init__(pe)
                self.vm.mem_map(second, 0x2000)
                self.words(second, BUTTON_VT, second + 0x1000)

            def boundary(self, vm, address, size, data):
                if address == HIT:
                    # Two real hit targets at the rendering-only tree boundary.
                    self.finish(8, BUTTON if self.word(MOVIE + 44) < 200 else second)
                elif address == BUTTON_EVENT:
                    stack, target = vm.reg_read(UC_X86_REG_ESP), vm.reg_read(UC_X86_REG_ECX)
                    self.events.append((target, self.word(stack + 4) & 255))
                    self.finish(4)
                else:
                    super().boundary(vm, address, size, data)

        mouse = DragMouse(pefile.PE(data=raw))
        for x, y, down in ((100, 100, 0), (100, 100, 1), (300, 300, 1), (300, 300, 0)):
            mouse.notify(x, y, down)
            mouse.advance()
        self.assertEqual(mouse.events.count((BUTTON, 1)), 1)  # Original source onPress.
        self.assertEqual(mouse.events.count((BUTTON, 3)), 1)  # Original source onReleaseOutside.
        self.assertNotIn((BUTTON, 2), mouse.events)  # Not an inside release cancelling the pickup.
        self.assertNotIn((second, 1), mouse.events)  # A drag never presses the target a second time.
        self.assertEqual(mouse.events[-1], (second, 4))
        self.assertEqual(mouse.processed(), 0)
        self.assertEqual((mouse.word(MOVIE + 44), mouse.word(MOVIE + 48)), (300, 300))
        print("PASS original x86 held diagonal drag: source press once, source releaseOutside once at destination, no inside release or target press, released neutral")

    def test_original_notify_is_not_hover_or_button_ack(self):
        config = json.loads((ROOT / "config/inputs.json").read_text(encoding="utf-8"))
        raw = (Path(config["source_root"]) / "Builds/Release/Bioshock.exe").read_bytes()
        self.assertEqual(hashlib.sha256(raw).hexdigest(), "7f2ee821ad60eaf08322b4bc6ebbf839d7fe010b4d0255ec12b5be4a2966f7f6")
        pe = pefile.PE(data=raw)
        slow = OriginalMouse(pe)
        slow.notify(800, 650, 0)
        self.assertEqual((slow.word(MOVIE + 44), slow.word(MOVIE + 48)), (800, 650))
        self.assertEqual(slow.word(MOVIE + 104), 0)  # Published point, not yet hit-tested.
        slow.advance()
        self.assertEqual(slow.word(MOVIE + 104), BUTTON)
        self.assertIn(4, slow.events)  # Original rollOver.
        slow.notify(800, 650, 1)
        self.assertEqual(slow.word(MOVIE + 52), 1)
        self.assertEqual(slow.processed(), 0)
        slow.advance()
        self.assertEqual(slow.processed(), 1)
        self.assertEqual(slow.events.count(1), 1)  # Original press.
        slow.notify(800, 650, 0)
        self.assertEqual(slow.processed(), 1)
        slow.advance()
        self.assertEqual(slow.processed(), 0)
        self.assertEqual(slow.events.count(2), 1)  # Original release.
        slow.advance()
        self.assertEqual((slow.events.count(1), slow.events.count(2)), (1, 1))

        fast = OriginalMouse(pe)
        fast.notify(800, 650, 0)
        fast.advance()
        fast.notify(800, 650, 1)
        fast.notify(800, 650, 0)  # Two input pumps may publish both edges before one Flash tick.
        fast.advance()
        self.assertEqual((fast.events.count(1), fast.events.count(2)), (0, 0))
        print("PASS original x86 Notify/Advance/mouse state machine: Notify xy precedes hover; LMB+0x70 acknowledges real press/release; DOWN+UP before Advance loses the click; one Advance per edge delivers exactly one release")


if __name__ == "__main__":
    unittest.main()

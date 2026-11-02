"""Synthetic instruction graphs; no game files or process access."""
import struct
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
from inspect_native_view_route import Image, Section, verify, verify_vtable_slot


BASE = 0x140000000


def rel(opcode, site, target):
    return bytes([opcode]) + struct.pack("<i", target - site - 5)


class NativeRouteTests(unittest.TestCase):
    def test_direct_call_requires_real_reachable_instruction_boundary(self):
        # A call-looking byte inside mov eax, immediate is not an instruction.
        code = b"\xb8\xe8\x00\x00\x00" + rel(0xE8, 0x1005, 0x1100) + b"\xc3"
        image = Image(BASE, [Section(0x1000, code, True), Section(0x1100, b"\xc3", True)])
        good = verify(image, 0x1000, 0x1005, "call", target=0x1100)
        self.assertTrue(good["verified"])
        bad = verify(image, 0x1000, 0x1001, "call")
        self.assertFalse(bad["verified"])
        self.assertEqual(bad["reason"], "site_not_reached")

    def test_jump_reaches_a_second_executable_section(self):
        image = Image(BASE, [Section(0x1000, rel(0xE9, 0x1000, 0x3000), True),
                             Section(0x3000, rel(0xE8, 0x3000, 0x3010) + b"\xc3", True)])
        result = verify(image, 0x1000, 0x3000, "call", target=0x3010)
        self.assertTrue(result["verified"])

    def test_unconditional_jump_does_not_certify_dead_fallthrough(self):
        code = rel(0xE9, 0x1000, 0x100A) + rel(0xE8, 0x1005, 0x1100) + b"\xc3"
        result = verify(Image(BASE, [Section(0x1000, code, True)]), 0x1000, 0x1005, "call")
        self.assertFalse(result["verified"])

    def test_conditional_branches_and_cycles_are_bounded(self):
        # jne +1 -> ret, or nop then ret.
        image = Image(BASE, [Section(0x1000, b"\x75\x01\x90\xc3", True)])
        self.assertTrue(verify(image, 0x1000, 0x1003, "ret")["verified"])
        loop = Image(BASE, [Section(0x1000, b"\xeb\xfe", True)])
        self.assertFalse(verify(loop, 0x1000, 0x1002, "ret")["verified"])

    def test_data_section_and_partial_instruction_reject(self):
        for image in (Image(BASE, [Section(0x1000, b"\xc3", False)]),
                      Image(BASE, [Section(0x1000, b"\xe8\x01", True)])):
            self.assertFalse(verify(image, 0x1000, 0x1000, "ret")["verified"])

    def test_loop_edges_and_overlapping_streams_are_not_silently_dropped(self):
        image = Image(BASE, [Section(0x1000, b"\xe2\x01\xb8\x90\x90\x90\x90\xc3", True)])
        result = verify(image, 0x1000, 0x1002, "mov")
        self.assertFalse(result["verified"])
        self.assertIn("overlapping_instruction_streams", result["limitations"])
        for opcode in (0xE0, 0xE1, 0xE2):
            image = Image(BASE, [Section(0x1000, bytes([opcode, 1, 0xC3, 0x90, 0xC3]), True)])
            self.assertTrue(verify(image, 0x1000, 0x1003, "nop")["verified"])

    def test_rip_target_and_wrong_operands_are_checked(self):
        code = b"\x48\x8b\x0d" + struct.pack("<i", 0x2000 - 0x1007) + b"\xc3"
        image = Image(BASE, [Section(0x1000, code, True)])
        self.assertTrue(verify(image, 0x1000, 0x1000, "mov", target=0x2000)["verified"])
        self.assertFalse(verify(image, 0x1000, 0x1000, "mov", target=0x2008)["verified"])
        self.assertFalse(verify(image, 0x1000, 0x1000, "lea", target=0x2000)["verified"])

    def test_budget_exhaustion_never_guesses_an_unseen_site(self):
        image = Image(BASE, [Section(0x1000, b"\x90" * 20 + b"\xc3", True)])
        result = verify(image, 0x1000, 0x1014, "ret", budget=4)
        self.assertFalse(result["verified"])
        self.assertIn("instruction_budget", result["limitations"])

    def test_overlapping_sections_reject(self):
        with self.assertRaises(ValueError):
            Image(BASE, [Section(0x1000, b"\x90\xc3", True), Section(0x1001, b"\xc3", True)])

    def test_read_requires_complete_file_backed_range(self):
        image = Image(BASE, [Section(0x1000, b"\x90\xc3", True)])
        self.assertEqual(image.read(0x1000, 2), b"\x90\xc3")
        for address, size in ((0xFFF, 1), (0x1001, 2), (0x1000, -1)):
            with self.assertRaises(ValueError):
                image.read(address, size)

    def test_vtable_slot_requires_exact_executable_pointer(self):
        # The vtable is file-backed data; the slot points at executable code.
        table = struct.pack("<Q", BASE + 0x3000)
        image = Image(BASE, [Section(0x2000, table, False),
                             Section(0x3000, b"\xc3", True)])
        self.assertTrue(verify_vtable_slot(image, 0x2000, 0, 0x3000)["verified"])
        self.assertFalse(verify_vtable_slot(image, 0x2000, 0, 0x3001)["verified"])
        self.assertFalse(verify_vtable_slot(image, 0x2000, 1, 0x3000)["verified"])
        bad = Image(BASE, [Section(0x2000, struct.pack("<Q", BASE + 0x3000), False),
                           Section(0x3000, b"\xc3", False)])
        self.assertFalse(verify_vtable_slot(bad, 0x2000, 0, 0x3000)["verified"])


if __name__ == "__main__":
    unittest.main()

"""Memory fixtures for the external native-view observer, without Win32 calls."""
import struct
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
from observe_native_views import sample_views, MAIN_VIEW_VTABLE_RVA, MANAGER_RVA, validate_session

BASE = 0x140000000


def fixture():
    blocks = {}
    def put(address, size, fields):
        data = bytearray(size)
        for offset, value in fields.items():
            struct.pack_into("<Q", data, offset, value)
        blocks[address] = bytes(data)
    put(BASE + MANAGER_RVA, 8, {0: 0x10000})
    put(0x10000, 0x18, {8: 0x20000, 0x10: 0x30000})
    put(0x30000, 0x3D8, {0x10: 0x50000, 0x58: 0x60000, 0x3C0: 0x20000, 0x3C8: 0x40000})
    put(0x20000, 0x20, {8: 0x70000, 0x10: 0x70008, 0x18: 0x70010})
    put(0x70000, 8, {0: 0x40000})
    put(0x40000, 0x50, {0: BASE + MAIN_VIEW_VTABLE_RVA, 0x10: 1000,
                        0x38: 0x80000, 0x40: 0x60000, 0x48: 0x50000})
    def read(address, size):
        for start, block in blocks.items():
            if start <= address and address + size <= start + len(block):
                return block[address - start:address - start + size]
        raise OSError("inaccessible")
    return blocks, read


class ViewObservationTests(unittest.TestCase):
    def test_preflight_rejects_same_executable_replacement_or_new_anticheat(self):
        expected = Path("armoredcore6.exe").resolve()
        original = dict(path=str(expected), creation=1, base=BASE, modules=["ac6_coop.dll"])
        self.assertEqual(validate_session(original, original, expected), BASE)
        for changes in (dict(creation=2), dict(base=BASE+0x10000),
                        dict(modules=["ac6_coop.dll", "EasyAntiCheat_EOS.dll"]),
                        dict(modules=[]), dict(path="other.exe")):
            replacement = dict(original, **changes)
            with self.assertRaises(ValueError):
                validate_session(original, replacement, expected)

    def test_coherent_main_list_is_identity_evidence_only(self):
        _, read = fixture()
        report = sample_views(read, BASE)
        self.assertEqual(report["status"], "identities_match")
        self.assertEqual(report["count"], 1)
        self.assertTrue(report["identity_repeat_equal"])
        self.assertFalse(report["frame_associated"])
        self.assertFalse(report["atomic_snapshot"])
        self.assertFalse(report["render_calls_observed"])

    def test_not_initialized_is_distinguished_from_empty_list(self):
        blocks, read = fixture()
        blocks[BASE + MANAGER_RVA] = bytes(8)
        self.assertEqual(sample_views(read, BASE)["status"], "manager_absent")
        blocks, read = fixture()
        blocks[0x20000] = bytes(0x20)
        self.assertEqual(sample_views(read, BASE)["status"], "main_not_listed")

    def test_count_misalignment_and_large_capacity_reject(self):
        for end, cap in ((0x70009, 0x70010), (0x6FFF8, 0x70010),
                         (0x70010, 0x70008), (0x71008, 0x71008)):
            blocks, read = fixture()
            data = bytearray(blocks[0x20000])
            struct.pack_into("<QQ", data, 0x10, end, cap)
            blocks[0x20000] = bytes(data)
            with self.assertRaises(ValueError):
                sample_views(read, BASE)

    def test_wrong_main_camera_or_scene_or_list_rejects_identity(self):
        for address, offset in ((0x40000, 0x48), (0x40000, 0x40), (0x30000, 0x3C0), (0x40000, 0)):
            blocks, read = fixture()
            data = bytearray(blocks[address])
            struct.pack_into("<Q", data, offset, 0x90000)
            blocks[address] = bytes(data)
            self.assertEqual(sample_views(read, BASE)["status"], "identity_mismatch")

    def test_changed_list_during_snapshot_rejects(self):
        blocks, read = fixture()
        calls = 0
        def racing(address, size):
            nonlocal calls
            if address == 0x70000:
                calls += 1
                if calls > 1:
                    return struct.pack("<Q", 0x90000)
            return read(address, size)
        report = sample_views(racing, BASE)
        self.assertEqual(report["status"], "identity_changed")
        self.assertFalse(report["identity_repeat_equal"])

    def test_partial_read_is_rejected(self):
        _, read = fixture()
        def partial(address, size):
            return read(address, size)[:-1]
        with self.assertRaises(ValueError):
            sample_views(partial, BASE)

    def test_additional_view_is_reported_without_claiming_its_role(self):
        blocks, read = fixture()
        data = bytearray(blocks[0x20000]);struct.pack_into("<Q", data, 0x10, 0x70010)
        blocks[0x20000] = bytes(data)
        blocks[0x70000] = struct.pack("<QQ", 0x40000, 0x90000)
        blocks[0x90000] = blocks[0x40000]
        report = sample_views(read, BASE)
        self.assertEqual(report["count"], 2)
        self.assertEqual(report["status"], "identities_match")
        self.assertFalse(report["views"][1]["is_main"])


if __name__ == "__main__":
    unittest.main()

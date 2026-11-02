"""Offline synthetic regressions for the fingerprinted frame boundary census."""
from contextlib import contextmanager
import sys
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))

import inspect_native_frame_boundary as boundary
from inspect_native_view_route import Image, Section


BASE = 0x140000000


def image_with(frame_code=b"\x90\xc3"):
    return Image(BASE, [Section(0x1000, frame_code, True),
                        Section(0x2000, b"\x90\xc3", True),
                        Section(0x3000, b"\x90\xc3", True),
                        Section(0x4000, b"\x90\xc3", True),
                        Section(0x5000, b"\x90\xc3", True)])


@contextmanager
def synthetic_anchors():
    with patch.multiple(boundary,
                        FRAME_ANCHORS=(("frame", 0x1000, 0x1000, "nop", None, None),),
                        RENDER_ANCHORS=(("render", 0x2000, 0x2000, "nop", None, None),),
                        STATE_ANCHORS=(("state", 0x3000, 0x3000, "nop", None, None),),
                        PREPASS_ANCHORS=(("prepass", 0x4000, 0x4000, "nop", None, None),),
                        DISPATCH_ANCHORS=(("dispatch", 0x5000, 0x5000, "nop", None, None),)):
        yield


class FrameBoundaryTests(unittest.TestCase):
    def test_synthetic_anchor_success_does_not_claim_runtime_or_replay(self):
        with synthetic_anchors():
            report = boundary.inspect_image(image_with())
        self.assertTrue(report["complete_graph"])
        self.assertTrue(report["all_anchors_verified"])
        self.assertEqual(report["verified_anchors"],
                         {"frame": 1, "render": 1, "state": 1, "prepass": 1,
                          "dispatch": 1})
        self.assertFalse(report["runtime_observed"])
        self.assertFalse(report["safe_to_reenter_proven"])
        self.assertFalse(report["second_eye_render_proven"])

    def test_wrong_instruction_never_passes_anchor_verification(self):
        with synthetic_anchors():
            report = boundary.inspect_image(image_with(b"\xcc"))
        self.assertFalse(report["all_anchors_verified"])
        self.assertEqual(report["verified_anchors"]["frame"], 0)

    def test_incomplete_cfg_invalidates_otherwise_verified_anchors(self):
        with synthetic_anchors():
            report = boundary.inspect_image(image_with(b"\x90" * 1502 + b"\xc3"))
        self.assertFalse(report["complete_graph"])
        self.assertFalse(report["all_anchors_verified"])
        self.assertIn("instruction_budget", report["graph_limitations"]["0x1000"])
        self.assertTrue(report["groups"]["frame"][0]["verified"])

    def test_dispatch_group_mismatch_invalidates_whole_report(self):
        with synthetic_anchors(), patch.object(
            boundary, "DISPATCH_ANCHORS",
            (("wrong_dispatch", 0x5000, 0x5000, "int3", None, None),)
        ):
            report = boundary.inspect_image(image_with())
        self.assertEqual(report["verified_anchors"]["dispatch"], 0)
        self.assertFalse(report["all_anchors_verified"])

    def test_fingerprint_mismatch_refuses_to_decode(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "not-ac6.exe"
            path.write_bytes(b"MZ" + b"\x00" * 64)
            with self.assertRaisesRegex(ValueError, "fingerprint mismatch"):
                boundary.inspect(path)


if __name__ == "__main__":
    unittest.main()

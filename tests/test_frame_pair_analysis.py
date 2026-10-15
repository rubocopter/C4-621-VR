import sys
import unittest
from pathlib import Path
try:
    import numpy as np
except ImportError:
    np = None
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
from analyze_frame_pair import read_color, read_depth


@unittest.skipIf(np is None, 'Run in .venv-analysis for NumPy snapshot tests')
class FramePairAnalysisTests(unittest.TestCase):
    def test_ten_bit_color_channels_and_padded_rows(self):
        meta = dict(width=2, height=2, color_pitch=256, color_offset=0, color_bytes=512, color_format=24)
        rows = np.zeros((2, 64), dtype='<u4')
        rows[0, :2] = [1023 | (3 << 30), (1023 << 10) | (3 << 30)]
        rows[1, :2] = [(1023 << 20) | (3 << 30), 512 | (256 << 10) | (768 << 20)]
        result = read_color(meta, rows.tobytes())
        np.testing.assert_allclose(result[0], [[1, 0, 0], [0, 1, 0]])
        np.testing.assert_allclose(result[1], [[0, 0, 1], [512 / 1023, 256 / 1023, 768 / 1023]], atol=1e-7)

    def test_float_depth_uses_offset_and_pitch(self):
        meta = dict(width=2, height=2, depth_pitch=256, depth_offset=512, depth_bytes=1024, depth_footprint_format=39)
        raw = bytearray(1024)
        for y, values in enumerate(([0.2, 0.4], [0.8, 1.0])):
            raw[512 + y * 256:520 + y * 256] = np.array(values, dtype='<f4').tobytes()
        np.testing.assert_allclose(read_depth(meta, raw), [[0.2, 0.4], [0.8, 1.0]])

    def test_rejects_bad_pitch_and_truncated_snapshot(self):
        meta = dict(width=2, height=2, color_pitch=4, color_offset=0, color_bytes=8, color_format=24)
        with self.assertRaises(ValueError):
            read_color(meta, bytes(8))
        meta.update(color_pitch=256, color_bytes=512)
        with self.assertRaises(ValueError):
            read_color(meta, bytes(511))

    def test_accepts_footprint_without_last_row_padding(self):
        meta = dict(width=2, height=2, depth_pitch=256, depth_offset=0, depth_bytes=264, depth_footprint_format=39)
        raw = bytearray(264)
        raw[256:264] = np.array([0.3, 0.7], dtype='<f4').tobytes()
        np.testing.assert_allclose(read_depth(meta, raw)[1], [0.3, 0.7])


if __name__ == '__main__':
    unittest.main()

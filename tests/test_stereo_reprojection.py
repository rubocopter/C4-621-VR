from pathlib import Path
import sys
import unittest
import numpy as np
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
from stereo_reprojection import reproject_eye


def matrix(sign=1, jitter=(0, 0)):
    return [1, 0, 0, 0, 0, 1, 0, 0,
            jitter[0] * sign, jitter[1] * sign, -sign / 9, sign, 0, 0, 10 / 9, 0]


def device_depth(distance):
    return -1 / 9 + (10 / 9) / distance


class StereoReprojectionTests(unittest.TestCase):
    def test_parallel_shortcut_matches_full_matrix_inverse_with_principal_offsets(self):
        # Independent 4x4 inverse/multiply oracle; catches sign, half-pixel and
        # target-size errors without reusing the shortcut's depth formula.
        color = np.ones((4, 16, 3))
        for sign in (-1, 1):
            for reverse in (False, True):
                m = np.array([1.25, 0, 0, 0, 0, 1.5, 0, 0,
                              .125 * sign, -.25 * sign,
                              (-1 if reverse else 2) * sign, sign,
                              0, 0, 2 if reverse else -2, 0], dtype='float64')
                for size in ((16, 4), (8, 2), (32, 8)):
                    for eye_x in (-.25, 0, .25):
                        depth = np.full((4, 16), np.nan)
                        depth[1, 7] = .375
                        ndc = np.array([2 * 7.5 / 16 - 1, 1 - 2 * 1.5 / 4, .375, 1])
                        point = ndc @ np.linalg.inv(m.reshape(4, 4))
                        point /= point[3]
                        point[0] -= eye_x
                        projected = point @ m.reshape(4, 4)
                        projected /= projected[3]
                        tx = int(np.floor((projected[0] + 1) * size[0] / 2))
                        ty = int(np.floor((1 - projected[1]) * size[1] / 2))
                        result = reproject_eye(color, depth, m.tolist(), eye_x, size)
                        expected = np.full((size[1], size[0]), -1)
                        expected[ty, tx] = 23
                        np.testing.assert_array_equal(result['source_index'], expected)

    def test_zero_offset_upscale_is_independent_of_depth(self):
        expected = np.full((2, 16), -1)
        expected[1, 1::2] = np.arange(8)
        for sign in (-1, 1):
            for value in (0.2, 0.3):
                result = reproject_eye(np.ones((1, 8, 3)), np.full((1, 8), value), matrix(sign), 0, (16, 2))
                np.testing.assert_array_equal(result['source_index'], expected)

    def test_zero_offset_preserves_every_pixel_with_jitter_and_both_handedness(self):
        color = np.arange(6 * 8 * 3, dtype='float32').reshape(6, 8, 3) / 144
        depth = np.linspace(0.1, 0.9, 48).reshape(6, 8)
        for sign in (-1, 1):
            result = reproject_eye(color, depth, matrix(sign, (0.001, -0.002)), 0)
            np.testing.assert_array_equal(result['color'], color)
            np.testing.assert_array_equal(result['source_index'], np.arange(48).reshape(6, 8))
            self.assertTrue(result['valid'].all())

    def test_parallel_eyes_have_expected_parallax_and_near_points_move_more(self):
        color = np.ones((1, 8, 3))
        for sign in (-1, 1):
            disparities = []
            for distance in (2, 4):
                depth = np.full((1, 8), np.nan)
                depth[0, 3] = device_depth(distance)
                left = reproject_eye(color, depth, matrix(sign), -0.5)
                right = reproject_eye(color, depth, matrix(sign), 0.5)
                lx, rx = (int(np.flatnonzero(r['valid'])[0]) for r in (left, right))
                self.assertGreater(lx, rx)
                disparities.append(lx - rx)
            self.assertEqual(disparities, [2, 1])

    def test_z_buffer_keeps_foreground_when_two_points_hit_one_pixel(self):
        color = np.zeros((1, 5, 3))
        color[0, 1] = [1, 0, 0]
        color[0, 2] = [0, 1, 0]
        depth = np.full((1, 5), np.nan)
        depth[0, 1], depth[0, 2] = device_depth(4), device_depth(1)
        result = reproject_eye(color, depth, matrix(), 0.4)
        self.assertEqual(result['valid'].sum(), 1)
        np.testing.assert_array_equal(result['color'][0, 1], [0, 1, 0])
        self.assertAlmostEqual(result['distance'][0, 1], 1)
        self.assertEqual(result['source_index'][0, 1], 2)

    def test_invalid_depth_and_out_of_view_points_leave_explicit_holes(self):
        color = np.ones((1, 5, 3))
        depth = np.array([[np.nan, -0.1, 1.1, np.inf, device_depth(1)]])
        result = reproject_eye(color, depth, matrix(), -10)
        self.assertFalse(result['valid'].any())
        self.assertTrue(np.isinf(result['distance']).all())
        self.assertTrue((result['source_index'] == -1).all())
        self.assertFalse(result['color'].any())

    def test_rejects_bad_dimensions_offsets_and_color(self):
        color, depth = np.ones((2, 4, 3)), np.ones((2, 4))
        for offset in (float('inf'), float('nan')):
            with self.assertRaises(ValueError):
                reproject_eye(color, depth, matrix(), offset)
        for size in ((0, 2), (4, -1), (4.5, 2)):
            with self.assertRaises(ValueError):
                reproject_eye(color, depth, matrix(), 0, size)
        with self.assertRaises(ValueError):
            reproject_eye(color[:, :2], depth, matrix(), 0)
        color[0, 0, 0] = np.nan
        with self.assertRaises(ValueError):
            reproject_eye(color, depth, matrix(), 0)


if __name__ == '__main__':
    unittest.main()

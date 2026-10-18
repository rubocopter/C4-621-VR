import math
from pathlib import Path
import sys
import unittest
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
from projection_math import inspect_projection, view_z_from_depth, unproject_ndc, project_view, summarize_projection_samples


def perspective(near=1, far=500000, reverse=True, handedness=1, jitter=(0, 0)):
    y = 1 / math.tan(math.radians(89) / 2)
    a = -near / (far - near) if reverse else far / (far - near)
    b = near * far / (far - near) if reverse else -near * far / (far - near)
    return [y / (16 / 9), 0, 0, 0, 0, y, 0, 0,
            jitter[0] * handedness, jitter[1] * handedness, a * handedness, handedness,
            0, 0, b, 0]


class ProjectionMathTests(unittest.TestCase):
    def test_reverse_classification_does_not_compare_rounded_z_endpoints(self):
        for sign in (-1, 1):
            for reverse in (False, True):
                m = [1, 0, 0, 0, 0, 1, 0, 0, 0, 0,
                     (-1e20 if reverse else 1e20) * sign, sign,
                     0, 0, 1e20 if reverse else -1e20, 0]
                self.assertEqual(inspect_projection(m)['reverse_z'], reverse)

    def test_nonzero_small_denominator_is_a_finite_far_plane(self):
        for sign in (-1, 1):
            m = [1, 0, 0, 0, 0, 1, 0, 0, 0, 0, -1e-20 * sign, sign, 0, 0, 1, 0]
            self.assertTrue(math.isclose(view_z_from_depth(m, 0), sign * 1e20, rel_tol=1e-15))
            self.assertTrue(math.isclose(inspect_projection(m)['far_game_units'], 1e20, rel_tol=1e-15))
            point = unproject_ndc(m, .25, -.5, 0)
            self.assertTrue(all(math.isfinite(v) for v in point))
            for actual, expected in zip(project_view(m, point), (.25, -.5, 0)):
                self.assertAlmostEqual(actual, expected)

    def test_valid_projection_math_does_not_assume_a_minimum_world_unit(self):
        for sign in (-1, 1):
            for scale in (1e-20, 1e-30):
                m = [1, 0, 0, 0, 0, 1, 0, 0, 0, 0, -sign, sign, 0, 0, scale, 0]
                point = unproject_ndc(m, .25, -.5, 0)
                self.assertTrue(math.isclose(point[2], sign * scale, rel_tol=1e-15))
                for actual, expected in zip(project_view(m, point), (.25, -.5, 0)):
                    self.assertAlmostEqual(actual, expected)

    def test_rejects_small_unsupported_shear_and_noncanonical_w(self):
        for index, value in ((1, 1e-12), (11, 1 + 1e-7)):
            m = perspective()
            m[index] = value
            with self.assertRaises(ValueError):
                inspect_projection(m)

    def test_nonzero_depth_overflow_is_not_reported_as_an_infinite_far_pole(self):
        m = [1, 0, 0, 0, 0, 1, 0, 0, 0, 0, -1e-200, 1, 0, 0, 1e200, 0]
        with self.assertRaises(ValueError):
            view_z_from_depth(m, 0)

    def test_projection_and_unprojection_reject_nonfinite_results(self):
        m = [1e308, 0, 0, 0, 0, 1, 0, 0, 0, 0, -1, 1, 0, 0, 2, 0]
        with self.assertRaises(ValueError):
            project_view(m, (2, 0, 1))
        m[0] = 1e-308
        with self.assertRaises(ValueError):
            unproject_ndc(m, 1, 0, 0)

    def test_summary_keeps_jitter_separate_and_rejects_changing_depth_model(self):
        m = perspective()
        jittered = perspective(jitter=(0.5 / 2560, -1 / 1440))
        rows = [dict(matrix=x, width=2560, height=1440, near_plane=1, far_plane=500000,
                     jitter_enabled=enabled, jitter_x=0.25, jitter_y=0.5) for x, enabled in ((m, False), (jittered, True))]
        result = summarize_projection_samples(rows)
        self.assertEqual(result['sample_count'], 2)
        self.assertEqual(result['unique_depth_models'], 1)
        self.assertAlmostEqual(result['max_jitter_formula_residual_ndc'], 0)
        self.assertGreater(result['max_principal_point_offset_pixels'], 0)
        rows[1]['matrix'] = perspective(near=2)
        with self.assertRaises(ValueError):
            summarize_projection_samples(rows)
        with self.assertRaises(ValueError):
            summarize_projection_samples([])

    def test_reverse_and_forward_depth_endpoints(self):
        for reverse in (True, False):
            for sign in (-1, 1):
                matrix = perspective(reverse=reverse, handedness=sign)
                self.assertAlmostEqual(view_z_from_depth(matrix, 1 if reverse else 0), sign)
                self.assertAlmostEqual(view_z_from_depth(matrix, 0 if reverse else 1), sign * 500000, delta=0.001)
                result = inspect_projection(matrix)
                self.assertEqual(result['reverse_z'], reverse)
                self.assertAlmostEqual(result['near_game_units'], 1)
                self.assertAlmostEqual(result['far_game_units'], 500000, delta=0.001)
                self.assertAlmostEqual(result['fov_y_degrees'], 89)

    def test_roundtrip_with_jitter_and_both_handedness(self):
        for sign in (-1, 1):
            matrix = perspective(handedness=sign, jitter=(0.00013, -0.0002))
            for x, y, depth in [(-1, -1, 0.1), (0, 0, 0.9), (0.75, 0.2, 0.0005)]:
                point = unproject_ndc(matrix, x, y, depth)
                result = project_view(matrix, point)
                for a, b in zip(result, (x, y, depth)):
                    self.assertAlmostEqual(a, b, places=11)

    def test_depth_pole_is_explicit_not_a_fake_nearby_point(self):
        matrix = perspective()
        matrix[10] = 0
        self.assertTrue(math.isinf(view_z_from_depth(matrix, 0)))

    def test_rejects_unsupported_matrix_shape_and_depth(self):
        matrix = perspective()
        for bad in ([1] * 16, [0] * 15, [math.nan] * 16):
            with self.assertRaises(ValueError):
                inspect_projection(bad)
        for bad_depth in (-0.01, 1.01, math.nan):
            with self.assertRaises(ValueError):
                view_z_from_depth(matrix, bad_depth)


if __name__ == '__main__':
    unittest.main()

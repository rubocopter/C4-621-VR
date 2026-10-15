import math
from pathlib import Path
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
from camera_math import compose_head_orientation, relative_head_quaternion

IDENTITY = [1.,0.,0.,0., 0.,1.,0.,0., 0.,0.,1.,0., 4.,5.,6.,1.]


def yaw(degrees):
    half = math.radians(degrees) / 2
    return [0., math.sin(half), 0., math.cos(half)]


class CameraMathTests(unittest.TestCase):
    def assertMatrix(self, actual, expected):
        for a, b in zip(actual, expected):
            self.assertAlmostEqual(a, b, places=6)

    def test_yaw_rotates_basis_and_preserves_position(self):
        expected = [0.,0.,-1.,0., 0.,1.,0.,0., 1.,0.,0.,0., 4.,5.,6.,1.]
        self.assertMatrix(compose_head_orientation(IDENTITY, yaw(0), yaw(90)), expected)

    def test_recentering_cancels_absolute_head_rotation(self):
        self.assertMatrix(compose_head_orientation(IDENTITY, yaw(37), yaw(37)), IDENTITY)

    def test_delta_is_relative_to_reference_orientation(self):
        expected = [0.,0.,-1.,0., 0.,1.,0.,0., 1.,0.,0.,0., 4.,5.,6.,1.]
        self.assertMatrix(compose_head_orientation(IDENTITY, yaw(30), yaw(120)), expected)

    def test_head_rotation_is_applied_in_camera_local_axes(self):
        base = [0.,1.,0.,0., -1.,0.,0.,0., 0.,0.,1.,0., 4.,5.,6.,1.]
        expected = [0.,0.,-1.,0., -1.,0.,0.,0., 0.,1.,0.,0., 4.,5.,6.,1.]
        self.assertMatrix(compose_head_orientation(base, yaw(0), yaw(90)), expected)

    def test_optional_handedness_conversion_changes_yaw_sign(self):
        expected = [0.,0.,1.,0., 0.,1.,0.,0., -1.,0.,0.,0., 4.,5.,6.,1.]
        self.assertMatrix(compose_head_orientation(IDENTITY, yaw(0), yaw(90), flip_z=True), expected)

    def test_head_delta_is_recentered_before_publication(self):
        self.assertMatrix(relative_head_quaternion(yaw(37), yaw(37)), yaw(0))

    def test_head_delta_is_clamped_without_changing_rotation_axis(self):
        self.assertMatrix(relative_head_quaternion(yaw(30), yaw(120), max_degrees=25), yaw(25))

    def test_invalid_tracking_quaternion_is_rejected(self):
        with self.assertRaises(ValueError):
            relative_head_quaternion(yaw(0), [0,0,0,0])


if __name__ == "__main__":
    unittest.main()

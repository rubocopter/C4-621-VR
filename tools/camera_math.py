"""Experimental pose composition; row-vector game transform, xyzw XR quaternion.

The game handedness remains a calibration choice, not an established fact.
"""

import math


def relative_head_quaternion(reference, current, max_degrees=25):
    if not math.isfinite(max_degrees) or not 0 <= max_degrees <= 180:
        raise ValueError("Invalid angular limit")
    def normalized(q):
        if len(q) != 4 or not all(math.isfinite(v) for v in q):
            raise ValueError("Invalid quaternion")
        norm = math.sqrt(sum(v*v for v in q))
        if norm < 1e-10:
            raise ValueError("Zero quaternion")
        return [v/norm for v in q]
    ax, ay, az, aw = normalized(reference)
    bx, by, bz, bw = normalized(current)
    q = [aw*bx - ax*bw - ay*bz + az*by,
         aw*by + ax*bz - ay*bw - az*bx,
         aw*bz - ax*by + ay*bx - az*bw,
         aw*bw + ax*bx + ay*by + az*bz]
    if q[3] < 0:
        q = [-v for v in q]
    vector_norm = math.sqrt(sum(v*v for v in q[:3]))
    angle = 2*math.atan2(vector_norm, q[3])
    limit = math.radians(max_degrees)
    if angle > limit and vector_norm > 1e-10:
        scale = math.sin(limit/2) / vector_norm
        q = [v*scale for v in q[:3]] + [math.cos(limit/2)]
    return q


def compose_head_orientation(game, reference, current, *, flip_z=False):
    if len(game) != 16 or not all(math.isfinite(x) for x in game):
        raise ValueError("Expected a finite 4x4 game transform")

    def rotation(quaternion):
        if len(quaternion) != 4 or not all(math.isfinite(x) for x in quaternion):
            raise ValueError("Expected finite xyzw quaternion")
        norm = math.sqrt(sum(x * x for x in quaternion))
        if norm < 1e-10:
            raise ValueError("Zero quaternion cannot describe an orientation")
        x, y, z, w = [v / norm for v in quaternion]
        return [[1 - 2*(y*y + z*z), 2*(x*y - z*w), 2*(x*z + y*w)],
                [2*(x*y + z*w), 1 - 2*(x*x + z*z), 2*(y*z - x*w)],
                [2*(x*z - y*w), 2*(y*z + x*w), 1 - 2*(x*x + y*y)]]

    initial, now = rotation(reference), rotation(current)
    # Column-vector XR relative orientation, then row-vector game convention.
    relative = [[sum(initial[k][i] * now[k][j] for k in range(3))
                 for j in range(3)] for i in range(3)]
    signs = [1, 1, -1 if flip_z else 1]
    delta = [[relative[j][i] * signs[i] * signs[j] for j in range(3)] for i in range(3)]
    result = list(game)
    for i in range(3):
        for j in range(3):
            result[4*i + j] = sum(delta[i][k] * game[4*k + j] for k in range(3))
    return result

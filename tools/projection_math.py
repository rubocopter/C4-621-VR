"""Row-vector perspective math in game units; no assumption that units are metres."""
import math
from collections import Counter


def _validate(matrix):
    if len(matrix) != 16 or not all(math.isfinite(v) for v in matrix):
        raise ValueError('Expected 16 finite row-vector matrix coefficients')
    if any(matrix[i] != 0 for i in (1, 2, 3, 4, 6, 7, 12, 13, 15)):
        raise ValueError('Unsupported perspective shape; orthographic/sheared matrices need a general inverse')
    if matrix[0] <= 0 or matrix[5] <= 0 or matrix[11] not in (-1, 1) or matrix[14] == 0:
        raise ValueError('Unsupported perspective scale, W sign or depth coefficient')


def inspect_projection(matrix):
    _validate(matrix)
    z0, z1 = (view_z_from_depth(matrix, d) for d in (0, 1))
    near, far = min(abs(z0), abs(z1)), max(abs(z0), abs(z1))
    if not near > 0 or (math.isfinite(z0) and z0 * matrix[11] <= 0) or (math.isfinite(z1) and z1 * matrix[11] <= 0):
        raise ValueError('Depth range is not in the forward half-space')
    # d(forwardDistance)/d(deviceDepth) = -Wsign**2 * B / denominator**2.
    # Endpoint distances may round equal; their comparison can lose the sign.
    return dict(reverse_z=matrix[14] > 0, near_game_units=near, far_game_units=far,
                fov_y_degrees=math.degrees(2 * math.atan(1 / matrix[5])),
                fov_x_degrees=math.degrees(2 * math.atan(1 / matrix[0])),
                aspect=matrix[5] / matrix[0], forward_z_sign=matrix[11],
                ndc_offset_x=matrix[8] / matrix[11], ndc_offset_y=matrix[9] / matrix[11],
                depth_a=matrix[10], depth_b=matrix[14],
                units='game units; metre scale is not established')


def view_z_from_depth(matrix, depth):
    _validate(matrix)
    if not math.isfinite(depth) or not 0 <= depth <= 1:
        raise ValueError('Device depth must be finite and within [0, 1]')
    denominator = matrix[11] * depth - matrix[10]
    if denominator == 0:
        return math.copysign(math.inf, matrix[11])
    z = matrix[14] / denominator
    if not math.isfinite(z) or z == 0:
        raise ValueError('Finite-depth reconstruction is outside the representable range')
    return z


def unproject_ndc(matrix, x, y, depth):
    if not math.isfinite(x) or not math.isfinite(y):
        raise ValueError('NDC coordinates must be finite')
    z = view_z_from_depth(matrix, depth)
    if not math.isfinite(z):
        raise ValueError('A depth pole represents a direction, not a finite view-space point')
    w = matrix[11] * z
    point = ((x * w - matrix[8] * z) / matrix[0], (y * w - matrix[9] * z) / matrix[5], z)
    if not all(math.isfinite(value) for value in point):
        raise ValueError('Unprojected point is outside the finite representable range')
    return point


def project_view(matrix, point):
    _validate(matrix)
    if len(point) != 3 or not all(math.isfinite(v) for v in point):
        raise ValueError('Expected a finite 3D point')
    x, y, z = point
    w = matrix[11] * z
    if w == 0:
        raise ValueError('Point lies at the perspective divide')
    projected = ((matrix[0] * x + matrix[8] * z) / w,
                 (matrix[5] * y + matrix[9] * z) / w,
                 (matrix[10] * z + matrix[14]) / w)
    if not all(math.isfinite(value) for value in projected):
        raise ValueError('Projected point is outside the finite representable range')
    return projected


def summarize_projection_samples(samples):
    if not samples:
        raise ValueError('No observed perspective samples')
    for sample in samples:
        inspect_projection(sample['matrix'])
    models = Counter((s['matrix'][0], s['matrix'][5], s['matrix'][10], s['matrix'][11], s['matrix'][14]) for s in samples)
    if len(models) != 1:
        raise ValueError('Perspective scale/depth coefficients changed; select one view or capture interval')
    dimensions = {(s['width'], s['height']) for s in samples}
    if len(dimensions) != 1:
        raise ValueError('Mixed dimensions cannot share one calibration')
    width, height = next(iter(dimensions))
    if width <= 0 or height <= 0:
        raise ValueError('Positive viewport dimensions are required')
    matrices = Counter(tuple(s['matrix']) for s in samples)
    representative = list(matrices.most_common(1)[0][0])
    residuals = []
    for sample in samples:
        expected_x = 2 * sample['jitter_x'] / width if sample['jitter_enabled'] else 0
        expected_y = -2 * sample['jitter_y'] / height if sample['jitter_enabled'] else 0
        residuals.extend((abs(sample['matrix'][8] - expected_x), abs(sample['matrix'][9] - expected_y)))
    return dict(sample_count=len(samples), width=width, height=height, unique_depth_models=len(models),
                unique_full_matrices=len(matrices), representative_matrix=representative,
                projection=inspect_projection(representative),
                configured_near_planes=sorted({s['near_plane'] for s in samples}),
                configured_far_planes=sorted({s['far_plane'] for s in samples}),
                max_principal_point_offset_pixels=max(max(abs(s['matrix'][8] / s['matrix'][11]) * width / 2,
                    abs(s['matrix'][9] / s['matrix'][11]) * height / 2) for s in samples),
                max_jitter_formula_residual_ndc=max(residuals),
                temporal_association='Projection observations and depth captures are separate; per-frame jitter is not associated')

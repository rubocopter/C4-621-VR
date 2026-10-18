"""Offline NumPy reference for parallel-eye point reprojection in game units."""
import math
import numpy as np
from projection_math import inspect_projection


def reproject_eye(color, depth, matrix, eye_x, output_size=None, *, target_matrix=None):
    """Translate a parallel camera along view X, preserving holes and nearest Z.

    Color is HxWx3, device depth is HxW in [0,1]. Output size is (width,height).
    Target defaults to the source projection; an explicit target changes only
    image intrinsics (focal scales/principal point), not near/far clipping.
    Both matrices must share view axes/W sign. No IPD/metre calibration, splat
    enlargement, temporal accumulation, hidden-surface recovery or HUD removal.
    Source indices make coverage/occlusion choices inspectable; -1 means a hole.
    """
    inspect_projection(matrix)
    target_matrix = matrix if target_matrix is None else target_matrix
    inspect_projection(target_matrix)
    if target_matrix[11] != matrix[11]:
        raise ValueError('Parallel source/target projections must share view axes/W sign')
    scale_x, scale_y = target_matrix[0] / matrix[0], target_matrix[5] / matrix[5]
    offset_x = (target_matrix[8] - scale_x * matrix[8]) / matrix[11]
    offset_y = (target_matrix[9] - scale_y * matrix[9]) / matrix[11]
    if not all(math.isfinite(v) for v in (scale_x, scale_y, offset_x, offset_y)) or min(scale_x, scale_y) <= 0:
        raise ValueError('Image intrinsics transform is not representable')
    color, depth = np.asarray(color), np.asarray(depth)
    if depth.ndim != 2 or min(depth.shape) <= 0 or color.shape != (*depth.shape, 3):
        raise ValueError('Expected nonempty HxW depth and matching HxWx3 color')
    if not np.isfinite(color).all() or not math.isfinite(eye_x):
        raise ValueError('Color and eye offset must be finite')
    height, width = depth.shape
    output_size = (width, height) if output_size is None else output_size
    if len(output_size) != 2 or any(not isinstance(v, (int, np.integer)) or v <= 0 for v in output_size):
        raise ValueError('Output size must be two positive integers')
    out_width, out_height = output_size
    result = dict(color=np.zeros((out_height, out_width, 3), dtype=color.dtype),
                  valid=np.zeros((out_height, out_width), dtype=bool),
                  distance=np.full((out_height, out_width), np.inf),
                  source_index=np.full((out_height, out_width), -1, dtype=np.int64))
    valid = np.isfinite(depth) & (depth >= 0) & (depth <= 1)
    sy, sx = np.nonzero(valid)
    device_depth = depth[sy, sx].astype('float64')
    denominator = matrix[11] * device_depth - matrix[10]
    with np.errstate(divide='ignore', invalid='ignore'):
        z = matrix[14] / denominator
    forward = np.isfinite(z) & (z * matrix[11] > 0)
    sy, sx, z = sy[forward], sx[forward], z[forward]
    device_depth = device_depth[forward]
    w = matrix[11] * z
    # Unproject/project analytically cancel for identical parallel projections.
    # Avoid multiplying/dividing Z at integer pixel boundaries during resizing.
    with np.errstate(over='ignore', invalid='ignore', divide='ignore'):
        px = (sx + 0.5) * out_width / width - matrix[0] * eye_x * out_width / (2 * w)
        py = (sy + 0.5) * out_height / height
        # Convert the source-projected eye point to destination intrinsics.
        # Keep the identity branch untouched, including at pixel boundaries.
        if (scale_x, scale_y, offset_x, offset_y) != (1, 1, 0, 0):
            px = (px - out_width / 2) * scale_x + (1 + offset_x) * out_width / 2
            py = (py - out_height / 2) * scale_y + (1 - offset_y) * out_height / 2
    inside = np.isfinite(px) & np.isfinite(py) & (px >= 0) & (px < out_width) & (py >= 0) & (py < out_height)
    target = np.floor(py[inside]).astype('int64') * out_width + np.floor(px[inside]).astype('int64')
    source = sy[inside] * width + sx[inside]
    distance = w[inside]
    # Original depth preserves distinctions that reconstructed float64 distance
    # can lose. Its monotonic order is controlled by B for either W sign.
    depth_order = -device_depth[inside] if matrix[14] > 0 else device_depth[inside]
    order = np.lexsort((source, depth_order, target))
    ordered_target = target[order]
    first = np.r_[True, ordered_target[1:] != ordered_target[:-1]] if len(order) else np.empty(0, dtype=bool)
    winners = order[first]
    destination, selected = target[winners], source[winners]
    result['color'].reshape(-1, 3)[destination] = color.reshape(-1, 3)[selected]
    result['valid'].ravel()[destination] = True
    result['distance'].ravel()[destination] = distance[winners]
    result['source_index'].ravel()[destination] = selected
    return result

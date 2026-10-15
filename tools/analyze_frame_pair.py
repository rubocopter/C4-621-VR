"""Inspect a private paired D3D12 snapshot. Requires numpy and Pillow.

Decoded color is a diagnostic preview, without HDR/color-space calibration.
Depth contours aid visual inspection, not proof of projection or metric scale.
"""
import argparse
import hashlib
import json
from pathlib import Path


def read_color(meta, raw):
    import numpy as np
    packed = _plane(meta, raw, 'color', '<u4')
    fmt = meta['color_format']
    if fmt == 24:  # R10G10B10A2_UNORM
        return np.stack([(packed >> shift) & 1023 for shift in (0, 10, 20)], axis=-1).astype('float32') / 1023
    if fmt in (28, 87):  # R8G8B8A8 / B8G8R8A8 UNORM
        shifts = (0, 8, 16) if fmt == 28 else (16, 8, 0)
        return np.stack([(packed >> shift) & 255 for shift in shifts], axis=-1).astype('float32') / 255
    raise ValueError(f'Unsupported color format: {fmt}')


def read_depth(meta, raw):
    if meta['depth_footprint_format'] not in (39, 40):
        raise ValueError('Expected a 32-bit float-compatible depth plane')
    return _plane(meta, raw, 'depth', '<f4').copy()


def _plane(meta, raw, name, dtype):
    import numpy as np
    width, height = meta['width'], meta['height']
    pitch, offset = meta[f'{name}_pitch'], meta[f'{name}_offset']
    if min(width, height) <= 0 or pitch % 4 or pitch < width * 4 or offset < 0 or offset % 4:
        raise ValueError('Invalid footprint dimensions, pitch or offset')
    # GetCopyableFootprints need not include padding after the last row.
    if len(raw) != meta[f'{name}_bytes'] or len(raw) < offset + (height - 1) * pitch + width * 4:
        raise ValueError('Truncated or unexpected snapshot size')
    return np.ndarray((height, width), dtype=dtype, buffer=raw, offset=offset, strides=(pitch, 4))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('report', 'color', 'depth', 'output-prefix'):
        parser.add_argument(f'--{name}', type=Path, required=True)
    args = parser.parse_args()
    import numpy as np
    from PIL import Image, ImageDraw
    report_text = args.report.read_text(encoding='utf-8')
    if args.report.suffix == '.jsonl':
        events = [json.loads(line) for line in report_text.splitlines()]
    else:
        events = json.loads(report_text)['events']
    meta = next(e for e in events if e.get('phase') == 'frame_pair')
    if not meta['written'] or not meta['copies_in_one_command_list'] or not meta.get('recording_retired') or meta['missed_observations'] or meta['ledger_unsafe']:
        raise ValueError('Pair was not captured with a complete state trace')
    outputs = {name: Path(str(args.output_prefix) + suffix) for name, suffix in (
        ('color', '-color.png'), ('depth', '-depth.png'), ('overlay', '-overlay.png'),
        ('montage', '-montage.jpg'), ('raw_depth', '-depth.npy'), ('summary', '-summary.json'))}
    if any(p.exists() for p in outputs.values()):
        raise ValueError('Output exists; choose a new prefix')
    color_raw, depth_raw = args.color.read_bytes(), args.depth.read_bytes()
    color, depth = read_color(meta, color_raw), read_depth(meta, depth_raw)
    finite = np.isfinite(depth)
    if not finite.any():
        raise ValueError('Depth contains no finite values')
    percentiles = np.percentile(depth[finite], [0, 1, 50, 99, 100])
    lo, hi = percentiles[1], percentiles[-2]
    gray = np.clip((np.nan_to_num(depth) - lo) / max(float(hi - lo), 1e-12), 0, 1) ** 0.35
    gy, gx = np.gradient(gray)
    magnitude = np.hypot(gx, gy)
    contours = (magnitude > max(0.015, float(np.percentile(magnitude, 95)))) & finite
    overlay = color.copy()
    overlay[contours] = [1, 0.15, 0.05]
    pictures = {'color': Image.fromarray(np.rint(color * 255).astype('uint8')),
                'depth': Image.fromarray(np.rint(gray * 255).astype('uint8')).convert('RGB'),
                'overlay': Image.fromarray(np.rint(overlay * 255).astype('uint8'))}
    args.output_prefix.parent.mkdir(parents=True, exist_ok=True)
    montage = Image.new('RGB', (1920, 384), '#15191e')
    draw = ImageDraw.Draw(montage)
    for i, (name, picture) in enumerate(pictures.items()):
        picture.save(outputs[name])
        thumb = picture.copy()
        thumb.thumbnail((640, 360))
        montage.paste(thumb, (i * 640, 24))
        draw.text((i * 640 + 8, 5), name, fill='white')
    montage.save(outputs['montage'], quality=92)
    np.save(outputs['raw_depth'], depth)
    summary = {'metadata': meta, 'color_sha256': hashlib.sha256(color_raw).hexdigest(),
               'depth_sha256': hashlib.sha256(depth_raw).hexdigest(),
               'finite_depth_fraction': float(finite.mean()),
               'depth_range_0_1_fraction': float(((depth >= 0) & (depth <= 1)).mean()),
               'depth_percentiles_0_1_50_99_100': percentiles.tolist(),
               'limitations': 'Paired GPU snapshot at the reported capture point; final presentation, projection, metric scale, colorimetry and exact geometric alignment not calibrated.'}
    outputs['summary'].write_text(json.dumps(summary, indent=2), encoding='utf-8')
    print(json.dumps(summary, indent=2))


if __name__ == '__main__':
    main()

"""Make inspectable offline stereo views from a private paired AC6 capture.

Requires NumPy/Pillow. Baseline is explicitly in game units, not metres/IPD.
This diagnostic uses the observed projection model but does not establish its
exact per-frame jitter association. It cannot recover hidden surfaces or HUD.
"""
import argparse
import hashlib
import json
import math
from pathlib import Path
import struct
import subprocess
import time
import numpy as np
from PIL import Image, ImageDraw
from analyze_frame_pair import read_color, read_depth
from analyze_projection import events
from projection_math import summarize_projection_samples
from stereo_reprojection import reproject_eye


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('projection-report', 'pair-report', 'color', 'depth', 'output-prefix'):
        parser.add_argument('--' + name, type=Path, required=True)
    parser.add_argument('--baseline-game-units', type=float, required=True)
    parser.add_argument('--width', type=int, default=1280)
    parser.add_argument('--height', type=int, default=720)
    parser.add_argument('--gpu-exe', type=Path, help='Standalone D3D12 diagnostic; also compare against CPU indices')
    args = parser.parse_args()
    if not math.isfinite(args.baseline_game_units) or args.baseline_game_units < 0 or min(args.width, args.height) <= 0:
        parser.error('Baseline must be finite/nonnegative; dimensions must be positive')
    outputs = {name: Path(str(args.output_prefix) + suffix) for name, suffix in (
        ('left', '-left.png'), ('right', '-right.png'), ('left_mask', '-left-valid.png'),
        ('right_mask', '-right-valid.png'), ('comparison', '-comparison.jpg'), ('summary', '-summary.json'))}
    if any(p.exists() for p in outputs.values()):
        raise ValueError('Choose a new output prefix')
    if args.gpu_exe and not args.gpu_exe.is_file():
        raise ValueError('GPU diagnostic executable does not exist')
    rows = events(args.projection_report)
    if not any(r.get('phase') == 'disabled' and r.get('hooks_restored') for r in rows):
        raise ValueError('Projection probe restoration is not confirmed')
    calibration = summarize_projection_samples([r for r in rows if r.get('phase') == 'projection_sample'])
    meta = next(r for r in events(args.pair_report) if r.get('phase') == 'frame_pair')
    if not meta.get('written') or not meta.get('copies_in_one_command_list') or not meta.get('recording_retired') or meta.get('missed_observations') or meta.get('ledger_unsafe'):
        raise ValueError('Pair has no complete, retired GPU snapshot')
    if (meta['width'], meta['height']) != (calibration['width'], calibration['height']):
        raise ValueError('Projection dimensions do not match the capture')
    # Keep the source aspect: resizing to a different ratio would stretch rays.
    if abs(args.width / args.height - meta['width'] / meta['height']) > 1e-6:
        raise ValueError('Output must preserve the observed projection aspect')
    color_raw, depth_raw = args.color.read_bytes(), args.depth.read_bytes()
    color, depth = read_color(meta, color_raw), read_depth(meta, depth_raw)
    matrix = calibration['representative_matrix']
    args.output_prefix.parent.mkdir(parents=True, exist_ok=True)
    summary = dict(baseline_game_units=args.baseline_game_units, metric_scale_calibrated=False,
        source_size=[meta['width'], meta['height']], output_size=[args.width, args.height],
        matrix=matrix, per_frame_jitter_associated=False,
        color_sha256=hashlib.sha256(color_raw).hexdigest(), depth_sha256=hashlib.sha256(depth_raw).hexdigest(),
        projection_report_sha256=hashlib.sha256(args.projection_report.read_bytes()).hexdigest(),
        limitations='Offline point reprojection; holes preserved, HUD projected as scene color, no unseen geometry or VR presentation; colorimetry uncalibrated.')
    if args.gpu_exe:
        summary['gpu_executable_sha256'] = hashlib.sha256(args.gpu_exe.read_bytes()).hexdigest()
    pictures, coverage = [], []
    for eye, sign in (('left', -1), ('right', 1)):
        start = time.perf_counter()
        result = reproject_eye(color, depth, matrix, sign * args.baseline_game_units / 2, (args.width, args.height))
        seconds = time.perf_counter() - start
        gpu_summary = None
        if args.gpu_exe:
            native_prefix = Path(str(args.output_prefix) + f'-{eye}-gpu')
            input_path = Path(str(native_prefix) + '-input.bin')
            native_rgba = Path(str(native_prefix) + '.rgba.raw')
            native_indices = Path(str(native_prefix) + '.indices.raw')
            if any(p.exists() for p in (input_path, native_rgba, native_indices)):
                raise ValueError('Choose a new GPU output prefix')
            rgba = np.concatenate((np.rint(np.clip(color, 0, 1) * 255).astype('uint8'),
                                   np.full((*depth.shape, 1), 255, dtype='uint8')), axis=-1)
            params = struct.pack('<4I6f2I', meta['width'], meta['height'], args.width, args.height,
                                 matrix[0], matrix[5], matrix[10], matrix[14], matrix[11],
                                 sign * args.baseline_game_units / 2, 0, 0)
            with input_path.open('xb') as stream:
                stream.write(b'AC6RPRJ1' + params)
                stream.write(depth.astype('<f4').tobytes())
                stream.write(rgba.tobytes())
            completed = subprocess.run([str(args.gpu_exe.resolve()), str(input_path.resolve()), str(native_prefix.resolve())],
                                       capture_output=True, text=True, check=True, timeout=40,
                                       creationflags=getattr(subprocess, 'CREATE_NO_WINDOW', 0))
            gpu_summary = json.loads(completed.stdout)
            gpu_ids = np.frombuffer(native_indices.read_bytes(), dtype='<u4').reshape(args.height, args.width)
            gpu_valid = gpu_ids != 0xffffffff
            if np.any(gpu_ids[gpu_valid] >= depth.size):
                raise ValueError('GPU returned an out-of-range source index')
            gpu_pixels = np.frombuffer(native_rgba.read_bytes(), dtype='uint8').reshape(args.height, args.width, 4)
            gpu_ids_signed = np.where(gpu_valid, gpu_ids.astype('int64'), -1)
            gpu_summary['cpu_index_mismatch_pixels'] = int((gpu_ids_signed != result['source_index']).sum())
            gpu_summary['cpu_coverage_mismatch_pixels'] = int((gpu_valid != result['valid']).sum())
            mismatched = (gpu_ids_signed != result['source_index']) & gpu_valid & result['valid']
            if mismatched.any():
                margins = []
                for ids in (gpu_ids_signed[mismatched], result['source_index'][mismatched]):
                    distances = matrix[11] * matrix[14] / (matrix[11] * depth.ravel()[ids].astype('float64') - matrix[10])
                    px = (ids % meta['width'] + 0.5) * args.width / meta['width'] - matrix[0] * sign * args.baseline_game_units * args.width / (4 * distances)
                    margins.append(np.abs(px - np.rint(px)))
                gpu_summary['max_mismatch_nearest_candidate_boundary_pixels'] = float(np.minimum(*margins).max())
            expected_rgba = np.zeros_like(gpu_pixels)
            expected_rgba[gpu_valid] = rgba.reshape(-1, 4)[gpu_ids[gpu_valid]]
            if not np.array_equal(gpu_pixels, expected_rgba):
                raise ValueError('GPU color does not match its selected source index')
            gpu_summary['selected_source_color_exact'] = True
            gpu_summary['cpu_reference_precision'] = 'float64; GPU float32 raster positions can cross integer pixel boundaries'
            result = dict(color=gpu_pixels[:, :, :3] / 255, valid=gpu_valid)
        pixels = np.rint(np.clip(result['color'], 0, 1) * 255).astype('uint8')
        picture = Image.fromarray(pixels)
        picture.save(outputs[eye])
        Image.fromarray(result['valid'].astype('uint8') * 255).save(outputs[eye + '_mask'])
        summary[eye] = dict(eye_x_game_units=sign * args.baseline_game_units / 2,
            covered_fraction=float(result['valid'].mean()), uncovered_pixels=int((~result['valid']).sum()),
            cpu_seconds=seconds, image_sha256=hashlib.sha256(outputs[eye].read_bytes()).hexdigest())
        if gpu_summary is not None:
            summary[eye]['gpu'] = gpu_summary
        pictures.append(picture)
        coverage.append(result['valid'])
    summary['eyes_identical'] = pictures[0].tobytes() == pictures[1].tobytes()
    summary['both_eyes_covered_fraction'] = float((coverage[0] & coverage[1]).mean())
    comparison = Image.new('RGB', (args.width * 2, args.height + 52), '#15191e')
    draw = ImageDraw.Draw(comparison)
    for i, (eye, picture) in enumerate(zip(('left', 'right'), pictures)):
        comparison.paste(picture, (i * args.width, 52))
        draw.text((i * args.width + 12, 8), f'{eye.upper()} — offline depth reprojection', fill='white')
        draw.text((i * args.width + 12, 28), f'Baseline {args.baseline_game_units:g} game units (not calibrated metres); black holes retained', fill='#c5cbd2')
    comparison.save(outputs['comparison'], quality=94)
    outputs['summary'].write_text(json.dumps(summary, indent=2), encoding='utf-8')
    print(json.dumps(summary, indent=2))


if __name__ == '__main__':
    main()

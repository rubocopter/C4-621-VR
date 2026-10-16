"""Inspect observed projection and optionally reconstruct a private color/depth pair.

Reconstruction uses game units. It does not establish metres, exact per-frame
jitter, shader constant-buffer provenance or a binocular rendering pipeline.
Requires NumPy/Pillow; the optional point-cloud plot also needs Matplotlib.
"""
import argparse
import hashlib
import json
from pathlib import Path
from projection_math import summarize_projection_samples, project_view, unproject_ndc


def events(path):
    text = path.read_text(encoding='utf-8')
    return [json.loads(line) for line in text.splitlines()] if path.suffix == '.jsonl' else json.loads(text)['events']


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--projection-report', type=Path, required=True)
    parser.add_argument('--pair-report', type=Path)
    parser.add_argument('--depth', type=Path)
    parser.add_argument('--color', type=Path)
    parser.add_argument('--output-prefix', type=Path, required=True)
    parser.add_argument('--plot', action='store_true', help='Save a Matplotlib view of the reconstructed cloud')
    args = parser.parse_args()
    if any((args.pair_report, args.depth, args.color)) and not all((args.pair_report, args.depth, args.color)):
        parser.error('Pair report, color and depth must be supplied together')
    outputs = {key: Path(str(args.output_prefix) + suffix) for key, suffix in (
        ('summary', '-summary.json'), ('distance', '-view-z.npy'), ('cloud', '-cloud.ply'), ('plot', '-cloud.png'))}
    if any(path.exists() for path in outputs.values()):
        raise ValueError('Choose a new output prefix')
    rows = events(args.projection_report)
    if not any(r.get('phase') == 'disabled' and r.get('hooks_restored') for r in rows):
        raise ValueError('Projection probe has no confirmed restoration')
    samples = [r for r in rows if r.get('phase') == 'projection_sample']
    summary = summarize_projection_samples(samples)
    summary['projection_report_sha256'] = hashlib.sha256(args.projection_report.read_bytes()).hexdigest()
    # This is an algebraic oracle, not independent validation of game geometry.
    matrix = summary['representative_matrix']
    roundtrip_errors = []
    for x in (-0.9, 0, 0.9):
        for y in (-0.8, 0, 0.8):
            for depth in (0.001, 0.02, 0.5, 0.99):
                restored = project_view(matrix, unproject_ndc(matrix, x, y, depth))
                roundtrip_errors.append(max(abs(a - b) for a, b in zip(restored, (x, y, depth))))
    summary['algebraic_roundtrip_max_error'] = max(roundtrip_errors)
    args.output_prefix.parent.mkdir(parents=True, exist_ok=True)
    if args.pair_report:
        import numpy as np
        from analyze_frame_pair import read_color, read_depth
        meta = next(r for r in events(args.pair_report) if r.get('phase') == 'frame_pair')
        if not meta['written'] or not meta.get('copies_in_one_command_list') or not meta.get('recording_retired') or meta.get('missed_observations') or meta.get('ledger_unsafe'):
            raise ValueError('Pair has no complete, retired GPU snapshot')
        if meta['width'] != summary['width'] or meta['height'] != summary['height']:
            raise ValueError('Pair dimensions do not match the projection observation')
        color_raw, depth_raw = args.color.read_bytes(), args.depth.read_bytes()
        depth, color = read_depth(meta, depth_raw), read_color(meta, color_raw)
        valid = np.isfinite(depth) & (depth >= 0) & (depth <= 1)
        denominator = matrix[11] * depth.astype('float64') - matrix[10]
        with np.errstate(divide='ignore', invalid='ignore'):
            view_z = matrix[14] / denominator
        valid &= np.isfinite(view_z) & (view_z * matrix[11] > 0)
        if not valid.any():
            raise ValueError('No depth values reconstruct in the forward half-space')
        view_z[~valid] = np.nan
        np.save(outputs['distance'], view_z)
        # Subsample pixel centres; no interpolation or invented hole filling.
        yy, xx = np.mgrid[0:depth.shape[0]:8, 0:depth.shape[1]:8]
        z = view_z[yy, xx]
        mask = valid[yy, xx]
        nx, ny = 2 * (xx + 0.5) / depth.shape[1] - 1, 1 - 2 * (yy + 0.5) / depth.shape[0]
        x = (nx * matrix[11] - matrix[8]) * z / matrix[0]
        y = (ny * matrix[11] - matrix[9]) * z / matrix[5]
        points = np.stack((x, y, z), axis=-1)[mask]
        rgb = np.rint(color[yy, xx][mask] * 255).astype('uint8')
        with outputs['cloud'].open('x', encoding='ascii') as stream:
            stream.write('ply\nformat ascii 1.0\ncomment Units are uncalibrated game units\n')
            stream.write(f'element vertex {len(points)}\nproperty float x\nproperty float y\nproperty float z\n')
            stream.write('property uchar red\nproperty uchar green\nproperty uchar blue\nend_header\n')
            for p, c in zip(points, rgb):
                stream.write(' '.join([*(f'{float(v):.8g}' for v in p), *(str(int(v)) for v in c)]) + '\n')
        summary['reconstruction'] = dict(valid_fraction=float(valid.mean()), cloud_points=len(points),
            view_z_percentiles_0_1_50_99_100=np.percentile(np.abs(view_z[valid]), [0, 1, 50, 99, 100]).tolist(),
            units='game units, not verified metres', depth_sha256=hashlib.sha256(depth_raw).hexdigest(),
            color_sha256=hashlib.sha256(color_raw).hexdigest(), per_frame_jitter_associated=False,
            point_cloud_includes_hud_color=True)
        if args.plot:
            import matplotlib
            matplotlib.use('Agg')
            import matplotlib.pyplot as plt
            fig = plt.figure(figsize=(14, 6), facecolor='#15191e')
            display_colors = (rgb / 255) ** 0.55  # visualisation only; stored RGB stays unchanged
            for i in (1, 2):
                if i == 2:
                    ax = fig.add_subplot(1, 2, i, facecolor='#15191e')
                    ax.scatter(points[:, 0], points[:, 2], c=display_colors, s=0.8, rasterized=True)
                    ax.set(xlabel='X (game units)', ylabel='Z forward (game units)', title='Top view')
                    ax.set_aspect('equal', adjustable='datalim')
                    ax.grid(color='#3a424c', alpha=0.45)
                    ax.tick_params(colors='#c5cbd2', labelsize=8)
                    for axis in (ax.xaxis, ax.yaxis):
                        axis.label.set_color('#c5cbd2')
                    ax.title.set_color('#c5cbd2')
                    continue
                ax = fig.add_subplot(1, 2, i, projection='3d', facecolor='#15191e')
                ax.scatter(points[:, 0], points[:, 2], points[:, 1], c=display_colors, s=0.8, depthshade=False, rasterized=True)
                ax.set(xlabel='X (game units)', ylabel='Z forward (game units)', zlabel='Y up (game units)')
                spans = np.ptp(points, axis=0)
                ax.set_box_aspect(np.maximum(spans[[0, 2, 1]], 1))
                ax.view_init(elev=20, azim=-70)
                ax.tick_params(colors='#c5cbd2', labelsize=7)
                for axis in (ax.xaxis, ax.yaxis, ax.zaxis):
                    axis.label.set_color('#c5cbd2')
                    axis.set_pane_color((0.08, 0.1, 0.12, 1))
            fig.suptitle('AC6 depth reconstruction — game units; metre scale and frame jitter uncalibrated', color='white')
            fig.tight_layout()
            fig.savefig(outputs['plot'], dpi=160, facecolor=fig.get_facecolor())
            plt.close(fig)
    outputs['summary'].write_text(json.dumps(summary, indent=2), encoding='utf-8')
    print(json.dumps(summary, indent=2))


if __name__ == '__main__':
    main()

"""Visualize one private D3D12 plane-0 snapshot; not metric depth calibration.

Dependencies: numpy and Pillow (install in a local analysis virtual environment).
"""
import argparse
import hashlib
import json
from pathlib import Path


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--report", type=Path, required=True)
    parser.add_argument("--raw", type=Path, required=True)
    parser.add_argument("--output-prefix", type=Path, required=True)
    args = parser.parse_args()
    import numpy as np
    from PIL import Image

    source = json.loads(args.report.read_text(encoding="utf-8"))
    meta = next(r for r in source["events"] if r["phase"] == "depth_capture")
    if not meta["written"] or meta["footprint_format"] not in (39, 40):
        raise RuntimeError("Expected a written plane with 32-bit float-compatible footprint")
    width, height, pitch = meta["width"], meta["height"], meta["row_pitch"]
    if min(width, height) <= 0 or pitch % 4 or pitch < width * 4:
        raise RuntimeError("Invalid depth dimensions or pitch")
    paths = [args.output_prefix.with_suffix(s) for s in (".png", ".npy", ".json")]
    if any(p.exists() for p in paths):
        raise RuntimeError("Output already exists; choose a new prefix")
    data = args.raw.read_bytes()
    if len(data) != meta["total_bytes"] or len(data) < meta["offset"] + pitch * height:
        raise RuntimeError("Unexpected raw snapshot size")
    depth = np.frombuffer(data, dtype="<f4", offset=meta["offset"], count=height * pitch // 4).reshape(height, pitch // 4)[:, :width].copy()
    finite = depth[np.isfinite(depth)]
    if finite.size == 0:
        raise RuntimeError("No finite depth values")
    percentiles = np.percentile(finite, [0, 1, 5, 25, 50, 75, 95, 99, 100])
    lo, hi = percentiles[1], percentiles[-2]
    gray = np.clip((np.nan_to_num(depth) - lo) / max(float(hi - lo), 1e-12), 0, 1) ** 0.35
    args.output_prefix.parent.mkdir(parents=True, exist_ok=True)
    picture = Image.fromarray((gray * 255).astype("uint8"))
    picture.thumbnail((1280, 720))
    picture.save(paths[0])
    np.save(paths[1], depth)
    summary = {"source_report": str(args.report.resolve()), "source_raw_sha256": hashlib.sha256(data).hexdigest(),
               "shape": [height, width], "units": "raw device depth; metric scale not calibrated",
               "finite_fraction": float(np.isfinite(depth).mean()),
               "range_0_1_fraction": float(((depth >= 0) & (depth <= 1)).mean()),
               "percentiles": dict(zip(["0", "1", "5", "25", "50", "75", "95", "99", "100"], percentiles.tolist())),
               "preview": "percentile-normalized, gamma 0.35, display only"}
    paths[2].write_text(json.dumps(summary, indent=2), encoding="utf-8")
    print(json.dumps(summary))


if __name__ == "__main__":
    main()

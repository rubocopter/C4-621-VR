"""Opt-in comparison against the offline D3D12 executable; no game or XR."""
import os
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
from stereo_reprojection import reproject_eye


@unittest.skipUnless(os.environ.get('AC6_STEREO_GPU_EXE'),
                     'Set AC6_STEREO_GPU_EXE to a built offline D3D12 executable')
class StereoGpuReferenceTests(unittest.TestCase):
    def test_depth_order_and_unit_scale_match_gpu_and_literal_winner(self):
        executable = str(Path(os.environ['AC6_STEREO_GPU_EXE']).resolve(strict=True))
        color = np.array([[[255, 0, 0], [0, 255, 0]]], dtype='uint8')
        rgba = np.concatenate((color, np.full((1, 2, 1), 255, dtype='uint8')), axis=2)
        with tempfile.TemporaryDirectory(prefix='ac6-offline-reference-') as temporary:
            root = Path(temporary)
            for sign in (-1, 1):
                for reverse in (False, True):
                    for scale in ('collapsed', 'small_units'):
                        a = (-1e20 if reverse else 1e20) if scale == 'collapsed' else (-1 if reverse else 2)
                        b = (1e20 if reverse else -1e20) if scale == 'collapsed' else (1e-20 if reverse else -2e-20)
                        # Identical float32 coefficients on both sides of the file contract.
                        a, b = float(np.float32(a * sign)), float(np.float32(b))
                        matrix = [1, 0, 0, 0, 0, 1, 0, 0, 0, 0, a, sign, 0, 0, b, 0]
                        for values in ((.25, .75), (.75, .25)):
                            with self.subTest(sign=sign, reverse=reverse, scale=scale, depth=values):
                                depth = np.array([values], dtype='<f4')
                                fixture = root / 'fixture.bin'
                                # AC6RPRJ1 plus the unchanged 48-byte Params layout.
                                fixture.write_bytes(b'AC6RPRJ1' + struct.pack('<4I6f2I',
                                    2, 1, 1, 1, 1, 1, a, b, sign, 0, 0, 0) + depth.tobytes() + rgba.tobytes())
                                prefix = root / f'{sign}-{reverse}-{scale}-{values[0]}'
                                completed = subprocess.run([executable, str(fixture), str(prefix)],
                                    capture_output=True, text=True, timeout=30,
                                    creationflags=getattr(subprocess, 'CREATE_NO_WINDOW', 0))
                                self.assertEqual(completed.returncode, 0, completed.stdout + completed.stderr)
                                expected = values.index(max(values) if reverse else min(values))
                                cpu = reproject_eye(color, depth, matrix, 0, (1, 1))
                                self.assertEqual(cpu['source_index'][0, 0], expected)
                                self.assertEqual(Path(str(prefix) + '.indices.raw').read_bytes(), struct.pack('<I', expected))
                                self.assertEqual(Path(str(prefix) + '.rgba.raw').read_bytes(), rgba[0, expected].tobytes())
                                np.testing.assert_array_equal(cpu['color'][0, 0], color[0, expected])

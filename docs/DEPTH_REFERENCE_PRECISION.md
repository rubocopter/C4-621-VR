# Depth ownership and numerical limits in offline references

18 October 2026. Host-only correction on top of PR #5 (`5edd130`). No shader,
live stream, OpenXR presentation, default baseline or binary protocol changes.

## Reproduced problem

The NumPy reference and the native offline CPU oracle selected the nearest
sample by reconstructed double-precision distance. Distinct device depths can
reconstruct to exactly the same floating-point distance. For example, with
`A=-float32(1e20)`, `B=float32(1e20)`, `Wsign=1`, both device depths `.25` and
`.75` round to distance 1. In a 2x1 source downsampled to 1x1 at zero eye offset,
the old CPU selected source 0; the unchanged GPU selected the nearer source 1.
Both target positions (.25 and .75) are away from integer pixel boundaries.

The GPU already orders original device depth. The correction brings both CPU
references into agreement, preserving smallest-source-index ties only when
original device depths are equal. For canonical perspective matrices:

```text
Z = B / (Wsign * depth - A)
forwardDistance = Wsign * Z
d(forwardDistance)/d(depth) = -Wsign^2 * B / (Wsign * depth - A)^2
```

Thus larger device depth is nearer when B is positive; smaller is nearer when
B is negative, for either W sign. Rounded near/far distances also cannot
reliably classify reverse Z: `inspect_projection` now uses the sign of B.
Returned distance diagnostics remain subject to floating-point rounding; this
change preserves ownership information rather than inventing extra precision.

## Projection reference corrections

`tools/projection_math.py` no longer imposes absolute `1e-15` thresholds on
perspective W or the depth denominator. A tiny nonzero denominator can represent
a finite far plane; a small finite W can represent a valid point in arbitrary
game units. Only an exact zero denominator denotes the mathematical pole.
Nonzero reconstruction that overflows or underflows to zero is rejected;
projection/unprojection also reject nonfinite result coordinates.

The analytic helper now requires exact supported sparse matrix shape and
Wsign +/-1 instead of silently approximating small shear or noncanonical W.
This matches the native contract's matrix shape, but Python's float64 domain
is still wider than the GPU contract's normal float32 range. These fixes do
not relax native GPU validation, establish metres, or recover information
already lost when source matrix/depth values were stored.

## Reproducible validation

Build in a fresh output directory (do not overwrite a binary used by a process):

```powershell
cmake -S native/stereo_reprojection -B out/depth-reference-20261018/stereo_reprojection -G "Visual Studio 17 2022" -A x64
cmake --build out/depth-reference-20261018/stereo_reprojection --config Release --parallel 2
$env:AC6_STEREO_GPU_EXE = (Resolve-Path out/depth-reference-20261018/stereo_reprojection/Release/ac6_offline_stereo.exe).Path
& .venv-analysis/Scripts/python.exe -B -m unittest discover -s tests -v
ctest --test-dir out/depth-reference-20261018/stereo_reprojection -C Release --output-on-failure
& .venv-analysis/Scripts/python.exe -m compileall -q tools tests
git diff --check
```

Results on RTX 4070 Ti, without D3D12 debug layer:

- 41 Python tests pass, including 16 direct Python/GPU/literal-winner fixtures
  with identical float32 coefficients, both W signs, both depth directions,
  reversed source order, collapsed distances and small game-unit scales.
- The native CTest passes all 37 internal GPU cases, comparing exact source
  indices and RGBA bytes. This retains the previous 25 cases covering parallax,
  invalid depth, holes, ties and source/destination resolution changes.
- The new tests fail against the previous Python implementations copied from
  `5edd130` into an isolated ignored directory: 25 relevant test methods,
  10 subtest failures and 9 errors. The native literal-winner assertion also
  failed before the oracle correction. No checkout or prior binary replaced.
- A separate existing PR #5 GPU executable reproduced source 1 while the old
  Python code chose source 0. Local evidence is under
  `out/depth-reference-20261018/`: `cpu-gpu-before.json`, `native-red.log`,
  `previous-reference-red.log`, `python-gpu-green.log`, `native-green.log`.
- Independent review found no actionable issues in the five-file core diff.

Without `AC6_STEREO_GPU_EXE`, the portable suite runs 40 tests and explicitly
skips the one GPU integration test. CI builds the native executable but runs
only the workflow's non-GPU checks; local D3D12 evidence is separate.

## Remaining validation boundary

These synthetic coefficients expose numerical assumptions; they are not newly
observed AC6 camera parameters. The frame identity of the game projection,
jitter and camera remains unverified. The optional eye-intrinsics path remains
disabled by default. Hidden surfaces and actual metric scale remain unresolved.

No AC6/SteamVR launch, injection or headset trial occurred for this increment.
No new physical test is required for this offline-reference-only correction.
The earlier PR #4 2560x1440/90-degree acceptance remains bounded to that build;
there is no new claim about visual quality, performance or comfort.

## Continuous consumer fixture oracle — follow-up

The same distance-sorting assumption remained in
`native/live_stereo/test_live_stereo.cpp::Oracle`. This oracle is separate from
the two offline references corrected above. It now also orders original device
depth by the sign of B, with source-index ties only for equal device depths.
No production renderer or shader changes.

Tests were introduced before correcting the oracle. A CPU-only literal-winner
check fails on the old oracle; the new two-process GPU fixture also fails
(frame4/eye0/pixel2723), because the existing GPU chooses the correct source
while the oracle expects the farther one. Synthetic profiles18..21 cover both
W signs and depth directions, swapped source order, true device-depth ties,
invalid samples, skipped/repeated/expired frames, and reconnection. These
profiles use identical source/target rays and the unchanged .5 baseline.

The CPU check explicitly establishes that both reconstructed distances round
to1 while the input depths remain .25/.75. Two source pixel centers map
strictly inside one destination pixel for both eye offsets, so the literal
expected owner does not depend on raster boundary rounding or another
reconstruction implementation. The test runs in non-GPU CI as
`live_stereo_depth_order_oracle`; the hardware comparison is
`live_stereo_collapsed_depth`. This completes oracle consistency for live
fixtures without claiming more precision in the distance diagnostics.

```powershell
cmake -S native/live_stereo -B out/live-oracle-depth-order-20261018/live_stereo -G "Visual Studio 17 2022" -A x64
cmake --build out/live-oracle-depth-order-20261018/live_stereo --config Release --parallel 2
ctest --test-dir out/live-oracle-depth-order-20261018/live_stereo -C Release --output-on-failure --no-tests=error
```

RED/GREEN logs: `out/live-oracle-depth-order-20261018/red.log` and `green.log`.
All14 live CTests pass on RTX4070Ti without the D3D12 debug layer, including
the four new hardware profiles and all previous profiles/resolutions/gates.
Portable Python:40 pass and the optional offline GPU integration test explicitly
skipped. Syntax/diff checks pass; independent review found no actionable issues.
Fixtures are synthetic, not newly observed AC6 camera matrices. No additional
game/headset test is needed for this fixture-only correction.

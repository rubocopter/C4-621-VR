# DLSS input/history host contract

Standalone C++17 admission and stereo-history policy. No NGX SDK, D3D12,
OpenXR, game access, production adapter or GPU dependency. `contract.hpp`
reuses `../reprojection/projection.hpp` for supported projection validation.
Passing this contract means only that supplied host declarations are coherent.

## Input declarations

`PairInput` is a value snapshot with exactly two optional eye slots: left,
then right. Both must exist. Every color, depth, motion and view binding has
its own `Stamp`: explicit eye, complete nonzero generation/source-frame/
simulation key, `FrameAssociated`, and a nonzero association-record reference.
All bindings must match the pair key and their slot's eye. The four bindings
within each eye must share that eye's association record. Different per-eye
association records and projection principal points are supported.

**Resource-view identities and association records are upstream declarations,
not evidence proved by this pure validator.** A record must refer to upstream
evidence associating the actual render, resources and constants; inventing an
ID or setting `FrameAssociated` does not discover that evidence. Resource-view
IDs identify a lifetime-scoped resource/subresource view, not a raw address.
The host cannot detect falsely relabelled or aliased resources from IDs alone.
Both association and eye enums default to `Unknown`.

The current shared protocol v1 has color/depth publication sequence/tick but
no complete per-eye render association, projection/jitter or motion inputs.
Neither static projection calibration provenance mode is frame-associated.
There is intentionally no adapter from either source into accepted inputs;
they must remain `Unassociated`/`Unknown` and reject. Completed fences, recent
timestamps and plausible matrices do not identify which render used constants.
Synthetic fixtures may declare their own known association.

## Supported input semantics

- Render/output dimensions are 1..8192 in each axis; output must be at least
  render size (equal size is allowed). All three surfaces must be nonzero
  resource-view IDs at render size. Configuration revision must be nonzero.
- `View` carries **projection matrices only**, never a full view-projection
  matrix or camera pose. Both matrices use the repository's supported
  row-vector perspective shape, finite coefficients and device depth [0,1].
  `Perspective` and `RayIntrinsics` validation preserve asymmetric principal
  points and reject unsupported shapes/subnormals. Forward/reverse depth
  direction must be explicit and agree with the depth coefficient `b`.
- Jitter is the applied displacement in render pixels, +X right/+Y down,
  with an explicit phase (phase zero is valid). Both eyes must declare the
  same phase and pixel jitter. Known applied zero jitter is valid. Both W
  signs are supported. Only projection entries 8/9 may change:

  ```text
  (rendered[8] - unjittered[8]) / Wsign =  2*jitterX/renderWidth
  (rendered[9] - unjittered[9]) / Wsign = -2*jitterY/renderHeight
  ```

  Validation reconstructs displacement in double and allows at most 1/1024
  render pixel error per axis. This fixed pixel bound cannot grow with a
  large principal point. Nonfinite/subnormal declared jitter rejects.
- Motion must declare full-scene coverage and current-to-previous displacement
  in render pixels, +X right/+Y down, with jitter excluded. Unknown coverage,
  camera-only vectors and other conventions reject. There are no floating
  motion samples in this API: actual texture values are outside its scope.
  A previous-frame key is either entirely absent (all zero), or complete,
  in the current generation and strictly earlier than the current source
  frame. Absence/mismatch with committed history requires reset; it never
  makes missing motion surfaces or unsupported motion conventions acceptable.

## History transaction

Create one noncopyable, nonmovable `History(nonzeroGeneration)` per stereo
stream, and serialize its calls. The caller owns resource lifetimes and must
report both backend evaluation outcomes truthfully.

1. `Prepare(pair)` validates all current inputs and returns a `Decision` with
   either a `Reject` reason and no plan, or an immutable `Plan`. A plan owns
   its input snapshot. `Input()`, `ResetHistory()` and `Previous()` expose
   const data for a future adapter; histories are separate left/right values.
2. Apply the plan's reset to **both** backend eye histories before evaluation.
   `Previous()` is empty for both eyes on reset. Otherwise each eye receives
   only its own last committed input and its motion reference matches that
   committed frame key exactly.
3. Call `Finish(plan, {leftResult,rightResult})`. Only two `Succeeded` results
   commit the stereo snapshot and advance the source-frame replay watermark.
   `Failed`, `NotEvaluated`, or any unsupported result invalidates reuse for
   both eyes, because even one evaluation may already have modified a backend.
   A failed frame can be retried with both histories reset.

`Prepare` never changes committed eye inputs or the replay watermark. Its
admission ticket, revision and `forceReset` state may mutate: every call
invalidates the preceding ticket.
Incomplete/invalid pairs force subsequent reset. Superseding any unfinished
plan also forces reset: its evaluation may already have touched backend state.
Only one plan can be outstanding. A stale or foreign `Finish` rejects without
disturbing the receiving history's current ticket. Tickets bind a private
owner token and revision, so another `History` at the same memory address
cannot inherit them. Tickets cannot be constructed or assigned by callers.

First use, explicit reset/camera cut, changed dimensions/configuration revision,
depth-direction/W-sign changes, and missing temporal continuity reset both
eyes. A skipped source frame can reuse history only if both motion fields
explicitly reference the actual last committed frame. Ordinary per-frame
jitter and principal-point changes do not themselves change configuration.
The caller must increment configuration revision for other incompatible
pipeline/input semantics that the host cannot infer.

`Reset(reason,generation)` revokes tickets and forces both-eye reset. Same-
generation reset retains the replay watermark, including `Rebind` with the
same generation. A different generation requires explicit `Rebind`; it clears
old committed inputs and begins a new watermark. Generation IDs must not be
recycled by the upstream owner. Constructor/reset misuse throws
`std::invalid_argument`. Revision exhaustion throws `std::overflow_error`;
create a new history rather than recycle tickets.

Invalid inputs reject even after a reset, configuration change or temporal gap.
A reset cannot certify provenance, replace missing planes or waive validation.

## Tests and bounded execution record

From the repository root:

```powershell
cmake -S native/dlss_contract -B out/dlss-contract-20261101 -G 'Visual Studio 17 2022' -A x64
cmake --build out/dlss-contract-20261101 --config Release --parallel 2
ctest --test-dir out/dlss-contract-20261101 -C Release --output-on-failure --no-tests=error
```

Eight deterministic CTest groups cover admission, association/eye identity,
projection/jitter with both W signs and depth directions, depth/motion
semantics, every floating matrix/jitter field with NaN/+Inf/-Inf, stereo
commit/failure, stale/foreign tickets including owner-address reuse, replay,
resets and generation rebind. No graphics device or SDK is required. The
interface library may also be consumed through `add_subdirectory`.
Numerical cases include subnormal coefficients/jitter, the exact pixel-error
boundary and adjacent float values, and lost jitter at large principal points.

Tests were written first and all eight groups failed against the deliberately
unimplemented interface scaffold before validation/history implementation.
The initial failure transcript is retained in the ignored build directory as
`red-ctest.log`. The final implementation contains no scaffold behavior.

A fresh read-only reviewer found no correctness defects. Its coverage findings
were addressed with independent reset-trigger tests (avoiding unfinished-plan
reset as a confounder), depth/W-sign transitions, and numerical boundary cases.
Failure tests isolate every non-success left/right evaluation-result combination
in separate histories for immediate committed-frame replay rejection and for
immediate reset/empty-history admission. Neither probe can mask the other.

Scope decisions: use the authorized shared checkout; touch only this directory;
leave CI and production unchanged; retain progress/evidence here and under the
requested ignored build directory instead of creating other tracking files.

Actual GPU resource descriptors, aliasing, ownership/lifetime, texture contents,
resource states, barriers/fences, NGX feature creation/evaluation, exposure/color
normalization and headset/runtime acceptance remain future adapter work. This
component does not establish AC6 frame association or implement DLSS rendering.

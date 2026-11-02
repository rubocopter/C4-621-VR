# Native views and DLSS preparation — 2026-11-01

The user authorized autonomous work on the PC after reviewing both DS3 VR
implementations and their per-eye DLSS. This is the first engineering stage:
identify AC6's actual scene-view route and build an independent, testable
admission/history contract for later DLSS inputs. Existing accepted stereo
production is the reference and is not replaced by this stage.

## Approach and decisions

Two complementary tracks: static investigation of native view creation/listing
above D3D12, and a standard-library-only C++17 DLSS contract. Native views may
eventually avoid single-source occlusion gaps. DLSS needs coherent temporal
inputs regardless of how stereo is generated. A generic command-list replay
or a second call of the full frame renderer is not selected: neither has a
verified lifetime/simulation contract. Integrating NGX before actual per-eye
depth/motion/projection exist would misrepresent capability.

Reuse the working checkout, preserving its uncommitted research and isolated
historical binaries. Keep game disassembly private outside the repository.
Do not install dependencies, alter game files, change presentation defaults,
activate frame metadata, or overwrite retained DLLs for this stage.

## Native investigation

Verify the installed executable SHA-256. Follow AC6 instructions from its own
camera/renderer and RendMan label; DS3 addresses are hints only. Record scene
factory, view-list add/remove/render, camera-link ownership, target setters and
main renderer callsites with bounded decoded-instruction evidence. PE unwind
ranges alone are not complete semantic functions: preserve unresolved jumps.
Produce a reproducible read-only tool and a metadata-only report; private
disassembly must not be distributed. Runtime activation requires separate
current offline/build checks and a bounded diagnostic, not speculative writes.

## DLSS contract

Add `native/dlss_contract/` with no NGX, D3D or OpenXR dependency and no connection
to production. Inputs are immutable value snapshots, with explicit Left/Right
identity and complete source generation/frame/simulation identity on color,
depth, motion and view data. All semantic defaults are Unknown. Every bound
input requires a nonzero upstream association-record reference; this reference
is an upstream attestation, never evidence manufactured by this validator.
No adapter from shared protocol v1 or static calibration may mark it associated.

Validate finite supported row-vector projections, render extents, explicit
[0,1] depth direction, current-to-previous full-scene motion convention, and
applied jitter in render pixels (+X right/+Y down). Rendered minus unjittered
projection must equal declared jitter; phase/pixel jitter must agree across
eyes. Reject unknown/camera-only motion for this initial contract.

History is stereo-transactional: Prepare may mutate admission ticket/revision
and forceReset state, but never committed eye inputs or the replay watermark.
This conservative admission rule revokes unfinished tickets and forces reset
after rejected inputs or superseded work. Finish commits only if both
evaluations succeeded. Any evaluation failure invalidates both
backend histories. Configuration/size changes, camera cuts, missing continuity
or explicit reset require both-eye reset. Reject generation mismatch, replay,
stale/foreign plans and incomplete pairs. Reset cannot waive input validation
or erase the replay watermark. Synthetic fixtures may attest their own data;
the existing AC6 v1 stream must still reject as unassociated.

The contract's accepted decision is host admission only, not GPU completion,
actual texture correctness, NGX availability, frame association discovery or
headset acceptance. A later adapter must verify real resources and fences.

## Verification and outcome

After the static route and external identities were verified in offline AC
test, extend the runtime census with a separate TWO-hook diagnostic at the
actual render-all-views and composite-view render entries. No creation calls,
camera writes or GPU work: preserve ABI, opaque return, exceptions, last error
and nested thread scope; bounded fixed storage; close after four complete view
loops or first overflow, max eight seconds/F12; restore hooks and retain DLL.
Host tests and independent review precede its single runtime load. Association
to captured depth/actual GPU execution remains false. This extension observes
normal engine calls rather than testing renderer reentrancy.

Use synthetic parser fixtures for reusable binary-analysis logic and deterministic
C++ tests for admission, eye isolation, jitter, depth/motion, temporal reset,
generation, stale plans and all-or-nothing commit. Build into a new ignored
directory. Review with a fresh agent after implementation. Report discovered
native routes separately from host-only DLSS preparation and untested runtime.

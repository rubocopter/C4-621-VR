# Native view resource ownership

Continue the authorized AC6 native stereo investigation in the shared checkout.
The previous bounded trace established four main-view CPU scopes, with restored
hooks, but did not create another view or identify independent GPU outputs.

## Approach

Follow AC6's target initializer and release paths, then extend the existing
external reader with an optional resource census. This is the next useful
experiment: a target-set allocation is insufficient evidence that its textures
are independent. Blind object copies lose reference/pool ownership, and calling
the complete renderer twice does not establish a second engine view.

Reuse Capstone's bounded instruction verifier and the pinned-handle Win32
preflight. No production presenter/protocol changes. No factory calls until
its input and camera/target ownership are established. Keep game bytes and
captures private. The earlier backup remains available; any new run is offline.

## Census

Read only statically verified fields: concrete view type, view target/scene/
camera and pipeline owner/state, target configuration extent, primary bundle
color/depth references, named velocity/depth resources, and global output
references. Compare object identities across bounded repeated reads; report
pointer equality explicitly rather than asserting ownership or independence.

Each selected nonzero pointer must be canonical/aligned, reads exact and bounded.
Null optional resources are absence, not errors or substitutes. Changes to
tracked identities produce an unstable result. Equality cannot rule out ABA,
aliasing, GPU in-flight use or shared backing allocations. Never infer motion
conventions, temporal validity, frame association, or DLSS readiness from labels.

## Verification

Synthetic memory tests first, including global-output alias, independent
reference, absent resources, bad/truncated reads, wrong type, changed identity,
and preservation of unknown GPU/frame ownership. Verify new instruction anchors
against the exact executable. Run the extended observer in an individual AC
test if host validation passes; close the game normally after evidence capture.

## Result required

Document the actual allocation/borrow/pool paths, measured relationships and
remaining prerequisites for a zero-difference second-view control. A factory
that still has unverified input/lifetime is a specific engineering gap, not an
invitation to guess its ABI or share destructed camera links.

## Follow-up: capture factory arguments before a construction control

The live census confirms shared primary output references. Before cloning,
capture the concrete factory's actual 0x70-byte descriptor and 0x18-byte auxiliary
input in the same process. At the title screen the main view manager is absent;
Continue is a natural bounded creation trigger. Use a separate single-hook
observer at verified RVA 0x22f0c50, close on first completed factory invocation
or timeout, preserve original ABI/result/error/exceptions and restore its entry.
The snapshot is diagnostic only: it must not call the factory again, edit input,
create extra views or reuse raw captured pointers across process lifetimes.

# Live stereo diagnostic implementation plan

**Goal:** bounded continuous color/depth GPU stream into stereo OpenXR quads.
**Architecture:** 3-slot shared DEFAULT-buffer ring, ready/released fences, existing command ledger, standalone GPU stereo consumer.
**Tech:** C++17/MSVC, D3D12, OpenXR, MinHook, Python orchestrator.
**Spec:** ../specs/2026-10-16-live-stereo.md

## Constraints
Main only; no game-file changes or rebuild/reload of mapped DLLs. Stream 60s maximum/F12. No waits/IO in game submission hook. GPU resource lifetimes retained on failure.

## Tasks
- [x] Protocol + deterministic ownership/drop/stale tests (RED/GREEN).
- [x] Producer sibling of one-shot Capture, new unique DLL, synthetic hook host; prove repeated paired copy and backpressure/resize cancellation.
- [x] GPU consumer library + changing/shared-buffer fixture, exact eye/color/winner tests (two consumer CTests).
- [x] Integrate consumer in display probe with exclusive --pair-stream option; bounded loader/orchestrator, freshness and evidence.
- [x] Review stream races, resource lifetimes and failure paths; monitor-only live diagnostic successful (PID 5440, 721 pairs published, 414 consumer updates in 6 s).
- [ ] Run short live binocular PSVR2 trial (`--mode vr`); record user feedback and outcome before adding headtracking.
- [ ] Verify combined binocular/headtracking trial and calibrated eye projections/scale in follow-on gates.
- [ ] Commit/push completed code and docs on main after verification.

## Review focus
Slot reuse while GPU reads; skipped sequences; unknown/aliased game list and multi-queue state; resize retaining backbuffer references; consumer exit/no consumer/device loss with outstanding copies.

## Ledger
Ruling: user already explicitly authorizes necessary work and prefers main; execute without another plan-approval round. Producer/consumer have disjoint source sets, shared protocol is fixed before delegation. Static screen geometry is retained for the first live trial to compare with the user-confirmed result; full asymmetric projection is deferred.

Protocol/producer: stub RED then changing paired GPU fixture GREEN. Added hardware queue-gate fixture with fake original-batch observer to prove pending-ready prevents slot reuse despite release advance, color-only cannot reuse old depth, and one original enqueue contains private tail. This gate fixture does not itself execute copy commands; original hardware fixture verifies actual bytes separately. Reviewer identified unsafe-ledger silent stall: terminal Failed publication added, removed to observe regression fail, restored to pass. Three CTests pass. Separate consumer-device test remains delegated. stop_requested at fixed header offset36 lets orchestrator request early worker exit; still bounded60s/F12, no unloading or resource teardown.

18-10 resume: current process30252 is identified (same build/cooperative, no probe modules), offline mission confirmation pending. Fresh independent consumer/integration review found a recoverable stale-source exception could skip release/EndFrame. Fixed with TryBlit(false) and tested stereo-frame transaction; RED then GREEN for stale GPU source and eye0/eye1 expiry/exception. Fresh results: Python25, producer3, consumer2, presenter4, toolsAST17. Live PSVR2 remains the next user-dependent gate, not assumed passed.

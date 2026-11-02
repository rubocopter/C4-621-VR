# Native views and DLSS preparation implementation plan

> **For agentic workers:** Execute the independent DLSS contract and native-route investigation in parallel. Preserve all pre-existing changes.

**Goal:** Identify the AC6 scene-view factory/list route and implement a host-only per-eye DLSS input/history contract.

**Architecture:** Static PE analysis supplies native instruction evidence; no game mutation. A separate standard C++17 component validates supplied temporal inputs and transactional stereo history, with no NGX backend or production adapter.

**Spec:** [Design](../specs/2026-11-01-native-views-dlss-design.md).

## Global constraints

- Preserve production protocol v1, accepted presenter, defaults and historical builds.
- Fingerprint the game; private disassembly stays outside the repository.
- Current stream/calibration cannot claim frame-associated DLSS inputs.
- No runtime hooks or secondary render until build/offline/ownership conditions are established.

## Tasks

- [x] Trace AC6's own view-list render, add/remove, main-view factory and camera/target ownership. Retain exact sites, limits and private evidence.
- [x] Implement reusable, bounded native-route inspection in `tools/inspect_native_view_route.py` with synthetic tests in `tests/test_native_view_route.py`; reject fingerprint mismatch and unverified decode boundaries.
- [x] Implement `native/dlss_contract/contract.hpp`, native tests, CMake and README. First run meaningful failing tests, then implement validation and transactional history.
- [x] Add the independent host component to CI without GPU/game dependencies; run its CTests and affected Python tests.
- [x] After offline external census, implement/test/review a separate two-hook native view-scope observer; run one bounded load with restoration evidence, without creating views or editing camera/GPU state.
- [x] Review the implementation independently and fix substantiated issues; update status, build instructions and research log with exact host/runtime limits.

## Review focus

- Instructions in the second PE `.text` section or fragmented functions must not be silently excluded.
- A plausible class/label or numerical matrix never becomes frame ownership evidence.
- Unknown input, partial pair or generation mismatch must reject even after a reset.
- One-eye evaluation failure must not leave reusable mixed temporal history.
- A stale or foreign plan must never commit over a newer history state.

# View Resource Ownership Implementation Plan

> **For agentic workers:** Execute inline with superpowers:executing-plans; preserve all existing changes.

**Goal:** Identify native view resource sharing before constructing a second view.

**Architecture:** Exact-build static anchors plus an optional read-only resource
census integrated with the existing pinned-handle external observer.

**Tech Stack:** Python, Capstone/pefile, ctypes Win32; existing Windows game host.

**Spec:** [Design](../specs/2026-11-01-view-resource-ownership-design.md).

## Global constraints

- No renderer duplication, factory invocation, game-file edits or GPU commands.
- No frame/DLSS association from pointers or resource labels.
- Existing route/census formats remain usable without the new option.
- Private instruction dumps, logs and captures stay ignored.

## Tasks

- [x] Follow target initialization, named buffers, global output and view-state
  pool acquisition/release. Add exact static checks without rewriting history.
- [x] Write meaningful synthetic memory tests for `sample_resources(read,base)`
  in `tests/test_native_view_resources.py`; observe failure, implement
  `tools/native_view_resources.py`, then verify green.
- [x] Integrate `--resources` into `tools/observe_native_views.py` using the same
  read-only handle; verify new code anchors before interpreting fields.
- [x] Run host tests, static checks and one bounded offline external census;
  retain evidence and close the game normally.
- [x] Update findings, current status and log with exact sharing and remaining
  second-view prerequisites, including GPU lifetime and temporal limitations.
- [x] After the read-only census, capture one natural main-view factory
  invocation from title/Continue using a separate host-tested single-hook
  observer. Preserve its frozen artifact and verify restoration externally.

## Review focus

- Global-output null values must never become a claimed alias.
- Changes to view/target/global identities must not produce a coherent result.
- Truncated, noncanonical and misaligned pointers must refuse before traversal.
- Object identity is not a D3D12 resource identity or allocation-lifetime proof.
- A named velocity target cannot certify full-scene motion or its conventions.

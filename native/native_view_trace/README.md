# Native view trace

Observation-only, two entry hooks: RenderAllViews RVA `0x2466510` and composite
View::Render RVA `0x2524790`. No creation, edits, GPU work, exports or unloading.
Public snapshot fields and ABI are in `trace.hpp`. Schema is
`native_view_trace_v1`; field names match the coordinator's offline analyzer.

Build SHA-256:
`e003771e5548bca91882c1d7dfe2df3de97cee05c2c989d0c0e34b036ea610d6`.
The two 16-byte signatures were obtained by reading that verified PE on disk
with Python, not by executing or inspecting the game process. The worker checks
loaded PE machine/timestamp/image size and both signatures; it does not compute
the full disk SHA. Full-image verification remains an external precondition.

Build/test (existing MinHook; no downloads):

```powershell
cmake -S native/native_view_trace -B out/native-view-trace-20261101 -G 'Visual Studio 17 2022' -A x64
cmake --build out/native-view-trace-20261101 --config Release --parallel 2
ctest --test-dir out/native-view-trace-20261101 -C Release --output-on-failure --no-tests=error
```

This task only builds and runs synthetic host fixtures. No game launch, input,
injection or live module inspection. The coordinator must independently verify
fresh PID/module absence, disk SHA, offline host and ownership before any load.

The output is **only** the new build's `Release/ac6_render_probe.dll`. Do not
overwrite a historical DLL or load it where the same basename is already
mapped. The unmodified generic loader can find its exclusively created
`render_probe_PID.jsonl` beside this DLL; it is written only after shutdown.

## Observation and shutdown

Two x64 signatures only: `U RenderAll(U list,U context)` and
`U ViewRender(U self,U context,U frameIndexPtr)` return opaque 64-bit RAX.
Every invocation forwards its original arguments exactly once, including
inactive/rejected paths, preserves incoming/outgoing last error, and propagates
original exceptions. SEH `finally` restores TLS and active counts. An original
exception never publishes a completed event. Callbacks use only fixed storage
(128 events), atomics, TLS, thread ID and guarded bounded reads: no I/O,
allocation, factory calls, object edits or GPU work.

The worker requires the expected executable module, `ac6_coop.dll`, its own
DLL basename, and absence of known EAC modules. Initial identities are sampled
twice from `image_base+0x4e23028`: manager, manager+8 list, manager+0x10 view
manager, view-manager+0x3c8 main view. The main vtable must equal
`image_base+0x3e3e7e0`; identities are rechecked before enabling admission.
Equal samples do not prove lifetime, exclude ABA, or certify continued ownership.

Only the sampled list admits RenderAll events. Its TLS token is the RenderAll
begin ordinal; every composite view within it is observed, not only the main
view. Unmatched RenderAll calls increment `unmatched_lists`, clear inherited
TLS during forwarding, and restore it afterwards. Views without a TLS parent
increment `unscoped_views` and forward without a join. Counters apply while
admission is open. Nested admitted RenderAll calls retain their real enclosing
parent token: the current analyzer intentionally rejects such a window because
it requires flat RenderAll roots. It must not rewrite nesting as flat calls.

Admission closes after four successfully completed admitted RenderAll scopes
(fixture-configurable), first overflow, F12, or the worker time bound. The
eight-second admission/drain budget reserves its last second for cleanup, so
observation lasts at most seven seconds. Synchronous MinHook operations and
file I/O are not hard-real-time. Queued disable is followed by independent
16-byte comparisons of both entries. DLL, trampolines and callback storage
remain mapped and inactive: no exports, hook removal, MinHook uninitialization
or DLL unload.

The ordinal cutoff filters straddlers (`end >= recording_cutoff`). Aborted or
late events remain visible as `event_attempts != saved_events`; overflow,
unmatched/unscoped calls or pending callbacks also invalidate completeness.
On failed drain or restoration the worker writes metadata/restoration/summary
only and never traverses mutable event storage. Concurrent completions can
cross the stop boundary; the analyzer must reject excess/mismatched scopes.
None of these observations establishes GPU submission, resources, rendered
frame identity or safe renderer reentrancy. `frame_associated` stays false.

## Exact JSONL fields

All pointers/ordinals are unsigned JSON integers, not hexadecimal strings.
Rows are emitted in this order after shutdown:

- Header: `phase="observing"`, `schema="native_view_trace_v1"`, `image_base`,
  `pid`, `expected_list`, `expected_main_view`, `frame_associated=false`.
- Two restoration rows: `phase="restoration"`, `rva`, `restored`.
- Each saved event: `phase="event"`, `kind` (1 RenderAll, 2 composite view),
  `begin`, `end`, `parent`, `thread`, `list`, `view`, `context`, `vtable`,
  `targets`, `scene`, `camera`, `valid`. Kind 1 has `view=0` and zero snapshot
  fields. `parent` is the enclosing RenderAll begin token, zero for roots.
  View validity bits 1/2/4/8 mean successful reads at self+0/+0x38/+0x40/+0x48.
  A successful read of a null pointer still sets its bit; it is not validity
  of the referenced object. `frameIndexPtr` is forwarded without dereferencing.
- Summary: `phase="disabled"`, `status`, `disable_status`, `hooks_restored`,
  `saved_events`, `event_attempts`, `event_overflow`, `pending_at_shutdown`,
  `completed_frames`, `recording_cutoff`, `first_loss_sequence`,
  `unmatched_lists`, `unscoped_views`, `dll_retained_inactive=true`,
  `frame_associated=false`. Status strings are MinHook names or a preflight
  rejection reason. No in-callback log writes occur.

Ten host groups cover both opaque ABIs and last error (also through two actual
local MinHook targets and inactive paths), nested TLS, original exceptions,
overflow, four-scope cutoff, guarded pages, root offsets, pending/late completion
rejection and exact JSONL generation. Initial eight core tests failed against
the unimplemented scaffold; both writer tests also failed before implementation.
The synthetic `trace_tests.exe json` output passes the coordinator's offline
analyzer with four main scopes, while retaining `frame_associated=false`.
The existing MinHook CMake emits a deprecation warning; it was not modified.

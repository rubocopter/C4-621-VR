# Native view factory observation

Independent observation-only hook at factory RVA `0x22f0c50`. ABI:
`uintptr_t __fastcall(uintptr_t factory, uintptr_t descriptor, uintptr_t auxiliary)`.
The return register is opaque: successful return does not prove a view type,
main-view identity, coherent ownership, frame association or GPU readiness.
No factory retries, construction controls, camera/input changes or GPU calls.

Build with MSVC x64 and the existing `reference/minhook` (no downloads):

```powershell
cmake -S native/native_view_factory_trace -B out/native-view-factory-20261101 -G "Visual Studio 17 2022" -A x64
cmake --build out/native-view-factory-20261101 --config Release
ctest --test-dir out/native-view-factory-20261101 -C Release --output-on-failure
```

Targets: `factory_trace_tests`, `factory_trace_core`, `ac6_render_probe`.
DLL: `out/native-view-factory-20261101/Release/ac6_render_probe.dll`.
The basename and adjacent `render_probe_PID.jsonl` reuse
`tools/load_camera_probe.py`. Loader/UI/analyzer integration is separate.
This implementation was built and tested locally, without game inspection,
injection, game input or commits. Existing MinHook emits a CMake deprecation
warning; its source is unchanged.

The local disk PE was read before implementation. Machine `0x8664`, timestamp
`0x6822405f`, image size `0x6c2fc00`, SHA-256
`e003771e5548bca91882c1d7dfe2df3de97cee05c2c989d0c0e34b036ea610d6`.
RVA `0x22f0c50` maps to file offset `0x22f0250`, exact 16 bytes:
`40 55 56 57 41 54 41 55 41 56 41 57 48 8d 6c 24`.
Worker independently checks the main `armoredcore6.exe` module, own DLL
identity, PE signature/AMD64/PE32+/timestamp/image size, `ac6_coop.dll`, and
absence of modules containing `easyanticheat` or beginning `eac_` (case
insensitive), then checks that full entry prefix before creating the hook.
Full executable SHA verification remains the external loader's responsibility.

The callback has one static slot and an atomic admission/completion/cutoff
state. Exactly one invocation can claim it, including concurrent or recursive
calls; all invocations call the original exactly once with all three unchanged
arguments. It restores incoming GetLastError immediately before the original,
returns its unchanged RAX, and restores outgoing GetLastError. `__finally`
decrements the active count on normal return and propagated C++/SEH exceptions.
Original exceptions never publish an event. No callback heap or event/file I/O.

The descriptor's `0x70` bytes and auxiliary's `0x18` bytes are copied BEFORE
the original. After a normal original return, a bounded `0x140` result prefix
supplies vtable at `0`, targets at `0x38`, scene at `0x40`, camera at `0x48`,
pipeline owner at `0x130`, and pipeline state at `0x138`. Target parameters
are the inline `0x78` bytes beginning at `targets+0x20`, with no extra pointer
dereference. The factory pointer and input addresses are diagnostic only.

Reads use a minimal independent copy of `native_view_trace`'s read utility:
overflow/null/zero-length/4096-byte bound checks, VirtualQuery across every
region, committed readable protection checks, and SEH around memcpy. A known
PAGE_GUARD is rejected before any source access, including cross-region reads;
guard flags are not consumed in the host tests. These checks cannot eliminate
concurrent remapping/protection/lifetime races or make snapshots atomic.
No vtable functions or diagnostic pointers are invoked.

The worker stops admission on first admitted normal completion, F12 being
held, or eight seconds from worker start, polling every millisecond. It queues
disable and applies it, compares all 16 original entry bytes, then drains
active callbacks for up to one further second. Hook management and final file
I/O are synchronous and not hard-real-time. An invocation completing after
cutoff cannot set ready. A pending callback, failed disable or failed
restoration suppresses the event. Log creation is exclusive, after shutdown;
an existing log is never overwritten. DLL, event storage, MinHook records and
trampolines stay mapped; there is no unload/remove/uninitialize path.

All raw snapshots belong only in the private ignored JSONL log (`*.jsonl` and
`out/` are already ignored). Do not publish captures or reuse addresses across
process lifetimes. The first captured result can be a non-main view; a
coordinator must compare its result with the postflight main holder in the
same process creation lifetime. This does not establish frame association.

JSONL is emitted in this order. Every address and counter is an unsigned JSON
integer, every boolean is a JSON boolean, and hex is lowercase with no prefix.

| Phase | Fields |
| --- | --- |
| `observing` | `schema="native_view_factory_trace_v1"`, `image_base`, `pid`, `process_creation`, `closed_tick_ms` |
| `restoration` | `rva=36637776` (`0x22f0c50`), `restored` |
| `factory_event` (zero or one) | `seq=1`, `thread`, `factory`, `descriptor_address`, `auxiliary_address`, `result`, `valid`, `descriptor_hex`, `auxiliary_hex`, `target_params_hex`, `view_vtable`, `targets`, `scene`, `camera`, `pipeline_owner`, `pipeline_state` |
| `disabled` | `status`, `disable_status`, `hooks_restored`, `attempts`, `completed`, `pending_at_shutdown`, `dll_retained_inactive`, `frame_associated=false` |

`process_creation` is the uint64 FILETIME from GetProcessTimes: high DWORD
shifted by 32, OR low DWORD, 100-nanosecond ticks since 1601 UTC. Failure to
obtain it rejects admission. The phase `observing` is a retrospective header;
it is written only after observation closes, including on preflight rejection.

`closed_tick_ms` is uint64 GetTickCount64 sampled after cutoff, disable,
restoration check and bounded drain. For a later external census in the same
process creation lifetime, require `sample_begin_tick_ms > closed_tick_ms`.
The shared system clock is quantized: equal tick values cannot establish
ordering. This bound does not establish snapshot coherence or frame identity;
successful disable/restoration and zero pending callbacks remain required.

`valid` bit 1 means the entire descriptor read succeeded, bit 2 auxiliary,
bit 4 result prefix, bit 8 target parameters. All successful reads give
`valid=15`, descriptor hex length 224, auxiliary 48, target parameters 240.
Failed read hex fields are empty strings, and unavailable selected fields
are zero. Partial reads never become valid. A normally returned invocation
with partial/invalid reads can still produce a diagnostic completed event;
an unfinished/throwing invocation never does.

`attempts` counts admissions (0 or 1); `completed` counts normally returned
admitted originals, even a result returned after cutoff; neither counts
inactive calls. `pending_at_shutdown` samples all active detour invocations
after bounded drain. `dll_retained_inactive` is true only with closed
admission, successful disable, verified restoration and no pending callbacks.
`status` and `disable_status` are `MH_OK` on success; status can instead name
a preflight rejection or MinHook failure. A failed disable stays reported
even if best-effort synchronous disable subsequently restores the entry.

Host TDD: all ten tests failed against minimal stubs before implementation,
then passed. Cases cover active/inactive three-argument forwarding and opaque
RAX/LastError, capture-before-mutation and selected offsets, original C++ and
SEH exceptions, partial and guard input/result reads, exactly one admission,
24 concurrent callers, a real local MinHook enable/disable and exact 16-byte
restoration, a blocked original spanning cutoff, and the serialized JSONL
contract. Each fixture runs in its own process; none attaches to a game.

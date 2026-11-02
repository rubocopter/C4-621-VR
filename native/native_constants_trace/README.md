# Native constant tracing experiment

Standalone diagnostic for the fingerprinted AC6 build documented in
[the native renderer route](../../docs/NATIVE_RENDER_ROUTE.md). This is original
project code. The external projects supplied architectural ideas; no source was
copied from them. It is independent of the accepted stereo producer/presenter.

Five native hooks observe constant uploads, the CPU-to-GPU address resolver,
the native instanced-draw wrapper, descriptor-table writes and root-table
bindings. Fixed storage contains byte snapshots and typed identity-read
validity flags. Callbacks forward once, preserve arguments/opaque RAX and last
error, propagate original exceptions, and restore nested TLS scopes. They do no
file I/O, heap allocation, matrix editing or GPU work. Snapshots can race with
other game threads and are not atomic copies of game state.

The worker validates five prologues and the offline mod host, queues hook
installation/removal, writes the private log after stopping admission, and
checks restoration. DLL, trampolines and storage stay resident. The loader
additionally checks executable path/hash and separately supplied offline
confirmation. Do not overwrite or reload a DLL mapped in the game.

The current source closes snapshot admission after a completed native draw
scope when at least 256 uploads and 1024 events have been recorded. Eight
seconds and F12 remain upper/cancel bounds. First overflow also closes
admission and marks the recording incomplete. An ordinal cutoff excludes
callbacks that straddle closure from candidate joins. A callback still active
after the shutdown timeout invalidates the recording. Guarded/inaccessible
pages are rejected before copying; SEH handles subsequent memory races.

`analyze.py` joins upload identities/bytes, descriptor-array elements, nested
GPUVA resolutions, tables and native draw scopes. It rejects missing snapshots,
observed overlapping writes through draw completion, ambiguous resolvers,
unstable sampled identities, arithmetic errors and incomplete windows.
**Candidates are observations of native recording, not GPU execution proof.**
The tool never marks a captured frame as associated or enables VR metadata.
It does not see arbitrary CPU writes, all descriptor APIs/copies, command-list
generations, indexed draws, queue order, fences or the captured depth resource.

Build and host tests, without starting a game or SteamVR:

```powershell
cmake -S native/native_constants_trace -B out/native-constants-host -A x64
cmake --build out/native-constants-host --config Release
ctest --test-dir out/native-constants-host -C Release --output-on-failure
.venv-analysis/Scripts/python.exe -m unittest discover -s native/native_constants_trace -p test_analyze.py -v
```

Requires the existing `reference/minhook` dependency and x64 MSVC. Analyze a
saved log without attaching to the game:

```powershell
.venv-analysis/Scripts/python.exe native/native_constants_trace/analyze.py <private-log.jsonl> --output <new-private-report.json>
```

The **single unattended runtime test** on 21 October used the earlier frozen
build at `out/native-constants-trace-20261021/build/Release/ac6_render_probe.dll`,
SHA-256 `cc8187aeea1d9f7b090fbe4bd09288c862c5db0f19a06b9810fddfb56cee4358`.
It restored all hooks but filled its buffers. Its 604 candidates are rejected
as a complete recording window. The source here subsequently adds early
closure, cutoff and guarded-page checks; those changes are **host-tested only**.
The mapped DLL and original experimental source/build have been preserved.

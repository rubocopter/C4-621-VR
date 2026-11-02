# Build and validation (developer experiment)

This is source code for a **bounded, experimental Windows x64 diagnostic**, not an installable VR mod. It does **not** include Armored Core VI, third-party tools, binary captures, compatible game files or a public release.

Current evidence and remaining gates: [verified status](docs/STATUS.md). For an
authorized physical trial, use the [current stereo procedure](docs/VR_TRIAL_RUNBOOK.md),
which creates fresh isolated builds and names. The fixed build paths below are
for development only: never run them against a DLL that may be mapped.

## Requirements

- Windows x64, Visual Studio **2022** with MSVC/C++ and Windows SDK, CMake 3.24+ (some native targets require 3.24), Git, PowerShell.
- Python 3.10+ and NumPy for the Python tests. Native-view inspection/tests also use Capstone **5.0.7** and pefile **2024.8.26**. Pillow is used by optional snapshot/preview tools; Matplotlib by optional plots.
- MinHook **v1.3.4** at commit `c3fcafdc10146beb5919319d0683e44e3c30d537`.
- Khronos OpenXR-SDK **release-1.1.63** at commit `f2448a8797c85814aa892efc1ab8707900fbcc78` (headers only; the program currently loads a SteamVR OpenXR loader at runtime).

Both external dependencies are locally installed into ignored `reference/` paths. They are **not vendored**, and the bootstrap refuses to overwrite existing local checkouts or headers. Their licenses remain those of their authors.

## Clean source checkout

From a PowerShell terminal in the root of the repository:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File tools/bootstrap_dependencies.ps1
py -m pip install "numpy>=1.26,<3" "Pillow>=10,<13" "capstone==5.0.7" "pefile==2024.8.26"
py -m unittest discover -s tests -v
py -m compileall -q tools tests
```

The bootstrap needs network access to GitHub. If you already have an unmanaged `reference/minhook`, use your existing dependencies or back them up before bootstrapping. Existing `reference/openxr` headers are left alone. A clean checkout receives the pinned SDK headers.

## Compile the main native components

```powershell
cmake -S native/live_pair_stream -B native/live_pair_stream/build -G "Visual Studio 17 2022" -A x64
cmake --build native/live_pair_stream/build --config Release
cmake -S native/xr_display_probe -B native/xr_display_probe/build -G "Visual Studio 17 2022" -A x64
cmake --build native/xr_display_probe/build --config Release
cmake -S native/camera_probe -B native/camera_probe/build -G "Visual Studio 17 2022" -A x64
cmake --build native/camera_probe/build --config Release
```

`xr_display_probe` includes the D3D12 consumer and GPU capture bridge. `live_pair_stream` builds the producer. Other `native/*` directories are independent, historically useful diagnostics. The CI builds the remaining components too.

Run only deterministic non-game tests in unattended CI. GPU fixtures and integration tests may need real D3D12 hardware, a graphics driver and/or a running game and headset. **A successful CI build is not a PSVR2 acceptance test.**

The static projection contract can be checked without a GPU or OpenXR runtime:

```powershell
cmake -S native/live_stereo -B out/projection-contract -G "Visual Studio 17 2022" -A x64
cmake --build out/projection-contract --config Release --target test_projection
ctest --test-dir out/projection-contract -C Release -R projection_contract_validation --output-on-failure --no-tests=error
```

The full `live_stereo` CTest suite additionally exercises four explicit synthetic
projection models on hardware D3D12 in separate processes. It does not open AC6,
SteamVR or OpenXR. See [projection evidence](PROJECTION_FINDINGS.md#contrato-de-proyección-verificable-en-host--18-10-2026).

## Bounded diagnostics and headset testing

For the opt-in, eight-second projection/provenance diagnostic and its CPU/GPU
fixtures, see [frame provenance](docs/FRAME_PROJECTION_PROVENANCE.md). The
completed v28 observation is documented in
[context provenance](docs/RENDER_CONTEXT_PROVENANCE.md#observación-presencial-v28--pid-21860).
The subsequent [native upload/render investigation](docs/NATIVE_RENDER_ROUTE.md)
supersedes repeating CPU-context coverage as the next prerequisite.
These diagnostics do not enable per-frame projection or require a headset.

Read the [verified status](docs/STATUS.md) and follow the [current isolated stereo procedure](docs/VR_TRIAL_RUNBOOK.md). Historical conditions and measurements remain in [LIVE_STEREO_FINDINGS.md](LIVE_STEREO_FINDINGS.md) and [QUALITY_PACING_FINDINGS.md](QUALITY_PACING_FINDINGS.md). The verified 18 October 2026 reference trial explicitly used `--eye-width 2560 --eye-height 1440 --rotation-limit-deg 90 --headtracking`. The program defaults remain 1280×720 and 25°.

The loader checks a specific executable path/hash and requires a cooperatively launched, separately confirmed single-player/offline session; it is **not** a general-purpose injector. Do not run experiments in official matchmaking or with anti-cheat active. Use a new PID, never reuse mapped DLL names, and never force-unload a mapped diagnostic DLL. Stop with F12 and verify hook restoration. Avoid publishing private game assets or diagnostic captures.

### Select the exact consumer build

`tools/run_live_stereo_trial.py --consumer-exe <path-to-consumer.exe>` selects an
isolated consumer build directly, without copying scripts or placing the binary
in the historical `native/*/build/Release` location. Use a display executable for
`--mode vr` or a monitor executable for `--mode monitor`. For example, after
building the display into a fresh `out/current-display` directory, add:

```powershell
--consumer-exe out/current-display/Release/ac6_xr_display_probe.exe
```

This is an argument to the existing bounded trial command, not a standalone
launch command. The trial report records `consumer_executable` as an absolute
resolved path alongside `consumer_sha256`. An explicit missing path, directory
or non-EXE is rejected before inspecting the game process; it never falls back
to an older build. Omitting the option retains the historical default paths.
All identity, offline confirmation, DLL freshness and GPU shutdown gates remain.

### Working branch

`main` contains the integrated source and technical evidence from PRs #4–#9.
The project owner uses this as the working branch. The prior stacked branches
are no longer required to obtain the validated source. Host-tested optional
intrinsics/FOV code remains disabled in presentation; the bounded headset
acceptance is documented separately in `LIVE_STEREO_FINDINGS.md`.

### Known limits

- Reprojection uses **one** color/depth source, not two geometry renders.
- Camera projection/jitter are not associated with the corresponding source frame.
- The 0.5-game-unit stereo baseline is **not** a measured IPD or a calibrated real-world scale.
- The presentation uses OpenXR quad layers rather than correct per-eye scene projection; no positional headtracking, HUD reconstruction or Sense controller input.
- CPU stage timings include waits and are not GPU timestamps or motion-to-photon latency.

The current build/research status is in the [README](README.md). Historical experiments and their frozen evidence remain in the corresponding `*_FINDINGS.md` files.

## Native constant observer (separate experiment)

[Native constant tracing](native/native_constants_trace/README.md) is an
independent diagnostic, not part of the accepted stereo producer/presenter.
Build and test without launching AC6 or SteamVR:

```powershell
cmake -S native/native_constants_trace -B out/native-constants-host -A x64
cmake --build out/native-constants-host --config Release
ctest --test-dir out/native-constants-host -C Release --output-on-failure
py -m unittest discover -s native/native_constants_trace -p test_analyze.py -v
```

The 21 October retained source passes one native CTest and nine Python
regressions. Its early closure/cutoff/guarded-page changes were host-tested
after the single unattended runtime test; do not describe that new build as
runtime-validated. The frozen tested DLL remains mapped inactive in PID 21860.
Do not rebuild into its directory or load another DLL with the same name in
that process. The analyzer can reuse existing logs without a game session.

## Native views and DLSS host preparation (1 November 2026)

[Native-view findings](docs/NATIVE_VIEW_ROUTE.md) document the exact-build
static verifier and bounded external read-only observer. Neither launches a
game. The live observer requires an explicit current PID and confirmed
offline session; it never installs hooks or enables stereo metadata:

```powershell
py tools/inspect_native_view_route.py --output evidence/<new-static-report>.json
py tools/observe_native_views.py --pid <current-offline-pid> --single-player-confirmed --seconds 3 --output evidence/<new-observation>.jsonl
py tools/observe_native_views.py --pid <current-offline-pid> --single-player-confirmed --resources --seconds 3 --output evidence/<new-resource-observation>.jsonl
```

The optional [resource census](docs/NATIVE_VIEW_RESOURCES.md) checks additional
live instruction anchors and reads bounded target/pipeline references. Pointer
equality is not GPU allocation identity, frame association or DLSS readiness.

[The DLSS contract](native/dlss_contract/README.md) is a standard C++17
admission/history component, without NGX, GPU, OpenXR or game access:

```powershell
cmake -S native/dlss_contract -B out/dlss-contract-host -G 'Visual Studio 17 2022' -A x64
cmake --build out/dlss-contract-host --config Release --parallel 2
ctest --test-dir out/dlss-contract-host -C Release --output-on-failure --no-tests=error
```

The separate [native scope observer](native/native_view_trace/README.md)
hooks only the two verified native view entries in an explicitly authorized
offline diagnostic. Its host tests need neither the game nor a GPU:

```powershell
cmake -S native/native_view_trace -B out/native-view-trace-host -G 'Visual Studio 17 2022' -A x64
cmake --build out/native-view-trace-host --config Release --parallel 2
ctest --test-dir out/native-view-trace-host -C Release --output-on-failure --no-tests=error
py tools/analyze_native_view_trace.py <private-trace.jsonl> --output <new-analysis.json>
```

Use a fresh build directory for a live load; never replace a mapped DLL. The
observer closes after four completed admitted scopes, retains its DLL inactive,
and does not create a second view or submit GPU commands. Its offline analyzer
requires explicit parent tokens and complete restoration/coverage evidence.

The independent [factory observer](native/native_view_factory_trace/README.md)
captures one natural constructor invocation, preserving arguments and return.
Build it separately; its DLL shares the generic diagnostic basename, so a
process with another `ac6_render_probe.dll` already mapped must be closed first:

```powershell
cmake -S native/native_view_factory_trace -B out/native-view-factory-host -G 'Visual Studio 17 2022' -A x64
cmake --build out/native-view-factory-host --config Release --parallel 2
ctest --test-dir out/native-view-factory-host -C Release --output-on-failure --no-tests=error
py tools/analyze_native_view_factory.py <private-factory-trace.jsonl> --census <later-resource-census.jsonl> --output <new-analysis.json>
```

The factory analyzer requires process-creation identity and strictly later
system ticks from the external resource census. Captured parameters contain
process-local pointers and must never be replayed from a saved log. This
observer does not create an additional view.

CI includes these components, native-constant observer/analyzer host tests and
synthetic native-view parser/observer tests. Passing them does not implement
DLSS evaluation, establish frame association or replace a headset trial.

## License

Original project-authored code and tools are under [GPL-3.0-or-later](LICENSE). The [copyright and reuse notice](COPYRIGHT_AND_REUSE.md) excludes game artwork and third-party inputs; each dependency retains its own license. Distribution of covered binaries requires GPL-compliant corresponding-source availability.

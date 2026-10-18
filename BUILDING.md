# Build and validation (developer experiment)

This is source code for a **bounded, experimental Windows x64 diagnostic**, not an installable VR mod. It does **not** include Armored Core VI, third-party tools, binary captures, compatible game files or a public release.

## Requirements

- Windows x64, Visual Studio **2022** with MSVC/C++ and Windows SDK, CMake 3.24+ (some native targets require 3.24), Git, PowerShell.
- Python 3.10+ and NumPy for the Python tests. Pillow is used by optional snapshot/preview tools; Matplotlib by optional plots.
- MinHook **v1.3.4** at commit `c3fcafdc10146beb5919319d0683e44e3c30d537`.
- Khronos OpenXR-SDK **release-1.1.63** at commit `f2448a8797c85814aa892efc1ab8707900fbcc78` (headers only; the program currently loads a SteamVR OpenXR loader at runtime).

Both external dependencies are locally installed into ignored `reference/` paths. They are **not vendored**, and the bootstrap refuses to overwrite existing local checkouts or headers. Their licenses remain those of their authors.

## Clean source checkout

From a PowerShell terminal in the root of the repository:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File tools/bootstrap_dependencies.ps1
py -m pip install "numpy>=1.26,<3" "Pillow>=10,<13"
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

## Testing inside the headset

Read [LIVE_STEREO_FINDINGS.md](LIVE_STEREO_FINDINGS.md) and [QUALITY_PACING_FINDINGS.md](QUALITY_PACING_FINDINGS.md) first. The verified 18 October 2026 reference trial explicitly used `--eye-width 2560 --eye-height 1440 --rotation-limit-deg 90 --headtracking`. The program defaults remain 1280×720 and 25°.

The loader checks a specific executable path/hash and requires a cooperatively launched, separately confirmed single-player/offline session; it is **not** a general-purpose injector. Do not run experiments in official matchmaking or with anti-cheat active. Use a new PID, never reuse mapped DLL names, and never force-unload a mapped diagnostic DLL. Stop with F12 and verify hook restoration. Avoid publishing private game assets or diagnostic captures.

### Known limits

- Reprojection uses **one** color/depth source, not two geometry renders.
- Camera projection/jitter are not associated with the corresponding source frame.
- The 0.5-game-unit stereo baseline is **not** a measured IPD or a calibrated real-world scale.
- The presentation uses OpenXR quad layers rather than correct per-eye scene projection; no positional headtracking, HUD reconstruction or Sense controller input.
- CPU stage timings include waits and are not GPU timestamps or motion-to-photon latency.

The current build/research status is in the [README](README.md). Historical experiments and their frozen evidence remain in the corresponding `*_FINDINGS.md` files.

## License

Original project-authored code and tools are under [GPL-3.0-or-later](LICENSE). The [copyright and reuse notice](COPYRIGHT_AND_REUSE.md) excludes game artwork and third-party inputs; each dependency retains its own license. Distribution of covered binaries requires GPL-compliant corresponding-source availability.

# C4-621 VR · Technical documentation

[← Project landing](../README.md) · [Build & test](../BUILDING.md) · [Research roadmap](../MODDING_PLAN.md) · [License / provenance](../COPYRIGHT_AND_REUSE.md)

This is the documentation entry point for **C4-621 VR**, an experimental Armored Core VI D3D12/OpenXR PCVR research project. It separates observed headset behavior, host-tested implementation details, and open engineering questions.

> **Current reference (18 October 2026):** The latest bounded PSVR2 regression (PR #8, now in `main`) was accepted at **2560×1440 per eye / 90°**: live fusible depth, no doubled image or observed freezes, and correct head turning between a near edge and distant background. This does not establish metric geometry, latency or prolonged comfort. Start with [verified status](STATUS.md); earlier reports retain historical results.

## Choose a reading path

| Your question | Read these first |
| --- | --- |
| *What has actually been seen in the headset?* | [Verified status](STATUS.md) → [Live stereo trials](../LIVE_STEREO_FINDINGS.md) → [quality and pacing measurements](../QUALITY_PACING_FINDINGS.md) |
| *How do I repeat the accepted bounded trial?* | [Current stereo procedure](VR_TRIAL_RUNBOOK.md), with isolated binaries and explicit consumer selection |
| *How does the D3D12 capture work?* | [Render capture](../RENDER_CAPTURE_FINDINGS.md) → [color/depth pairing](../FRAME_PAIR_FINDINGS.md) → [continuous GPU stream](../LIVE_STEREO_FINDINGS.md) |
| *Which submission/recording supplied a captured pair?* | [Optional frame provenance trace and missing projection binding](FRAME_PROJECTION_PROVENANCE.md) |
| *What if the pair expires during the last eye copy?* | [Final source check before frame submission](../LIVE_STEREO_FINDINGS.md#frescura-al-enviar-el-par--corrección-host-del-18-10-2026) |
| *Where does stereo disparity come from?* | [Projection and offline stereo](../PROJECTION_FINDINGS.md) → [continuous stereo](../LIVE_STEREO_FINDINGS.md) |
| *Can the consumer use different image intrinsics per eye?* | [Optional parallel-eye intrinsics: host contract and tests](EYE_PROJECTION_CONTRACT.md) |
| *How do CPU and GPU choose the nearest depth sample?* | [Depth ownership, numerical limits and regression fixtures](DEPTH_REFERENCE_PRECISION.md) |
| *How is head movement connected to the game?* | [Camera discovery](../CAMERA_FINDINGS.md) → [current combined stereo procedure](VR_TRIAL_RUNBOOK.md); [initial display history](../VR_DISPLAY_FINDINGS.md) |
| *Can I build and test it?* | [Reproducible Windows build](../BUILDING.md) → [CI workflow](../.github/workflows/ci.yml) → [native test sources](../native/) |
| *What should be implemented next?* | [Roadmap and validation gates](../MODDING_PLAN.md) → [quality/pacing limits](../QUALITY_PACING_FINDINGS.md) |
| *Where did an assumption or failure originate?* | [Research chronology](../MODLOG.md) and individual findings documents |

## Architecture at a glance

| Component / directory | Responsibility |
| --- | --- |
| [`native/camera_probe/`](../native/camera_probe/) | Observes and applies the calibrated orientation delta to the game camera within a bounded hook window |
| [`native/render_probe/`](../native/render_probe/) and [`native/frame_pair_probe/`](../native/frame_pair_probe/) | Identifies D3D12 resources and tracks coherent color/depth captures |
| [`native/live_pair_stream/`](../native/live_pair_stream/) | Shares paired color/depth frames using a bounded D3D12 GPU ring and fences; never waits for the consumer inside the game's graphics-submit hook |
| [`native/live_stereo/`](../native/live_stereo/) | Consumer-side GPU reconstruction, stereo output and transport diagnostics |
| [`native/xr_display_probe/`](../native/xr_display_probe/) | OpenXR D3D12 session, left/right eye presentation and headset orientation publication |
| [`native/stereo_reprojection/`](../native/stereo_reprojection/) and [`tools/stereo_reprojection.py`](../tools/stereo_reprojection.py) | Offline reconstruction and reference/validation paths |
| [`native/gpu_capture_bridge/`](../native/gpu_capture_bridge/) | Earlier GPU WGC/window-image transport experiment; separate from current paired D3D12 stream |
| [`tools/`](../tools/) and [`tests/`](../tests/) | Offline sampling, evidence analysis, trial orchestration and regression fixtures |

## Evidence and assumptions

### Observed in the PS VR2 headset

- **V7:** live depth-reprojected stereo changed as the game camera moved with a standard controller; the operator confirmed visible depth and no doubled image.
- **V8:** live stereo and calibrated head orientation ran together in an OpenXR session. This is **3DoF orientation**, not room-scale 6DoF.
- **V11:** explicit **2560×1440 output per eye, 90° rotation limit**; short-run operator feedback reported improved clarity without noticeable stutter.
- **PR #4 and PR #8:** subsequent bounded regressions accepted at that profile; PR #8 specifically compared a near edge with distant background. Forced last-eye expiry remains host-tested, not physically induced.

See the exact conditions and diagnostic counters in [LIVE_STEREO_FINDINGS.md](../LIVE_STEREO_FINDINGS.md) and [QUALITY_PACING_FINDINGS.md](../QUALITY_PACING_FINDINGS.md). The application's default output remains **1280×720 / 25°**.

### Not yet established

- A source-frame-matched projection matrix, including that frame's jitter.
- Correct headset eye projection/asymmetry, real-world metric scale or a validated IPD mapping. Current baseline is experimental game units.
- Hidden-surface recovery, stable HUD presentation, real independent geometry per eye, or position tracking.
- Motion-to-photon latency and sustained comfort under combat, boost, lock-on and scene transitions.
- SteamVR/PSVR2 Sense-to-game controls, general headset compatibility or a public installer.

**Important interpretation:** Presenting two different reprojected eye images does not mean the game engine renders geometry twice. The display currently uses OpenXR eye-restricted quad layers, and the quality/pacing logs contain CPU timings with waits—not direct GPU timestamps or latency measurements.

## Historical research archive

These reports are retained as a technical audit trail. A claim marked “pending” at the time of an older trial should not be read as a current regression. Use the [current roadmap](../MODDING_PLAN.md) for outstanding gates; use the [current procedure](VR_TRIAL_RUNBOOK.md) instead of archived launch commands.

| Document | Historical emphasis |
| --- | --- |
| [Camera findings](../CAMERA_FINDINGS.md) | Camera signatures, stable hook location, rotation/calibration |
| [Render capture](../RENDER_CAPTURE_FINDINGS.md) | Identifying D3D12 render resources and safe readback/copy boundaries |
| [Color/depth pair](../FRAME_PAIR_FINDINGS.md) | Source-frame identity and synchronization |
| [Projection findings](../PROJECTION_FINDINGS.md) | Observed FOV/depth model and first offline stereo reference |
| [VR display findings](../VR_DISPLAY_FINDINGS.md) | Initial headset display/window transport and early orientation proof |
| [Headtracking runbook](../HEADTRACKING_RUNBOOK.md) | Time-limited historical tracking procedure |
| [Live stereo findings](../LIVE_STEREO_FINDINGS.md) | Continuous headset stream, queue diagnostics, pose, recovery |
| [Quality/pacing findings](../QUALITY_PACING_FINDINGS.md) | Configurable presentation quality and latest reference headset run |
| [Research log](../MODLOG.md) | Chronological experiments, failures, changes and operator evidence |
| [D3D12 stream design specification](superpowers/specs/2026-10-16-live-stereo.md) | Historical scoped design |
| [D3D12 stream implementation plan](superpowers/plans/2026-10-16-live-stereo.md) | Historical implementation checklist |

## Development and responsible testing

Use [BUILDING.md](../BUILDING.md) for pinned dependencies, isolated Windows builds, automated tests, executable fingerprint checks and safe runtime constraints. **CI success is not a VR hardware certification.** The runtime remains sensitive to unsupported game builds and queue changes; its existing refusal/cleanup gates must not be bypassed to obtain screenshots.

Do not publish proprietary game assets or private game snapshots. Use a legally owned copy of the game, confirmed offline/single-player mode and no active anti-cheat or official matchmaking.

Original authored code is [GPL-3.0-or-later](../LICENSE). Third-party game content, SDKs and artwork retain their original rights; see [COPYRIGHT_AND_REUSE.md](../COPYRIGHT_AND_REUSE.md).

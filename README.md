<p align="center">
  <img src="assets/ac6_banner_vr.png" alt="C4-621 VR — experimental Armored Core VI PCVR project" width="100%">
</p>

<h1 align="center">C4-621 VR</h1>

<p align="center">
  <strong>Bring Rubicon into the headset.</strong><br>
  An experimental path to playing <em>Armored Core VI: Fires of Rubicon</em> in PC virtual reality.<br>
  Live D3D12 capture. Depth-reprojected stereo. OpenXR presentation. Head-tracked camera.
</p>

<p align="center">
  <a href="https://github.com/rubocopter/C4-621-VR/actions/workflows/ci.yml"><img alt="Windows source validation" src="https://github.com/rubocopter/C4-621-VR/actions/workflows/ci.yml/badge.svg"></a>
  <img alt="Status: research prototype" src="https://img.shields.io/badge/status-research%20prototype-F0AD4E?style=flat-square">
  <img alt="Platform: Windows PC" src="https://img.shields.io/badge/platform-Windows%20PC-0078D6?style=flat-square">
  <img alt="VR: OpenXR" src="https://img.shields.io/badge/VR-OpenXR-3955A3?style=flat-square">
  <a href="LICENSE"><img alt="License: GPL v3 or later for original code" src="https://img.shields.io/badge/code-GPLv3%2B-3A6B52?style=flat-square"></a>
</p>

<p align="center">
  <a href="#the-idea">The idea</a> ·
  <a href="#what-already-works">Progress</a> ·
  <a href="#under-the-hood">Technology</a> ·
  <a href="#the-road-ahead">Roadmap</a> ·
  <a href="#technical-documentation">Technical docs</a>
</p>

> [!IMPORTANT]
> **Research prototype — not a playable VR release.** There is no downloadable player build or one-click installer. The current results come from short, controlled experiments on **PS VR2 connected to a Windows PC**. A passing CI build does not establish headset compatibility, comfort, or full-game support.

## The idea

**What if you could look around Rubicon from inside your AC?**

C4-621 VR investigates a native PCVR route for Armored Core VI without treating the desktop image as the finished solution. Instead of relying solely on a flat-screen mirror, the prototype captures the game's live **color and depth**, constructs different images for each eye on the GPU, and presents them through **OpenXR**. Head orientation feeds back into the game's camera.

The goal is a convincing, responsive cockpit-scale experience. Today it is a working technical foundation—not a finished VR conversion.

## What already works

| Milestone | Evidence from the tested PS VR2 setup |
| --- | --- |
| **Live in-headset stereo** | The running game's view updates with controller movement, with visible depth and no double image reported in the bounded V7 trial. |
| **Head-controlled view** | OpenXR head orientation and the game's camera were exercised together with the live stereo stream in V8 and subsequent trials. **Rotational tracking only (3DoF).** |
| **Higher-quality output** | The V11 experiment used **2560 × 1440 output pixels per eye** and a **90° head-rotation limit**. The operator reported a clearer image with no noticeable stutter during that short trial. |
| **Controlled GPU transport** | A D3D12 color/depth stream travels through shared GPU resources and fences to an independent stereo consumer, without CPU pixel readback in the normal path. |
| **Safe experimental shutdown** | Bounded trial runs, frame-expiry handling, explicit stop signals, and hook restoration are implemented and tested. |

<sub>Reference: 18 October 2026. The recorded 1440p/90° setup is an <strong>explicit test configuration</strong>, not the program default (1280×720 and 25°). The subjective headset result is not a measured guarantee of smooth frame pacing or motion-to-photon latency. See <a href="QUALITY_PACING_FINDINGS.md">quality and pacing evidence</a> and <a href="LIVE_STEREO_FINDINGS.md">live stereo findings</a>.</sub>

## Under the hood

A conventional game render becomes two depth-derived views, displayed in a separate OpenXR session. No extracted game models or double geometry rendering are needed for this experimental path.

```text
     ARMORED CORE VI (Direct3D 12)
                  |
          Live color + depth
                  |
      GPU-only shared frame ring
       (resources + two fences)
                  |
        Independent D3D12 GPU
        stereo reprojection
             /         \
         Left eye     Right eye
             \         /
         OpenXR stereo display
                  |
           PS VR2 headset

       OpenXR head orientation
                  |
        Calibrated camera hook
                  |
            Game view (3DoF)
```

**What makes this approach different:** the two views contain depth-dependent parallax from a real, changing game frame, rather than simply duplicating a flat desktop view. The producer avoids waiting for the stereo consumer inside the game's graphics submission callback, and stale or invalid frames are rejected rather than blindly presented.

**What it does not do:** this is **reprojection from one original render**, *not* independent geometry rendering for each eye. Stereo scale and interpupillary distance are not yet metrically calibrated, the projection/jitter values are not paired with every source frame, and OpenXR's asymmetric per-eye projections are not yet used for true scene-space VR presentation. The current output uses eye-specific **quad layers**.

[Explore the architecture and evidence →](docs/INDEX.md)

## The road ahead

| Proven in limited trials | Still required for a comfortable VR experience |
| --- | --- |
| Live stereo, orientation-only camera tracking, independent GPU transport | **Correct world scale / IPD** and per-eye projection |
| Configurable stereo output and rotation limit | **Per-frame camera matrices and jitter** tied to captured color/depth |
| Bounded runtime and cleanup tests | **Stable pacing and measured latency** across longer sessions |
| Standard gamepad control in the game | **6DoF, HUD treatment, occlusion handling**, and gameplay comfort |
| Tested scenes on one PS VR2 setup | **Boost, lock-on, transitions and broader mission coverage** |
| Reproducible Windows source builds | A **safe, supported player installer or release** |

PS VR2 Sense input is **not implemented**. The current experiments use a conventional controller for game input. The 3DoF proof of concept should not be mistaken for room-scale tracking or a finished mech cockpit.

## Technical documentation

Want to inspect the actual engineering rather than just the project overview?

| Start here | What you'll find |
| --- | --- |
| **[Technical documentation hub](docs/INDEX.md)** | Architecture, investigation timeline, all primary reports, and a reading path by topic |
| [Live stereo implementation](LIVE_STEREO_FINDINGS.md) | D3D12 producer/consumer, frame ring, GPU reprojection, headset tests and recovery |
| [Quality and frame pacing](QUALITY_PACING_FINDINGS.md) | Output profiles, 1440p/90° experiment, timings and limitations |
| [Camera and tracking](CAMERA_FINDINGS.md) | Camera path, hook observations and calibration |
| [Projection and depth](PROJECTION_FINDINGS.md) | Source projection, reconstructed geometry, stereo limits and calibration work |
| [Build and validation](BUILDING.md) | Pinned dependencies, Windows builds, offline tests and safe trial boundaries |
| [Research roadmap](MODDING_PLAN.md) | Completed technical gates, outstanding risks and future direction |

<details>
<summary><strong>For developers: source and validation</strong></summary>

- Native components: [`native/`](native/) — D3D12 producer, depth reprojection, OpenXR display and camera experiments.
- Research and analysis tools: [`tools/`](tools/).
- Regression tests: [`tests/`](tests/) and [Windows GitHub Actions](.github/workflows/ci.yml).
- Historical chronology: [`MODLOG.md`](MODLOG.md); some older notes describe pending steps that have since been completed. Start with the current live-stereo and quality reports instead.
- The source can be built for research using [`BUILDING.md`](BUILDING.md). Live experiments require a specifically fingerprinted game build and an explicitly confirmed offline/single-player environment without active anti-cheat. **Do not test in official online matchmaking.**

</details>

## Follow the project

This repository tracks the technical work in public, including limitations and failed experiments—not just successful screenshots. You can [browse the history](MODLOG.md), inspect [source validation](https://github.com/rubocopter/C4-621-VR/actions), or [open an issue](https://github.com/rubocopter/C4-621-VR/issues) with a reproducible technical finding. Please do not upload proprietary game files, private captures or crash dumps containing personal data.

If you want to support development, you can [do so on Ko-fi](https://ko-fi.com/onitaku).

## License and disclaimer

Original project-authored code and research tools are released under **[GPL-3.0-or-later](LICENSE)**. See [copyright, reuse and third-party scope](COPYRIGHT_AND_REUSE.md): this grant does **not** relicense Armored Core VI, its artwork, third-party SDKs, or other external content.

**Unofficial fan research project.** Not affiliated with or endorsed by FromSoftware, Bandai Namco Entertainment, Valve or Sony Interactive Entertainment. Armored Core VI is required, is **not included**, and is not provided for download here.

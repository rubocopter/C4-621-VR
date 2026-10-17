<p align="center">
  <img src="assets/ac6_banner_vr.png" alt="Armored Core VI: C4-621 VR" width="100%">
</p>

<p align="center">
  <img alt="Status: experimental" src="https://img.shields.io/badge/status-experimental-orange?style=flat-square">
  <img alt="Platform: Windows" src="https://img.shields.io/badge/platform-Windows-blue?style=flat-square">
  <img alt="Runtime: SteamVR / OpenXR" src="https://img.shields.io/badge/VR-SteamVR%20%2F%20OpenXR-1a9fff?style=flat-square">
</p>

<p align="center">
  <a href="https://ko-fi.com/onitaku"><img alt="Support development on Ko-fi" src="https://ko-fi.com/img/githubbutton_sm.svg"></a>
</p>

<p align="center">
  <b>Armored Core VI, explored in virtual reality.</b><br>
  An experimental community project investigating a path to PC VR for C4-621.
</p>

## Current progress

A proof of concept has displayed the game in a PS VR2 headset and followed the player's head orientation. The headset view and calibrated horizontal and vertical tracking were confirmed during a hands-on trial.

A GPU texture-sharing capture path has been tested with the live game, and paired color and depth have been captured on its D3D12 graphics queue. See the [render and capture research notes](RENDER_CAPTURE_FINDINGS.md) for the earlier experiments.

The observed perspective matrix allows depth reconstruction in game units. Separate eye views were first generated offline on CPU and D3D12; the resulting **static stereo scene was displayed in PS VR2**, where the user confirmed perceived depth without double vision. See the [projection and stereo findings](PROJECTION_FINDINGS.md).

A **continuous GPU color/depth stream** has now also been demonstrated with the running game. In a six-second **monitor-only** trial, the producer published 721 paired frames (dropping 120 without blocking the game), and the independent D3D12 consumer produced 414 stereo updates with distinct left/right images and changing scene content. **Continuous stereo has not yet been tested in the headset.** See the [live stereo findings](LIVE_STEREO_FINDINGS.md).

This remains an early experiment: the confirmed headset gameplay view uses a monoscopic image with restricted head rotation; the confirmed stereo headset trial uses a fixed capture. Metric scale, per-eye OpenXR projections, continuous binocular headset presentation, and combined stereo/head tracking still need validation. It is not a complete VR conversion or a public release.

## Project

The work is exploring whether Armored Core VI can become a comfortable, stereoscopic PC VR experience. The next milestone is a bounded **live stereo headset trial**, followed by validation with head orientation, correct eye projections, and scale.

Follow along as the project develops, and [support the work on Ko-fi](https://ko-fi.com/onitaku) if you would like to help.

---

An unofficial fan project, not affiliated with or endorsed by Bandai Namco Entertainment or FromSoftware. Armored Core VI is required and is not included.

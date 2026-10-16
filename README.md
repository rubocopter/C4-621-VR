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

A GPU texture-sharing capture path has also been tested with the live game, and a D3D12 depth snapshot has been recovered. Depth is not yet used for live stereo presentation. See the [render and capture research notes](RENDER_CAPTURE_FINDINGS.md) for verification and limitations.

Paired color and depth have been captured together on the game's graphics queue. The observed perspective matrix now allows depth reconstruction in game units, and separate eye views have been generated offline on CPU and D3D12. Metric scale and live binocular presentation remain pending; see the [projection and stereo findings](PROJECTION_FINDINGS.md).

This is an early, limited experiment: the image is monoscopic and head rotation is restricted. It is not a complete VR conversion or a public release.

## Project

The work is exploring whether Armored Core VI can become a comfortable, stereoscopic PC VR experience. The next major milestone is rendering the game world independently for each eye.

Follow along as the project develops, and [support the work on Ko-fi](https://ko-fi.com/onitaku) if you would like to help.

---

An unofficial fan project, not affiliated with or endorsed by Bandai Namco Entertainment or FromSoftware. Armored Core VI is required and is not included.

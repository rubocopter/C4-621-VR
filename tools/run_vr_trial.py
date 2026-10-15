"""Run the bounded, calibrated monoscopic AC6/PSVR2 trial.

Requires a fresh cooperative single-player game session. Starts display output,
waits for visible tracked head poses, then loads the 15-second camera probe.
No game-file edits. Controller-input focus is not required for VIEW-space poses.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess
import sys
import time

from observe_camera import EXPECTED_EXE, EXPECTED_SHA, process_metadata


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--pid", type=int, required=True)
    parser.add_argument("--single-player-confirmed", action="store_true", required=True)
    parser.add_argument("--tag", required=True, help="New evidence name: letters, numbers, hyphen or underscore")
    parser.add_argument("--gpu-capture", action="store_true", help="Use GPU shared textures instead of CPU pixel readback")
    args = parser.parse_args()
    if args.pid <= 0 or not re.fullmatch(r"[A-Za-z0-9_-]{1,64}", args.tag):
        parser.error("Invalid PID or evidence tag")
    root = Path(__file__).resolve().parents[1]
    exe = root / "native/xr_display_probe/build/Release/ac6_xr_display_probe.exe"
    dll = root / "native/camera_probe/build/Release/ac6_headtracking_calibrated_probe.dll"
    if not exe.is_file() or not dll.is_file():
        raise RuntimeError("Build the display app and calibrated camera probe first")
    meta = process_metadata(args.pid)
    modules = {m.lower() for m in meta["Modules"]}
    if Path(meta["Path"]).resolve() != EXPECTED_EXE.resolve():
        raise RuntimeError("Wrong game executable")
    with EXPECTED_EXE.open("rb") as stream:
        if hashlib.file_digest(stream, "sha256").hexdigest() != EXPECTED_SHA:
            raise RuntimeError("Game build differs from the researched version")
    if "ac6_coop.dll" not in modules or any("easyanticheat" in m or m.startswith("eac_") for m in modules):
        raise RuntimeError("Requires cooperative single-player host without EAC")
    if dll.name.lower() in modules:
        raise RuntimeError("Calibrated probe already mapped; use a fresh game session, do not force-unload it")
    folder = root / "evidence"
    display = folder / f"{args.tag}.jsonl"
    console = folder / f"{args.tag}-console.log"
    result = folder / f"{args.tag}-result.json"
    native = folder / f"{args.tag}-native.json"
    capture = folder / f"{args.tag}-active.png"
    native_log = dll.with_name(f"headtracking_calibrated_probe_{args.pid}.jsonl")
    outputs = [display, display.with_suffix(".bmp"), console, result, native, capture,
               capture.with_stem(capture.stem + "_later"), native_log]
    if any(p.exists() for p in outputs):
        raise RuntimeError("Evidence already exists; preserve it and select a new tag/session")
    folder.mkdir(parents=True, exist_ok=True)
    report = {"pid": args.pid, "game_files_changed": False, "game_geometry_stereo": False,
              "coordinate_conversion": "reflect_z", "rotation_limit_degrees": 25,
              "capture_transport": "gpu_shared_texture" if args.gpu_capture else "cpu_readback",
              "display_sha256": hashlib.sha256(exe.read_bytes()).hexdigest(),
              "dll_sha256": hashlib.sha256(dll.read_bytes()).hexdigest()}
    with console.open("x", encoding="utf-8") as stream:
        display_command = [str(exe), "--seconds", "27", "--game-pid", str(args.pid), "--headtracking", "--output", str(display)]
        if args.gpu_capture:
            display_command.append("--gpu-capture")
        app = subprocess.Popen(display_command, cwd=root,
                               stdout=stream, stderr=subprocess.STDOUT,
                               creationflags=subprocess.CREATE_NO_WINDOW)
        try:
            deadline = time.monotonic() + 35
            focused = False
            tracking_ready = False
            while app.poll() is None and time.monotonic() < deadline:
                if display.exists():
                    rows = [json.loads(s) for s in display.read_text(encoding="utf-8").splitlines() if s.endswith("}")]
                    focused = any(r.get("kind") == "session_state" and r.get("state") == 5 for r in rows)
                    progress = [r for r in rows if r.get("kind") == "progress"]
                    tracking_ready = bool(progress and progress[-1].get("tracking_ready"))
                    if tracking_ready:
                        break
                time.sleep(0.1)
            report["focused_before_hook"] = focused
            report["tracking_ready_before_hook"] = tracking_ready
            if tracking_ready:
                print("OpenXR visible with tracked head pose: starting the bounded camera trial.", flush=True)
                loaded = subprocess.run([sys.executable, "-B", str(root / "tools/load_camera_probe.py"),
                    "--pid", str(args.pid), "--single-player-confirmed", "--dll", str(dll),
                    "--capture", str(capture), "--output", str(native)], cwd=root,
                    capture_output=True, text=True, timeout=45, creationflags=subprocess.CREATE_NO_WINDOW)
                report.update(loader_exit_code=loaded.returncode, loader_stdout=loaded.stdout, loader_stderr=loaded.stderr)
            report["display_exit_code"] = app.wait(timeout=90)
        except Exception as error:
            report["error"] = str(error)
            raise
        finally:
            if app.poll() is None:
                try:
                    app.wait(timeout=65)
                except subprocess.TimeoutExpired:
                    # Only this script's standalone child, never the game.
                    app.terminate()
                    app.wait(timeout=5)
                    report["display_child_terminated_after_timeout"] = True
            result.write_text(json.dumps(report, indent=2), encoding="utf-8")
    print(json.dumps({k: v for k, v in report.items() if not k.startswith("loader_std")}, indent=2))
    return 0 if report.get("loader_exit_code") == 0 and report.get("display_exit_code") == 0 else 2


if __name__ == "__main__":
    raise SystemExit(main())

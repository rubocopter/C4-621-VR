"""Load a bounded diagnostic probe into an explicit cooperative single-player PID.

Uses ordinary Windows LoadLibraryW; no anti-cheat/DRM changes or game-file edits.
The fixed-yaw probe runs for about 8 seconds; head tracking runs for about 20
(15 seconds active). Both auto-disable; F12 cancels earlier. The DLL remains
mapped inert. Never unload it while game threads may have used its detour.
The D3D12 render observer runs for about 8 seconds and verifies its own hooks.
"""

import argparse
import ctypes as C
from ctypes import wintypes as W
import hashlib
import json
from pathlib import Path
import subprocess
import time

from observe_camera import EXPECTED_EXE, EXPECTED_SHA, process_metadata


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--pid", type=int, required=True)
    parser.add_argument("--single-player-confirmed", action="store_true", required=True)
    parser.add_argument("--dll", type=Path, required=True)
    parser.add_argument("--capture", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    dll_path = args.dll.resolve(strict=True)
    meta = process_metadata(args.pid)
    if Path(meta["Path"]).resolve() != EXPECTED_EXE.resolve():
        raise RuntimeError("Wrong process executable")
    mods = {m.lower() for m in meta["Modules"]}
    if "ac6_coop.dll" not in mods or any("easyanticheat" in m or m.startswith("eac_") for m in mods):
        raise RuntimeError("Requires the verified cooperative single-player host")
    if dll_path.name.lower() in mods:
        raise RuntimeError("Probe DLL is already mapped; do not reload it")
    with EXPECTED_EXE.open("rb") as f:
        if hashlib.file_digest(f, "sha256").hexdigest() != EXPECTED_SHA:
            raise RuntimeError("Different game build")
    is_tracking = dll_path.name.startswith("ac6_headtracking")
    is_render = dll_path.name in ("ac6_render_probe.dll", "ac6_depth_capture_probe.dll", "ac6_depth_batch_capture_probe.dll")
    prefix = "headtracking_probe" if is_tracking else "camera_probe"
    if dll_path.name == "ac6_headtracking_display_probe.dll":
        prefix = "headtracking_display_probe"
    elif dll_path.name == "ac6_headtracking_calibrated_probe.dll":
        prefix = "headtracking_calibrated_probe"
    elif dll_path.name == "ac6_depth_batch_capture_probe.dll":
        prefix = "depth_batch_capture_probe"
    elif dll_path.name == "ac6_depth_capture_probe.dll":
        prefix = "depth_capture_probe"
    elif is_render:
        prefix = "render_probe"
    log_path = dll_path.with_name(f"{prefix}_{args.pid}.jsonl")
    later_capture = args.capture.with_stem(args.capture.stem + "_later")
    if any(p.exists() for p in (args.output, args.capture, later_capture, log_path)):
        raise RuntimeError("Choose new evidence paths; probe log must not already exist")
    k = C.WinDLL("kernel32", use_last_error=True)
    signatures = {
        "GetModuleHandleW": (H := C.c_void_p, [W.LPCWSTR]),
        "GetProcAddress": (H, [H, C.c_char_p]),
        "GetModuleHandleExW": (W.BOOL, [W.DWORD, H, C.POINTER(H)]),
        "GetModuleFileNameW": (W.DWORD, [H, W.LPWSTR, W.DWORD]),
        "OpenProcess": (H, [W.DWORD, W.BOOL, W.DWORD]),
        "CloseHandle": (W.BOOL, [H]),
        "VirtualAllocEx": (H, [H, H, C.c_size_t, W.DWORD, W.DWORD]),
        "VirtualFreeEx": (W.BOOL, [H, H, C.c_size_t, W.DWORD]),
        "WriteProcessMemory": (W.BOOL, [H, H, H, C.c_size_t, C.POINTER(C.c_size_t)]),
        "ReadProcessMemory": (W.BOOL, [H, H, H, C.c_size_t, C.POINTER(C.c_size_t)]),
        "CreateRemoteThread": (H, [H, H, C.c_size_t, H, H, W.DWORD, C.POINTER(W.DWORD)]),
        "WaitForSingleObject": (W.DWORD, [H, W.DWORD]),
    }
    for name, (result, arguments) in signatures.items():
        function = getattr(k, name)
        function.restype, function.argtypes = result, arguments
    local_function = k.GetProcAddress(k.GetModuleHandleW("kernel32.dll"), b"LoadLibraryW")
    owner = H()
    if not local_function or not k.GetModuleHandleExW(6, local_function, C.byref(owner)):
        raise C.WinError(C.get_last_error())
    owner_name = C.create_unicode_buffer(32768)
    if not k.GetModuleFileNameW(owner, owner_name, len(owner_name)):
        raise C.WinError(C.get_last_error())
    module_name = Path(owner_name.value).name.lower()
    command = (f"$p=Get-Process -Id {args.pid} -ErrorAction Stop; "
               "@($p.Modules | ForEach-Object {[pscustomobject]@{Name=$_.ModuleName;Base=$_.BaseAddress.ToInt64()}}) | ConvertTo-Json -Compress")
    result = subprocess.run(["powershell.exe", "-NoProfile", "-NonInteractive", "-Command", command],
                            capture_output=True, check=True, text=True)
    modules = json.loads(result.stdout)
    remote_owner = next(m["Base"] for m in modules if m["Name"].lower() == module_name)
    remote_load = remote_owner + local_function - owner.value
    process = k.OpenProcess(0x43A, False, args.pid)
    if not process:
        raise C.WinError(C.get_last_error())
    allocation, thread, completed = None, None, False
    report = {"pid": args.pid, "dll": str(dll_path), "dll_sha256": hashlib.sha256(dll_path.read_bytes()).hexdigest(),
              "native_log": str(log_path), "method": "Windows LoadLibraryW in explicit mod process", "game_files_changed": False}
    try:
        buffer = C.create_string_buffer(16)
        count = C.c_size_t()
        if not k.ReadProcessMemory(process, meta["Base"] + 0x6BFE50, buffer, 16, C.byref(count)):
            raise C.WinError(C.get_last_error())
        expected = bytes.fromhex("48 89 5c 24 08 57 48 83 ec 20 48 8b f9 48 8b da")
        if count.value != 16 or buffer.raw != expected:
            raise RuntimeError("Unexpected camera-copy prologue; refuse to load probe")
        path_bytes = (str(dll_path) + "\0").encode("utf-16-le")
        allocation = k.VirtualAllocEx(process, None, len(path_bytes), 0x3000, 0x04)
        if not allocation:
            raise C.WinError(C.get_last_error())
        path_buffer = C.create_string_buffer(path_bytes)
        if not k.WriteProcessMemory(process, allocation, path_buffer, len(path_bytes), C.byref(count)) or count.value != len(path_bytes):
            raise C.WinError(C.get_last_error())
        tid = W.DWORD()
        thread = k.CreateRemoteThread(process, None, 0, remote_load, allocation, 0, C.byref(tid))
        if not thread:
            raise C.WinError(C.get_last_error())
        wait = k.WaitForSingleObject(thread, 10000)
        if wait != 0:
            raise RuntimeError(f"Loader thread not completed (wait={wait}); preserve its argument allocation")
        completed = True
        capture_done = False
        deadline = time.monotonic() + (30 if is_tracking else 14)
        offset_seen_at = None
        later_done = False
        records = []
        while time.monotonic() < deadline:
            if log_path.exists():
                try:
                    text = log_path.read_text(encoding="utf-8")
                except PermissionError:
                    time.sleep(0.05)
                    continue
                records = [json.loads(line) for line in text.splitlines() if line.endswith("}")]
                phases = {r["phase"] for r in records}
                if "offset_started" in phases and not capture_done:
                    offset_seen_at = time.monotonic()
                    args.capture.parent.mkdir(parents=True, exist_ok=True)
                    capture = subprocess.run(["um.cmd", "win", "shot", "--exe", "armoredcore6.exe", str(args.capture), "--scale", "0.33"],
                                             capture_output=True, text=True, timeout=10, creationflags=subprocess.CREATE_NO_WINDOW)
                    report["capture"] = {"exit_code": capture.returncode, "output": capture.stdout, "error": capture.stderr}
                    capture_done = True
                if is_tracking and offset_seen_at and not later_done and time.monotonic() - offset_seen_at >= 4:
                    capture = subprocess.run(["um.cmd", "win", "shot", "--exe", "armoredcore6.exe", str(later_capture), "--scale", "0.33"],
                                             capture_output=True, text=True, timeout=10, creationflags=subprocess.CREATE_NO_WINDOW)
                    report["later_capture"] = {"exit_code": capture.returncode, "output": capture.stdout, "error": capture.stderr}
                    later_done = True
                if "disabled" in phases or any("failed" in p or "rejected" in p for p in phases):
                    break
            time.sleep(0.05)
        report["events"] = records
        report["auto_disabled"] = any(r["phase"] == "disabled" for r in records)
        if is_render:
            report["render_hooks_restored"] = any(r.get("hooks_restored") for r in records if r["phase"] == "restoration")
        if dll_path.name in ("ac6_depth_capture_probe.dll", "ac6_depth_batch_capture_probe.dll"):
            report["depth_capture_written"] = any(r.get("written") for r in records if r["phase"] == "depth_capture")
        if not k.ReadProcessMemory(process, meta["Base"] + 0x6BFE50, buffer, 16, C.byref(count)):
            raise C.WinError(C.get_last_error())
        report["prologue_restored"] = buffer.raw == expected
        report["dll_remains_mapped_inactive"] = True
    finally:
        if thread:
            k.CloseHandle(thread)
        if allocation and completed:
            k.VirtualFreeEx(process, allocation, 0, 0x8000)
        k.CloseHandle(process)
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(report, indent=2))
    return 0 if report.get("auto_disabled") and report.get("prologue_restored") and (not is_render or report.get("render_hooks_restored")) and report.get("depth_capture_written", True) else 2


if __name__ == "__main__":
    raise SystemExit(main())

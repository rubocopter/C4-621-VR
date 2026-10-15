"""Temporary camera-mirror experiment for the fingerprinted single-player AC6.

Default is read-only. --apply holds a bounded orientation offset for <=4 s,
without changing position, game files, hooks or page protections. It restores
the saved orientation only if the mirror still contains our last write.
This tests whether this mirror affects the displayed camera, not VR itself.
"""

import argparse
import concurrent.futures
import ctypes as C
from ctypes import wintypes as W
import hashlib
import json
import math
from pathlib import Path
import struct
import subprocess
import sys
import time

from camera_math import compose_head_orientation
from observe_camera import EXPECTED_EXE, EXPECTED_SHA, GLOBAL_RVA, matrix_metrics, process_metadata


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--pid", type=int, required=True)
    parser.add_argument("--single-player-confirmed", action="store_true", required=True)
    parser.add_argument("--yaw", type=float, default=10)
    parser.add_argument("--seconds", type=float, default=3)
    parser.add_argument("--apply", action="store_true")
    parser.add_argument("--stage", choices=["mirror", "source"], default="mirror")
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--capture", type=Path)
    args = parser.parse_args()
    if sys.platform != "win32" or C.sizeof(C.c_void_p) != 8:
        parser.error("Windows x64 required")
    if args.pid <= 0 or not 0 < args.seconds <= 4 or not math.isfinite(args.yaw) or abs(args.yaw) > 15:
        parser.error("Limit: 4 seconds and 15 degrees")
    if args.output.exists() or (args.capture and args.capture.exists()):
        parser.error("Choose new evidence paths; existing evidence will not be overwritten")
    meta = process_metadata(args.pid)
    if Path(meta["Path"]).resolve() != EXPECTED_EXE.resolve():
        raise RuntimeError("Wrong executable")
    modules = {m.lower() for m in meta["Modules"]}
    if "ac6_coop.dll" not in modules or any("easyanticheat" in m or m.startswith("eac_") for m in modules):
        raise RuntimeError("This experiment requires the observed cooperative single-player session without EAC")
    with EXPECTED_EXE.open("rb") as f:
        if hashlib.file_digest(f, "sha256").hexdigest() != EXPECTED_SHA:
            raise RuntimeError("Different executable build")

    k = C.WinDLL("kernel32", use_last_error=True)
    k.OpenProcess.argtypes = [W.DWORD, W.BOOL, W.DWORD]
    k.OpenProcess.restype = W.HANDLE
    k.CloseHandle.argtypes = [W.HANDLE]
    k.ReadProcessMemory.argtypes = [W.HANDLE, C.c_void_p, C.c_void_p, C.c_size_t, C.POINTER(C.c_size_t)]
    k.ReadProcessMemory.restype = W.BOOL
    k.WriteProcessMemory.argtypes = k.ReadProcessMemory.argtypes
    k.WriteProcessMemory.restype = W.BOOL
    rights = 0x410 | (0x28 if args.apply else 0)
    handle = k.OpenProcess(rights, False, args.pid)
    if not handle:
        raise C.WinError(C.get_last_error())

    def read(address, count):
        buf, size = C.create_string_buffer(count), C.c_size_t()
        if not k.ReadProcessMemory(handle, address, buf, count, C.byref(size)) or size.value != count:
            raise C.WinError(C.get_last_error())
        return buf.raw

    def pointer(address):
        value, = struct.unpack("<Q", read(address, 8))
        if not 0x10000 <= value <= 0x7FFFFFFFFFFF:
            raise RuntimeError("Invalid pointer")
        return value

    def write(address, raw):
        size = C.c_size_t()
        buf = C.create_string_buffer(raw)
        if not k.WriteProcessMemory(handle, address, buf, len(raw), C.byref(size)) or size.value != len(raw):
            raise C.WinError(C.get_last_error())

    report = {"pid": args.pid, "mode": "apply" if args.apply else "dry-run", "rights": hex(rights),
              "stage": args.stage,
              "yaw_degrees": args.yaw, "duration_seconds": args.seconds, "writes": 0,
              "restoration": "not_needed", "scope": "First 48 matrix bytes only; translation untouched"}
    pool, capture_future = None, None
    try:
        base = meta["Base"]
        insn = read(base + 0x151ED17, 7)
        if insn[:3] != b"\x48\x8b\x05" or base + 0x151ED1E + struct.unpack("<i", insn[3:])[0] != base + GLOBAL_RVA:
            raise RuntimeError("Live instruction differs")
        if args.stage == "source" and read(base + 0x6EF560, 5) != bytes.fromhex("48 8b 41 08 c3"):
            raise RuntimeError("Live source-camera getter differs")
        root_rva = 0x4E09E80 if args.stage == "source" else GLOBAL_RVA
        owner_offset = 8 if args.stage == "source" else 0xC6A8
        matrix_offset = 0x10 if args.stage == "source" else 0x110
        global_pointer = pointer(base + root_rva)
        destination = pointer(global_pointer + owner_offset)
        address = destination + matrix_offset
        original = read(address, 64)
        values = struct.unpack("<16f", original)
        metrics = matrix_metrics(values)
        if not metrics["finite"] or metrics["basis_orthogonality_error"] > 0.001 or metrics["homogeneous_error"] > 0.001:
            raise RuntimeError("Candidate is not a valid rigid transform")
        half = math.radians(args.yaw) / 2
        target = compose_head_orientation(values, [0, 0, 0, 1], [0, math.sin(half), 0, math.cos(half)])
        target_raw = struct.pack("<12f", *target[:12])
        report.update(matrix_address=hex(address), original_matrix=list(values), target_matrix=target)
        if args.apply:
            if args.capture:
                args.capture.parent.mkdir(parents=True, exist_ok=True)
                pool = concurrent.futures.ThreadPoolExecutor(max_workers=1)
            start = time.monotonic()
            print("Holding temporary camera offset", flush=True)
            try:
                while time.monotonic() - start < args.seconds:
                    if pointer(base + root_rva) != global_pointer or pointer(global_pointer + owner_offset) != destination:
                        raise RuntimeError("Camera owner changed; stopping experiment")
                    write(address, target_raw)
                    report["writes"] += 1
                    if pool and capture_future is None and time.monotonic() - start >= 0.5:
                        capture_future = pool.submit(subprocess.run,
                            ["um.cmd", "win", "shot", "--exe", "armoredcore6.exe", str(args.capture), "--scale", "0.33"],
                            capture_output=True, text=True, timeout=10, creationflags=subprocess.CREATE_NO_WINDOW)
                    time.sleep(0.001)
            finally:
                # Do not replace a newer game-produced orientation on restoration.
                if report["writes"]:
                    if pointer(base + root_rva) == global_pointer and pointer(global_pointer + owner_offset) == destination:
                        if read(address, 48) == target_raw:
                            write(address, original[:48])
                            report["restoration"] = "saved_orientation_restored"
                        else:
                            report["restoration"] = "game_already_replaced_orientation"
                    else:
                        report["restoration"] = "owner_changed_no_stale_write"
        if capture_future:
            result = capture_future.result(timeout=12)
            report["capture"] = {"exit_code": result.returncode, "output": result.stdout,
                                 "error": result.stderr, "path": str(args.capture)}
        report["matrix_after"] = list(struct.unpack("<16f", read(address, 64)))
    except Exception as error:
        report["error"] = str(error)
        raise
    finally:
        k.CloseHandle(handle)
        if pool:
            pool.shutdown(wait=True)
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(json.dumps({k: v for k, v in report.items() if k not in ("original_matrix", "target_matrix", "matrix_after")}))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

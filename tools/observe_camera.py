"""Read one AC6 camera-matrix candidate in a confirmed single-player mod session.

No debugger, hooks, injection or write-capable process handle. Requires an
explicit PID and --single-player-confirmed, because loader presence cannot establish
the game's network state. Addresses are specific to the fingerprinted build.
"""

from __future__ import annotations

import argparse
import ctypes
from ctypes import wintypes
import datetime
import hashlib
import json
import math
from pathlib import Path
import struct
import subprocess
import sys
import time


EXPECTED_EXE = Path(r"C:\Program Files (x86)\Steam\steamapps\common\ARMORED CORE VI FIRES OF RUBICON\Game\armoredcore6.exe")
EXPECTED_SHA = "e003771e5548bca91882c1d7dfe2df3de97cee05c2c989d0c0e34b036ea610d6"
GLOBAL_RVA = 0x4F71C70


def matrix_metrics(values: tuple[float, ...]) -> dict:
    if not all(math.isfinite(v) for v in values):
        return {"finite": False}
    basis = [values[i:i + 3] for i in (0, 4, 8)]
    dots = [[sum(a * b for a, b in zip(x, y)) for y in basis] for x in basis]
    orth_error = max(abs(dots[i][j] - (1.0 if i == j else 0.0))
                     for i in range(3) for j in range(3))
    a, b, c = basis
    determinant = (a[0] * (b[1] * c[2] - b[2] * c[1])
                   - a[1] * (b[0] * c[2] - b[2] * c[0])
                   + a[2] * (b[0] * c[1] - b[1] * c[0]))
    return {"finite": True, "basis_orthogonality_error": orth_error,
            "basis_determinant": determinant, "position_candidate": list(values[12:15]),
            "homogeneous_error": max(abs(values[3]), abs(values[7]), abs(values[11]), abs(values[15] - 1))}


def process_metadata(pid: int) -> dict:
    # The only interpolated value is an integer parsed by argparse.
    command = (
        f"$p=Get-Process -Id {pid} -ErrorAction Stop; "
        "[pscustomobject]@{Id=$p.Id;Path=$p.MainModule.FileName;"
        "Base=$p.MainModule.BaseAddress.ToInt64();"
        "Modules=@($p.Modules | ForEach-Object {$_.ModuleName})} | ConvertTo-Json -Depth 3 -Compress"
    )
    result = subprocess.run(["powershell.exe", "-NoProfile", "-NonInteractive", "-Command", command],
                            capture_output=True, check=True, encoding="utf-8", errors="replace")
    return json.loads(result.stdout)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--pid", type=int, required=True)
    parser.add_argument("--single-player-confirmed", action="store_true", required=True)
    parser.add_argument("--seconds", type=float, default=5)
    parser.add_argument("--interval", type=float, default=0.25)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    if sys.platform != "win32" or ctypes.sizeof(ctypes.c_void_p) != 8:
        parser.error("Requires 64-bit Python on Windows")
    if args.pid <= 0 or not 0 <= args.seconds <= 60 or not 0.05 <= args.interval <= 5:
        parser.error("Invalid PID or sampling window (0..60s, 0.05..5s interval)")
    metadata = process_metadata(args.pid)
    if Path(metadata["Path"]).resolve() != EXPECTED_EXE.resolve():
        raise RuntimeError("PID does not belong to the selected local AC6 executable")
    with EXPECTED_EXE.open("rb") as binary:
        sha = hashlib.file_digest(binary, "sha256").hexdigest()
    if sha != EXPECTED_SHA:
        raise RuntimeError("Build hash differs from camera research; do not reuse offsets")
    modules = [m.lower() for m in metadata["Modules"]]
    if not ({"modengine2.dll", "ac6_coop.dll"} & set(modules)):
        raise RuntimeError("No expected mod loader found in the selected process")
    if any("easyanticheat" in m or m.startswith("eac_") for m in modules):
        raise RuntimeError("EAC module detected; no memory observation will be performed")

    kernel = ctypes.WinDLL("kernel32", use_last_error=True)
    kernel.OpenProcess.argtypes = [wintypes.DWORD, wintypes.BOOL, wintypes.DWORD]
    kernel.OpenProcess.restype = wintypes.HANDLE
    kernel.ReadProcessMemory.argtypes = [wintypes.HANDLE, ctypes.c_void_p, ctypes.c_void_p,
                                         ctypes.c_size_t, ctypes.POINTER(ctypes.c_size_t)]
    kernel.ReadProcessMemory.restype = wintypes.BOOL
    kernel.CloseHandle.argtypes = [wintypes.HANDLE]
    kernel.CloseHandle.restype = wintypes.BOOL
    kernel.QueryFullProcessImageNameW.argtypes = [wintypes.HANDLE, wintypes.DWORD,
                                                 wintypes.LPWSTR, ctypes.POINTER(wintypes.DWORD)]
    kernel.QueryFullProcessImageNameW.restype = wintypes.BOOL
    # PROCESS_QUERY_INFORMATION | PROCESS_VM_READ; deliberately excludes write rights.
    handle = kernel.OpenProcess(0x0400 | 0x0010, False, args.pid)
    if not handle:
        raise ctypes.WinError(ctypes.get_last_error())

    def read(address: int, count: int) -> bytes:
        buffer = ctypes.create_string_buffer(count)
        received = ctypes.c_size_t()
        if not kernel.ReadProcessMemory(handle, address, buffer, count, ctypes.byref(received)):
            raise ctypes.WinError(ctypes.get_last_error())
        if received.value != count:
            raise RuntimeError("Incomplete process-memory read")
        return buffer.raw

    def pointer(address: int) -> int:
        value, = struct.unpack("<Q", read(address, 8))
        if not 0x10000 <= value <= 0x7FFFFFFFFFFF:
            raise RuntimeError(f"Uninitialized/invalid pointer at 0x{address:x}")
        return value

    try:
        # Recheck executable through the opened handle to catch PID reuse.
        name = ctypes.create_unicode_buffer(32768)
        length = wintypes.DWORD(len(name))
        if not kernel.QueryFullProcessImageNameW(handle, 0, name, ctypes.byref(length)):
            raise ctypes.WinError(ctypes.get_last_error())
        if Path(name.value).resolve() != EXPECTED_EXE.resolve():
            raise RuntimeError("Process identity changed")
        base = metadata["Base"]
        load_site = base + 0x151ED17
        instruction = read(load_site, 7)
        if instruction[:3] != b"\x48\x8b\x05":
            raise RuntimeError("Live global-load opcode differs from inspected build")
        displacement, = struct.unpack("<i", instruction[3:])
        if load_site + 7 + displacement != base + GLOBAL_RVA:
            raise RuntimeError("Live global reference differs from the research")

        args.output.parent.mkdir(parents=True, exist_ok=True)
        with args.output.open("x", encoding="utf-8") as out:
            out.write(json.dumps({"kind": "metadata", "pid": args.pid, "sha256": sha,
                "module_base": hex(base), "session_state": "single_player_confirmed_by_user",
                "offline_state": "not_independently_verified",
                "loaders": sorted(set(modules) & {"modengine2.dll", "ac6_coop.dll"}),
                "access": "PROCESS_QUERY_INFORMATION|PROCESS_VM_READ",
                "scope": "candidate mirror matrix; camera identity not established"}) + "\n")
            start = time.monotonic()
            previous = None
            valid = changes = samples = 0
            consecutive_errors = 0
            while True:
                row = {"kind": "sample", "time_utc": datetime.datetime.now(datetime.timezone.utc).isoformat()}
                try:
                    global_ptr = pointer(base + GLOBAL_RVA)
                    destination = pointer(global_ptr + 0xC6A8)
                    address = destination + 0x110
                    raw = read(address, 64)
                    values = struct.unpack("<16f", raw)
                    metrics = matrix_metrics(values)
                    changed = previous is not None and previous != raw
                    changes += changed
                    valid += bool(metrics["finite"])
                    previous = raw
                    row.update(global_pointer=hex(global_ptr), destination=hex(destination),
                               matrix_address=hex(address), matrix=list(values) if metrics["finite"] else None,
                               immediate_repeat_equal=(raw == read(address, 64)),
                               changed_from_previous=changed, **metrics)
                    consecutive_errors = 0
                except (OSError, RuntimeError) as error:
                    row["error"] = str(error)
                    consecutive_errors += 1
                out.write(json.dumps(row, allow_nan=False) + "\n")
                out.flush()
                samples += 1
                if consecutive_errors >= 3 or time.monotonic() - start >= args.seconds:
                    break
                time.sleep(min(args.interval, max(0, args.seconds - (time.monotonic() - start))))
        print(json.dumps({"samples": samples, "finite_samples": valid, "changes": changes,
                          "output": str(args.output.resolve())}))
        return 0 if valid else 2
    finally:
        kernel.CloseHandle(handle)


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, RuntimeError, subprocess.CalledProcessError) as error:
        print(str(error), file=sys.stderr)
        raise SystemExit(1)

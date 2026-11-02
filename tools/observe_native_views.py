"""Bounded external read-only census of AC6's native view list.

Requires a CURRENT explicitly confirmed offline session, exact build and PID.
No DLL, hooks, code execution in the game, write rights or GPU commands.
Repeated identities are consistency checks, not an atomic/render-frame snapshot.
"""
from __future__ import annotations

import argparse
import ctypes
from ctypes import wintypes
import datetime
import json
from pathlib import Path
import struct
import sys
import time

MANAGER_RVA = 0x4E23028
MAIN_VIEW_VTABLE_RVA = 0x3E3E7E0


def validate_session(initial: dict, current: dict, expected_image: Path) -> int:
    for identity in (initial, current):
        if Path(identity["path"]).resolve() != expected_image.resolve():
            raise ValueError("Unexpected process executable")
        modules = {name.lower() for name in identity["modules"]}
        if "ac6_coop.dll" not in modules:
            raise ValueError("Expected offline mod host absent")
        if any("easyanticheat" in name or name.startswith("eac_") for name in modules):
            raise ValueError("Anti-cheat module present; observation refused")
    if any(initial[key] != current[key] for key in ("creation", "base", "path")):
        raise ValueError("Process instance changed during preflight")
    return current["base"]


def sample_views(read, base: int) -> dict:
    def exact(address, size):
        result = read(address, size)
        if len(result) != size:
            raise ValueError("Partial memory snapshot")
        return result
    def q(data, offset=0):
        return struct.unpack_from("<Q", data, offset)[0]
    def pointer(value):
        if not 0x10000 <= value <= 0x7FFFFFFFFFFF or value & 7:
            raise ValueError("Invalid native pointer")
        return value
    result = dict(status="manager_absent", count=0, views=[],
                  identity_repeat_equal=False, atomic_snapshot=False,
                  render_calls_observed=False, frame_associated=False)
    root_bytes = exact(base + MANAGER_RVA, 8)
    root = q(root_bytes)
    if not root:
        return result
    pointer(root)
    root_fields = exact(root, 0x18)
    list_address, view_manager = q(root_fields, 8), q(root_fields, 0x10)
    result.update(manager=hex(root), list=hex(list_address), view_manager=hex(view_manager))
    if not list_address or not view_manager:
        result["status"] = "list_or_view_manager_absent"
        return result
    pointer(list_address);pointer(view_manager)
    manager_fields = exact(view_manager, 0x3D8)
    def manager_ids(data):
        return tuple(q(data, n) for n in (0x10, 0x58, 0x3C0, 0x3C8))
    camera, scene, manager_list, main = manager_ids(manager_fields)
    result.update(main_view=hex(main), main_camera=hex(camera), main_scene=hex(scene))
    header = exact(list_address, 0x20)
    begin, end, capacity = (q(header, n) for n in (8, 0x10, 0x18))
    if begin == end == capacity == 0:
        elements = b""
        count = 0
    else:
        pointer(begin)
        if end < begin or capacity < end or (end - begin) % 8 or (capacity - begin) % 8:
            raise ValueError("Invalid view vector bounds")
        count = (end - begin) // 8
        if count > 32 or (capacity - begin) // 8 > 4096:
            raise ValueError("View vector exceeds observation bounds")
        elements = exact(begin, count * 8) if count else b""
    records = []
    def identities(data):
        return tuple(q(data, n) for n in (0, 0x38, 0x40, 0x48))
    addresses = [q(elements, i * 8) for i in range(count)]
    if len(set(addresses)) != len(addresses):
        raise ValueError("Duplicate view identities")
    for address in addresses:
        pointer(address)
        data = exact(address, 0x50)
        vtable, targets, view_scene, link = identities(data)
        pointer(vtable)
        records.append((address, identities(data)))
        result["views"].append(dict(address=hex(address), vtable=hex(vtable),
            priority=struct.unpack_from("<I", data, 0x10)[0],
            targets=hex(targets), scene=hex(view_scene), camera_link=hex(link),
            is_main=address == main, expected_main_type=vtable == base + MAIN_VIEW_VTABLE_RVA))
    repeated = (exact(base + MANAGER_RVA, 8) == root_bytes and
                exact(root, 0x18) == root_fields and
                manager_ids(exact(view_manager, 0x3D8)) == manager_ids(manager_fields) and
                exact(list_address, 0x20) == header and
                (not count or exact(begin, count * 8) == elements))
    if repeated:
        repeated = all(identities(exact(address, 0x50)) == ids for address, ids in records)
    result.update(count=count, identity_repeat_equal=repeated)
    if not repeated:
        result["status"] = "identity_changed"
        return result
    if main not in addresses:
        result["status"] = "main_not_listed"
        return result
    row = result["views"][addresses.index(main)]
    matches = (manager_list == list_address and row["expected_main_type"] and
               camera != 0 and scene != 0 and row["camera_link"] == hex(camera) and
               row["scene"] == hex(scene) and row["targets"] != "0x0")
    result["status"] = "identities_match" if matches else "identity_mismatch"
    return result


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--pid", type=int, required=True)
    parser.add_argument("--single-player-confirmed", action="store_true", required=True)
    parser.add_argument("--seconds", type=float, default=3)
    parser.add_argument("--interval", type=float, default=0.1)
    parser.add_argument("--resources", action="store_true", help="Also census verified target/pipeline references")
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    if (sys.platform != "win32" or ctypes.sizeof(ctypes.c_void_p) != 8 or args.pid <= 0 or
            not 0 < args.seconds <= 8 or not 0.05 <= args.interval <= 1):
        parser.error("Requires Windows x64, positive PID, 0..8 seconds and 0.05..1 second interval")
    from observe_camera import EXPECTED_EXE, EXPECTED_SHA
    from inspect_native_view_route import inspect
    if args.resources:
        from native_view_resources import sample_resources
    kernel = ctypes.WinDLL("kernel32", use_last_error=True)
    if args.resources:
        kernel.GetTickCount64.argtypes = []
        kernel.GetTickCount64.restype = ctypes.c_ulonglong
    kernel.OpenProcess.argtypes = [wintypes.DWORD, wintypes.BOOL, wintypes.DWORD]
    kernel.OpenProcess.restype = wintypes.HANDLE
    kernel.ReadProcessMemory.argtypes = [wintypes.HANDLE, ctypes.c_void_p, ctypes.c_void_p,
                                         ctypes.c_size_t, ctypes.POINTER(ctypes.c_size_t)]
    kernel.ReadProcessMemory.restype = wintypes.BOOL
    kernel.QueryFullProcessImageNameW.argtypes = [wintypes.HANDLE, wintypes.DWORD,
                                                wintypes.LPWSTR, ctypes.POINTER(wintypes.DWORD)]
    kernel.QueryFullProcessImageNameW.restype = wintypes.BOOL
    kernel.GetProcessTimes.argtypes = [wintypes.HANDLE] + [ctypes.POINTER(wintypes.FILETIME)] * 4
    kernel.GetProcessTimes.restype = wintypes.BOOL
    kernel.K32EnumProcessModulesEx.argtypes = [wintypes.HANDLE, ctypes.POINTER(ctypes.c_void_p),
                                              wintypes.DWORD, ctypes.POINTER(wintypes.DWORD), wintypes.DWORD]
    kernel.K32EnumProcessModulesEx.restype = wintypes.BOOL
    kernel.K32GetModuleFileNameExW.argtypes = [wintypes.HANDLE, ctypes.c_void_p,
                                             wintypes.LPWSTR, wintypes.DWORD]
    kernel.K32GetModuleFileNameExW.restype = wintypes.DWORD
    kernel.CloseHandle.argtypes = [wintypes.HANDLE]
    kernel.CloseHandle.restype = wintypes.BOOL
    handle = kernel.OpenProcess(0x410, False, args.pid)
    if not handle:
        raise ctypes.WinError(ctypes.get_last_error())
    def read(address, size):
        buffer = ctypes.create_string_buffer(size)
        received = ctypes.c_size_t()
        if not kernel.ReadProcessMemory(handle, address, buffer, size, ctypes.byref(received)):
            raise ctypes.WinError(ctypes.get_last_error())
        if received.value != size:
            raise ValueError("Incomplete process-memory read")
        return buffer.raw
    def identity():
        name = ctypes.create_unicode_buffer(32768)
        length = wintypes.DWORD(len(name))
        if not kernel.QueryFullProcessImageNameW(handle, 0, name, ctypes.byref(length)):
            raise ctypes.WinError(ctypes.get_last_error())
        times = [wintypes.FILETIME() for _ in range(4)]
        if not kernel.GetProcessTimes(handle, *(ctypes.byref(value) for value in times)):
            raise ctypes.WinError(ctypes.get_last_error())
        array = (ctypes.c_void_p * 4096)()
        needed = wintypes.DWORD()
        if not kernel.K32EnumProcessModulesEx(handle, array, ctypes.sizeof(array), ctypes.byref(needed), 3):
            raise ctypes.WinError(ctypes.get_last_error())
        if not 0 < needed.value <= ctypes.sizeof(array) or needed.value % ctypes.sizeof(ctypes.c_void_p):
            raise ValueError("Incomplete module enumeration")
        modules = []
        for index in range(needed.value // ctypes.sizeof(ctypes.c_void_p)):
            path = ctypes.create_unicode_buffer(32768)
            count = kernel.K32GetModuleFileNameExW(handle, array[index], path, len(path))
            if not 0 < count < len(path):
                raise ValueError("Incomplete module identity")
            if index == 0 and Path(path.value).resolve() != Path(name.value).resolve():
                raise ValueError("Primary module does not match executable")
            modules.append(Path(path.value).name)
        return dict(path=str(Path(name.value).resolve()),
                    creation=times[0].dwLowDateTime | (times[0].dwHighDateTime << 32),
                    base=array[0], modules=modules)
    try:
        # All identity/module queries use this SAME read-only process handle.
        # A recycled PID cannot inherit another instance's module approval.
        initial = identity()
        validate_session(initial, initial, EXPECTED_EXE)
        route = inspect(EXPECTED_EXE)
        if route["executable_sha256"] != EXPECTED_SHA or not route["all_checks_verified"]:
            raise ValueError("Native route not fully verified for this image")
        base = validate_session(initial, identity(), EXPECTED_EXE)
        import pefile
        pe = pefile.PE(str(EXPECTED_EXE), fast_load=True)
        try:
            sites = {0x1292BFB, 0x2466510, 0x2466470, 0x2524790, 0x2524A90}
            if args.resources:
                from inspect_native_view_route import OWNERSHIP_CHECKS
                sites.update(check[1] for check in OWNERSHIP_CHECKS)
                sites.update(check[2] for check in OWNERSHIP_CHECKS)
            for site in sorted(sites):
                if read(base + site, 16) != pe.get_data(site, 16):
                    raise ValueError("Live native-route code differs from the inspected image")
        finally:
            pe.close()
        counts = {}
        errors = samples = 0
        with args.output.open("x", encoding="utf-8") as out:
            out.write(json.dumps(dict(kind="metadata", schema="native_view_resources_v1" if args.resources else "native_view_observation_v1", pid=args.pid,
                executable_sha256=EXPECTED_SHA, module_base=hex(base), creation_time=initial["creation"], process_access="0x410",
                session="offline_confirmed_by_operator", offline_independently_verified=False,
                hooks_installed=False, game_writes=False, frame_associated=False)) + "\n")
            deadline = time.monotonic() + args.seconds
            while True:
                row = dict(kind="sample", utc=datetime.datetime.now(datetime.timezone.utc).isoformat())
                if args.resources:
                    row["sample_begin_tick_ms"] = kernel.GetTickCount64()
                try:
                    row.update(sample_resources(read, base) if args.resources else sample_views(read, base))
                    errors = 0
                except (ValueError, OSError) as error:
                    row.update(status="read_error", error=str(error), frame_associated=False)
                    errors += 1
                counts[row["status"]] = counts.get(row["status"], 0) + 1
                samples += 1
                out.write(json.dumps(row, allow_nan=False) + "\n")
                out.flush()
                remaining = deadline - time.monotonic()
                if remaining <= 0 or errors >= 3:
                    break
                time.sleep(min(args.interval, remaining))
        print(json.dumps(dict(samples=samples, statuses=counts, output=str(args.output.resolve()),
                              hooks_installed=False, game_writes=False, frame_associated=False)))
        return 0 if counts.get("resource_identities_match" if args.resources else "identities_match") else 2
    finally:
        kernel.CloseHandle(handle)


if __name__ == "__main__":
    raise SystemExit(main())

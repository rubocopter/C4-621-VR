"""Experimental OpenXR tracking probe; no swapchains or game-memory access.

Uses XR_MND_headless and the installed SteamVR loader. ABI declarations follow
Khronos openxr.h/openxr_platform.h retained under reference/openxr. This is a
research probe, not the render integration or a game mod.
"""

from __future__ import annotations

import argparse
import ctypes as C
import datetime
import json
import math
from pathlib import Path
import sys
import time


I = C.c_int32
U = C.c_uint32
U64 = C.c_uint64
T = C.c_int64
H = C.c_void_p
P = C.POINTER


class Quaternion(C.Structure):
    _fields_ = [(n, C.c_float) for n in ("x", "y", "z", "w")]


class Vector(C.Structure):
    _fields_ = [(n, C.c_float) for n in ("x", "y", "z")]


class Pose(C.Structure):
    _fields_ = [("orientation", Quaternion), ("position", Vector)]


class Application(C.Structure):
    _fields_ = [("name", C.c_char * 128), ("version", U), ("engine", C.c_char * 128),
                ("engine_version", U), ("api_version", U64)]


class InstanceInfo(C.Structure):
    _fields_ = [("type", I), ("next", H), ("flags", U64), ("application", Application),
                ("layer_count", U), ("layers", P(C.c_char_p)),
                ("extension_count", U), ("extensions", P(C.c_char_p))]


class InstanceProperties(C.Structure):
    _fields_ = [("type", I), ("next", H), ("version", U64), ("name", C.c_char * 128)]


class SystemInfo(C.Structure):
    _fields_ = [("type", I), ("next", H), ("form_factor", I)]


class SessionInfo(C.Structure):
    _fields_ = [("type", I), ("next", H), ("flags", U64), ("system", U64)]


class SessionBegin(C.Structure):
    _fields_ = [("type", I), ("next", H), ("view_type", I)]


class SpaceInfo(C.Structure):
    _fields_ = [("type", I), ("next", H), ("space_type", I), ("pose", Pose)]


class Location(C.Structure):
    _fields_ = [("type", I), ("next", H), ("flags", U64), ("pose", Pose)]


class EventBuffer(C.Structure):
    _fields_ = [("type", I), ("next", H), ("data", C.c_uint8 * 4000)]


class SessionEvent(C.Structure):
    _fields_ = [("type", I), ("next", H), ("session", H), ("state", I), ("time", T)]


class ViewInfo(C.Structure):
    _fields_ = [("type", I), ("next", H), ("view_type", I), ("time", T), ("space", H)]


class ViewState(C.Structure):
    _fields_ = [("type", I), ("next", H), ("flags", U64)]


class Fov(C.Structure):
    _fields_ = [(n, C.c_float) for n in ("left", "right", "up", "down")]


class View(C.Structure):
    _fields_ = [("type", I), ("next", H), ("pose", Pose), ("fov", Fov)]


def pose_dict(pose: Pose) -> dict:
    return {"orientation_xyzw": [getattr(pose.orientation, n) for n in ("x", "y", "z", "w")],
            "position_m": [getattr(pose.position, n) for n in ("x", "y", "z")]}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--seconds", type=float, default=8)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--publish-pid", type=int)
    parser.add_argument("--flip-z", action="store_true")
    args = parser.parse_args()
    if sys.platform != "win32" or C.sizeof(H) != 8 or not 0 <= args.seconds <= 60:
        parser.error("Requires Windows x64 and a duration of 0..60 seconds")
    # Windows x64 ABI sanity checks, before passing structures to native code.
    for structure, size in [(InstanceInfo, 328), (InstanceProperties, 152),
                            (SessionInfo, 32), (SpaceInfo, 48), (Location, 56),
                            (EventBuffer, 4016), (SessionEvent, 40), (ViewInfo, 40), (View, 64)]:
        if C.sizeof(structure) != size:
            raise RuntimeError(f"Unexpected ABI for {structure.__name__}")
    loader = Path(r"C:\Program Files (x86)\Steam\steamapps\common\SteamVR\bin\win64\openxr_loader.dll")
    dll = C.WinDLL(str(loader))

    def bind(name, *types):
        function = getattr(dll, name)
        function.restype = I
        function.argtypes = list(types)
        return function

    def check(result, operation):
        if result < 0:
            raise RuntimeError(f"{operation}: XrResult={result}")
        return result

    create_instance = bind("xrCreateInstance", P(InstanceInfo), P(H))
    destroy_instance = bind("xrDestroyInstance", H)
    get_properties = bind("xrGetInstanceProperties", H, P(InstanceProperties))
    get_system = bind("xrGetSystem", H, P(SystemInfo), P(U64))
    create_session = bind("xrCreateSession", H, P(SessionInfo), P(H))
    destroy_session = bind("xrDestroySession", H)
    begin_session = bind("xrBeginSession", H, P(SessionBegin))
    end_session = bind("xrEndSession", H)
    request_exit = bind("xrRequestExitSession", H)
    create_space = bind("xrCreateReferenceSpace", H, P(SpaceInfo), P(H))
    destroy_space = bind("xrDestroySpace", H)
    locate_space = bind("xrLocateSpace", H, H, T, P(Location))
    locate_views = bind("xrLocateViews", H, P(ViewInfo), P(ViewState), U, P(U), P(View))
    poll_event = bind("xrPollEvent", H, P(EventBuffer))
    get_proc = bind("xrGetInstanceProcAddr", H, C.c_char_p, P(H))

    instance, session, local, view_space = H(), H(), H(), H()
    running = False
    publication = None
    states = []
    state = 0

    def events():
        nonlocal state
        while True:
            event = EventBuffer(type=16)
            result = check(poll_event(instance, C.byref(event)), "xrPollEvent")
            if result == 4:
                break
            if event.type == 18:
                changed = C.cast(C.byref(event), P(SessionEvent)).contents
                if changed.session == session.value:
                    state = changed.state
                    states.append(state)

    # Exclusive capture creation ensures no previous evidence is overwritten.
    args.output.parent.mkdir(parents=True, exist_ok=True)
    with args.output.open("x", encoding="utf-8") as out:
        try:
            extensions = (C.c_char_p * 2)(b"XR_MND_headless", b"XR_KHR_win32_convert_performance_counter_time")
            app = Application(name=b"AC6 tracking research", version=1, engine=b"diagnostic", api_version=1 << 48)
            info = InstanceInfo(type=3, application=app, extension_count=2, extensions=extensions)
            check(create_instance(C.byref(info), C.byref(instance)), "xrCreateInstance")
            properties = InstanceProperties(type=32)
            check(get_properties(instance, C.byref(properties)), "xrGetInstanceProperties")
            system = U64()
            check(get_system(instance, C.byref(SystemInfo(type=4, form_factor=1)), C.byref(system)), "xrGetSystem")
            check(create_session(instance, C.byref(SessionInfo(type=8, system=system)), C.byref(session)), "xrCreateSession headless")
            deadline = time.monotonic() + 8
            while state != 2 and time.monotonic() < deadline:
                events()
                time.sleep(0.02)
            if state != 2:
                raise RuntimeError(f"Headless session did not reach READY; states={states}")
            check(begin_session(session, C.byref(SessionBegin(type=10, view_type=2))), "xrBeginSession")
            running = True
            for space_type, output in [(2, local), (1, view_space)]:
                info = SpaceInfo(type=37, space_type=space_type, pose=Pose(orientation=Quaternion(w=1)))
                check(create_space(session, C.byref(info), C.byref(output)), "xrCreateReferenceSpace")
            address = H()
            check(get_proc(instance, b"xrConvertWin32PerformanceCounterToTimeKHR", C.byref(address)), "xrGetInstanceProcAddr time")
            convert_time = C.WINFUNCTYPE(I, H, P(T), P(T))(address.value)
            kernel = C.WinDLL("kernel32", use_last_error=True)
            kernel.QueryPerformanceCounter.argtypes = [P(T)]
            kernel.QueryPerformanceCounter.restype = C.c_bool
            metadata = {"kind": "metadata", "api": "OpenXR", "runtime": properties.name.decode(),
                        "runtime_version_raw": properties.version, "extensions": [e.decode() for e in extensions],
                        "session": "headless", "reference_spaces": "VIEW relative to LOCAL",
                        "scope": "Tracking and view parameters only; no swapchains, display output or game writes"}
            if args.publish_pid:
                from pose_publication import PosePublication
                publication = PosePublication(args.publish_pid, flip_z=args.flip_z)
                metadata["pose_publication_pid"] = args.publish_pid
                metadata["rotation_limit_degrees"] = 25
                metadata["flip_z"] = args.flip_z
            out.write(json.dumps(metadata) + "\n")
            start = time.monotonic()
            valid_count = samples = 0
            while True:
                events()
                if state in (6, 7, 8):
                    break
                counter, xr_time = T(), T()
                if not kernel.QueryPerformanceCounter(C.byref(counter)):
                    raise C.WinError(C.get_last_error())
                check(convert_time(instance, C.byref(counter), C.byref(xr_time)), "xrConvertWin32PerformanceCounterToTimeKHR")
                location = Location(type=42)
                check(locate_space(view_space, local, xr_time, C.byref(location)), "xrLocateSpace")
                row = {"kind": "sample", "time_utc": datetime.datetime.now(datetime.timezone.utc).isoformat(),
                       "xr_time": xr_time.value, "location_flags": location.flags, **pose_dict(location.pose)}
                valid = (location.flags & 3) == 3
                row["orientation_and_position_valid"] = valid
                if publication:
                    publication.update(row["orientation_xyzw"], (location.flags & 5) == 5)
                if valid and not all(math.isfinite(v) for v in row["orientation_xyzw"] + row["position_m"]):
                    raise RuntimeError("Runtime marked nonfinite pose as valid")
                views = (View * 2)(View(type=7), View(type=7))
                view_state, count = ViewState(type=11), U()
                locate_info = ViewInfo(type=6, view_type=2, time=xr_time, space=local)
                check(locate_views(session, C.byref(locate_info), C.byref(view_state), 2, C.byref(count), views), "xrLocateViews")
                row["view_flags"] = view_state.flags
                row["views"] = [{**pose_dict(v.pose), "fov_radians": [v.fov.left, v.fov.right, v.fov.up, v.fov.down]} for v in views[:count.value]]
                out.write(json.dumps(row, allow_nan=False) + "\n")
                out.flush()
                samples += 1
                valid_count += valid
                if time.monotonic() - start >= args.seconds:
                    break
                time.sleep(0.02)
            print(json.dumps({**metadata, "states": states, "samples": samples, "valid_samples": valid_count,
                              "output": str(args.output.resolve())}))
            return 0 if valid_count else 2
        finally:
            if publication:
                publication.close()
            if running:
                request_exit(session)
                deadline = time.monotonic() + 2
                while state != 6 and time.monotonic() < deadline:
                    events()
                    time.sleep(0.02)
                if state == 6:
                    end_session(session)
            for space in (view_space, local):
                if space.value:
                    destroy_space(space)
            if session.value:
                destroy_session(session)
            if instance.value:
                destroy_instance(instance)


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, RuntimeError) as error:
        print(str(error), file=sys.stderr)
        raise SystemExit(1)

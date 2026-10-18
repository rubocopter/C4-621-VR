"""Bounded cooperative offline AC6 paired-GPU stereo diagnostic (monitor or VR).

Every producer run needs a NEW, unmapped versioned DLL. No game-file changes.
The stream stops on F12, explicit stop request, failure, or 60-second deadline.
"""
import argparse
import ctypes as C
from ctypes import wintypes as W
import hashlib
import json
import math
from pathlib import Path
import re
import subprocess
import sys
import time
from observe_camera import EXPECTED_EXE, EXPECTED_SHA, process_metadata


def consumer_command(exe, mode, mapping, prefix, log, pid, tracking, width, height, rotation_limit):
    if (width, height) not in ((1280, 720), (1920, 1080), (2560, 1440)):
        raise ValueError('Unsupported eye size')
    if not math.isfinite(rotation_limit) or not 1 <= rotation_limit <= 180 or (not tracking and rotation_limit != 25):
        raise ValueError('Rotation limit requires tracking and must be 1..180 degrees')
    if mode == 'monitor':
        if tracking:
            raise ValueError('Head tracking requires VR mode')
        return [str(exe), mapping, '6', str(prefix), str(width), str(height)]
    if mode != 'vr':
        raise ValueError('Unknown consumer mode')
    command = [str(exe), '--pair-stream', mapping, '--seconds', '27' if tracking else '20', '--output', str(log),
               '--eye-width', str(width), '--eye-height', str(height)]
    if tracking:
        command += ['--game-pid', str(pid), '--headtracking', '--rotation-limit-deg', str(rotation_limit)]
    return command


def rows(path):
    if not path.exists():
        return []
    try:
        return [json.loads(s) for s in path.read_text(encoding='utf-8').splitlines() if s.endswith('}')]
    except PermissionError:
        return []


def request_stop(name, expected_pid=None):
    # Aligned 32-bit store on supported Windows x64; only requests worker exit.
    kernel = C.WinDLL('kernel32', use_last_error=True)
    kernel.OpenFileMappingW.argtypes = [W.DWORD, W.BOOL, W.LPCWSTR]
    kernel.OpenFileMappingW.restype = W.HANDLE
    kernel.MapViewOfFile.argtypes = [W.HANDLE, W.DWORD, W.DWORD, W.DWORD, C.c_size_t]
    kernel.MapViewOfFile.restype = C.c_void_p
    kernel.UnmapViewOfFile.argtypes = [C.c_void_p]
    kernel.CloseHandle.argtypes = [W.HANDLE]
    handle = kernel.OpenFileMappingW(2, False, name)
    if not handle:
        return False
    view = kernel.MapViewOfFile(handle, 2, 0, 0, 120)
    try:
        if not view:
            return False
        if W.LONG.from_address(view).value != 1 or not W.DWORD.from_address(view + 4).value:
            return False
        if expected_pid is not None and W.DWORD.from_address(view + 4).value != expected_pid:
            return False
        W.LONG.from_address(view + 36).value = 1
        return True
    finally:
        if view:
            kernel.UnmapViewOfFile(view)
        kernel.CloseHandle(handle)


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--pid', type=int, required=True)
    p.add_argument('--single-player-confirmed', action='store_true', required=True)
    p.add_argument('--dll', type=Path, required=True)
    p.add_argument('--tag', required=True)
    p.add_argument('--mode', choices=('monitor', 'vr'), default='monitor')
    p.add_argument('--headtracking', action='store_true')
    p.add_argument('--eye-width', type=int, default=1280)
    p.add_argument('--eye-height', type=int, default=720)
    p.add_argument('--rotation-limit-deg', type=float, default=25)
    p.add_argument('--camera-dll', type=Path)
    args = p.parse_args()
    if args.pid <= 0 or not re.fullmatch(r'[A-Za-z0-9_-]{1,64}', args.tag):
        p.error('Invalid PID or new evidence tag')
    if args.headtracking and args.mode != 'vr':
        p.error('Head tracking requires VR mode')
    if args.camera_dll and not args.headtracking:
        p.error('Camera DLL requires head tracking')
    try:
        consumer_command('', args.mode, '', Path('.'), Path('.'), args.pid, args.headtracking, args.eye_width, args.eye_height, args.rotation_limit_deg)
    except ValueError as error:
        p.error(str(error))
    dll = args.dll.resolve(strict=True)
    if not re.fullmatch(r'ac6_live_pair_probe_v[1-9][0-9]*\.dll', dll.name):
        p.error('Select a uniquely versioned live pair DLL')
    meta = process_metadata(args.pid)
    modules = {s.lower() for s in meta['Modules']}
    if Path(meta['Path']).resolve() != EXPECTED_EXE.resolve() or 'ac6_coop.dll' not in modules or any('easyanticheat' in m or m.startswith('eac_') for m in modules):
        raise RuntimeError('Requires the selected offline cooperative AC6 host')
    with EXPECTED_EXE.open('rb') as f:
        if hashlib.file_digest(f, 'sha256').hexdigest() != EXPECTED_SHA:
            raise RuntimeError('Different game build')
    if dll.name.lower() in modules:
        raise RuntimeError('Producer DLL already mapped; build a new name, never unload/overwrite it')
    root = Path(__file__).resolve().parents[1]
    display = root / 'native/xr_display_probe/build/Release/ac6_xr_display_probe.exe'
    monitor = root / 'native/live_stereo/build/Release/live_stereo_monitor.exe'
    camera = (args.camera_dll or root / 'native/camera_probe/build/Release/ac6_headtracking_calibrated_probe.dll').resolve()
    if args.headtracking and not re.fullmatch(r'ac6_headtracking_calibrated_probe(?:_v[1-9][0-9]*)?\.dll', camera.name):
        p.error('Select a calibrated headtracking probe')
    app_exe = monitor if args.mode == 'monitor' else display
    if not app_exe.is_file():
        raise RuntimeError('Build the consumer first')
    if args.headtracking and (not camera.is_file() or camera.name.lower() in modules or camera.with_name(f'{camera.stem.removeprefix("ac6_")}_{args.pid}.jsonl').exists()):
        raise RuntimeError('Camera probe requires a new session without a mapped/calibrated probe or existing log')
    folder = root / 'evidence'
    prefix = folder / args.tag
    result = prefix.with_name(prefix.name + '-result.json')
    native = prefix.with_name(prefix.name + '-native.json')
    console = prefix.with_name(prefix.name + '-console.log')
    loader_log = prefix.with_name(prefix.name + '-loader.log')
    app_log = prefix.with_suffix('.jsonl')
    native_log = dll.with_name(f'{dll.stem.removeprefix("ac6_")}_{args.pid}.jsonl')
    mapping = f'Local\\AC6PairStream_{dll.stem.removeprefix("ac6_")}_{args.pid}'
    if any(path.exists() for path in (result, native, console, loader_log, app_log, native_log)) or list(folder.glob(args.tag + '-first*')) or list(folder.glob(args.tag + '-last*')):
        raise RuntimeError('Evidence exists; choose a new tag and unmapped DLL')
    folder.mkdir(parents=True, exist_ok=True)
    report = dict(pid=args.pid, mode=args.mode, mapping=mapping, game_files_changed=False,
                  eye_width=args.eye_width, eye_height=args.eye_height, rotation_limit_degrees=args.rotation_limit_deg if args.headtracking else 0,
                  auxiliary_screenshots=False,
                  source_is_live=True, depth_reprojected_stereo=True, metric_scale_calibrated=False,
                  producer_sha256=hashlib.sha256(dll.read_bytes()).hexdigest(),
                  consumer_sha256=hashlib.sha256(app_exe.read_bytes()).hexdigest())
    app = None
    with loader_log.open('x', encoding='utf-8') as producer_output, console.open('x', encoding='utf-8') as consumer_output:
        loader = subprocess.Popen([sys.executable, '-B', str(root / 'tools/load_camera_probe.py'), '--pid', str(args.pid), '--single-player-confirmed',
            '--dll', str(dll), '--capture', str(prefix) + '-screen.png', '--output', str(native)], cwd=root,
            stdout=producer_output, stderr=subprocess.STDOUT, creationflags=subprocess.CREATE_NO_WINDOW)
        try:
            deadline = time.monotonic() + 15
            while time.monotonic() < deadline:
                events = rows(native_log)
                if any(r.get('phase') == 'stream_ready' for r in events):
                    break
                if loader.poll() is not None or any(r.get('phase') == 'failed' for r in events):
                    raise RuntimeError('Producer failed before stream readiness; inspect native log')
                time.sleep(0.05)
            else:
                raise RuntimeError('Producer readiness timeout')
            command = consumer_command(app_exe,args.mode,mapping,prefix,app_log,args.pid,args.headtracking,args.eye_width,args.eye_height,args.rotation_limit_deg)
            app = subprocess.Popen(command, cwd=root, stdout=consumer_output, stderr=subprocess.STDOUT, creationflags=subprocess.CREATE_NO_WINDOW)
            if args.headtracking:
                deadline = time.monotonic() + 35
                while app.poll() is None and time.monotonic() < deadline:
                    progress = [r for r in rows(app_log) if r.get('kind') == 'progress']
                    if progress and progress[-1].get('tracking_ready'):
                        break
                    time.sleep(0.05)
                else:
                    raise RuntimeError('No visible fresh image and tracked orientation')
                camera_report = prefix.with_name(prefix.name + '-camera.json')
                report['camera_dll'] = str(camera)
                report['camera_sha256'] = hashlib.sha256(camera.read_bytes()).hexdigest()
                camera_result = subprocess.run([sys.executable, '-B', str(root / 'tools/load_camera_probe.py'), '--pid', str(args.pid), '--single-player-confirmed',
                    '--dll', str(camera), '--capture', str(prefix) + '-tracking.png', '--output', str(camera_report), '--no-screenshots'], cwd=root,
                    capture_output=True, text=True, timeout=45, creationflags=subprocess.CREATE_NO_WINDOW)
                report['camera_loader_exit_code'] = camera_result.returncode
            report['consumer_exit_code'] = app.wait(timeout=65)
        except Exception as e:
            report['error'] = str(e)
            raise
        finally:
            if app is not None and app.poll() is None:
                app.terminate()  # only this standalone child, never AC6
                app.wait(timeout=5)
            report['stop_requested'] = request_stop(mapping, args.pid)
            # Allow bounded native hook cleanup; never terminate/unload the DLL.
            report['producer_loader_exit_code'] = loader.wait(timeout=75)
            report['producer_events'] = rows(native_log)
            result.write_text(json.dumps(report, indent=2), encoding='utf-8')
    print(json.dumps(report, indent=2))
    return 0 if report.get('consumer_exit_code') == 0 and report.get('producer_loader_exit_code') == 0 and report.get('camera_loader_exit_code', 0) == 0 else 2


if __name__ == '__main__':
    raise SystemExit(main())

import ctypes as C
from ctypes import wintypes as W
import os
from pathlib import Path
import sys
import tempfile
import unittest
import uuid
from unittest.mock import Mock, patch
from contextlib import redirect_stderr, redirect_stdout
import io
import hashlib
import json
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
from run_live_stereo_trial import request_stop, rows, consumer_command
import run_live_stereo_trial as trial


class ConsumerSelectionTests(unittest.TestCase):
    def test_explicit_build_wins_over_existing_default_and_reaches_command(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            default = root / 'native/xr_display_probe/build/Release/ac6_xr_display_probe.exe'
            default.parent.mkdir(parents=True)
            default.write_bytes(b'old fixture executable, never launched')
            current = root / 'isolated build/current.exe'
            current.parent.mkdir()
            current.write_bytes(b'current fixture executable, never launched')
            for mode in ('vr', 'monitor'):
                selected = trial.select_consumer(root, mode, current)
                self.assertEqual(selected, current.resolve())
                command = consumer_command(selected, mode, 'mapping', Path('preview'), Path('log'), 123,
                                           mode == 'vr', 2560, 1440, 90 if mode == 'vr' else 25)
                self.assertEqual(command[0], str(current.resolve()))
            self.assertEqual(trial.select_consumer(root, 'vr'), default.resolve())
            monitor = root / 'native/live_stereo/build/Release/live_stereo_monitor.exe'
            monitor.parent.mkdir(parents=True)
            monitor.write_bytes(b'monitor fixture, never launched')
            self.assertEqual(trial.select_consumer(root, 'monitor'), monitor.resolve())

    def test_invalid_override_never_falls_back_to_the_old_build(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            default = root / 'native/xr_display_probe/build/Release/ac6_xr_display_probe.exe'
            default.parent.mkdir(parents=True)
            default.write_bytes(b'old fixture')
            not_exe = root / 'current.dll'
            not_exe.write_bytes(b'wrong extension')
            directory = root / 'directory.exe'
            directory.mkdir()
            for invalid in (root / 'absent.exe', not_exe, directory):
                with self.assertRaises(ValueError):
                    trial.select_consumer(root, 'vr', invalid)
            with self.assertRaises(ValueError):
                trial.select_consumer(root, 'unsupported', default)
            with self.assertRaises(ValueError):
                trial.select_consumer(root, 'monitor')

    def test_missing_consumer_is_rejected_before_accessing_the_game(self):
        with tempfile.TemporaryDirectory() as folder:
            missing = Path(folder) / 'missing.exe'
            argv = ['run_live_stereo_trial.py', '--pid', '123', '--single-player-confirmed',
                    '--dll', 'ac6_live_pair_probe_v99.dll', '--tag', 'fixture', '--consumer-exe', str(missing)]
            with patch.object(sys, 'argv', argv), patch.object(trial, 'process_metadata') as metadata:
                errors = io.StringIO()
                with redirect_stderr(errors), self.assertRaises(SystemExit) as error:
                    trial.main()
                self.assertEqual(error.exception.code, 2)
                self.assertIn('Consumer executable', errors.getvalue())
                metadata.assert_not_called()

    def test_cli_launch_and_evidence_identify_the_same_explicit_build(self):
        # Exercise main's command/report wiring, replacing every process and
        # shared-mapping operation. Fixture bytes are never executed.
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            game, dll, current = (root / name for name in ('game.exe', 'ac6_live_pair_probe_v99.dll', 'current build.exe'))
            for path in (game, dll, current):
                path.write_bytes(path.name.encode('utf-8'))
            loader, consumer = Mock(), Mock()
            loader.wait.return_value = consumer.wait.return_value = 0
            consumer.poll.return_value = 0
            argv = ['run_live_stereo_trial.py', '--pid', '123', '--single-player-confirmed', '--dll', str(dll),
                    '--tag', 'fixture', '--mode', 'monitor', '--consumer-exe', str(current)]
            metadata = {'Path': str(game), 'Modules': ['ac6_coop.dll']}
            with patch.object(sys, 'argv', argv), patch.object(trial, '__file__', str(root / 'tools/run_live_stereo_trial.py')), \
                 patch.object(trial, 'EXPECTED_EXE', game), patch.object(trial, 'EXPECTED_SHA', hashlib.sha256(game.read_bytes()).hexdigest()), \
                 patch.object(trial, 'process_metadata', return_value=metadata), \
                 patch.object(trial, 'rows', return_value=[{'phase': 'stream_ready'}]), \
                 patch.object(trial, 'request_stop', return_value=True), \
                 patch.object(trial.subprocess, 'CREATE_NO_WINDOW', 0, create=True), \
                 patch.object(trial.subprocess, 'Popen', side_effect=[loader, consumer]) as launch, \
                 redirect_stdout(io.StringIO()):
                self.assertEqual(trial.main(), 0)
            command = launch.call_args_list[1].args[0]
            report = json.loads((root / 'evidence/fixture-result.json').read_text(encoding='utf-8'))
            self.assertEqual(command[0], str(current.resolve()))
            self.assertEqual(report['consumer_executable'], command[0])
            self.assertEqual(report['consumer_sha256'], hashlib.sha256(current.read_bytes()).hexdigest())


@unittest.skipUnless(sys.platform == 'win32', 'Windows shared mapping protocol')
class LiveTrialControlTests(unittest.TestCase):
    def test_quality_and_rotation_reach_the_selected_consumer(self):
        command = consumer_command('display.exe', 'vr', 'mapping', Path('preview'), Path('log'), 123, True, 1920, 1080, 90)
        self.assertEqual(command[command.index('--eye-width')+1], '1920')
        self.assertEqual(command[command.index('--eye-height')+1], '1080')
        self.assertEqual(command[command.index('--rotation-limit-deg')+1], '90')
        self.assertIn('--headtracking', command)
        self.assertEqual(consumer_command('monitor.exe', 'monitor', 'mapping', Path('preview'), Path('log'), 123, False, 1920, 1080, 25)[-2:], ['1920', '1080'])
        for width, height, limit, tracking, mode in [(1920,720,25,False,'vr'),(1280,720,float('nan'),True,'vr'),(1280,720,181,True,'vr'),(1280,720,90,False,'vr'),(1280,720,25,True,'monitor')]:
            with self.assertRaises(ValueError):
                consumer_command('app', mode, 'mapping', Path('p'), Path('l'), 123, tracking, width, height, limit)

    def test_stop_only_changes_request_field_in_matching_protocol(self):
        kernel = C.WinDLL('kernel32', use_last_error=True)
        kernel.CreateFileMappingW.argtypes = [W.HANDLE, C.c_void_p, W.DWORD, W.DWORD, W.DWORD, W.LPCWSTR]
        kernel.CreateFileMappingW.restype = W.HANDLE
        kernel.MapViewOfFile.argtypes = [W.HANDLE, W.DWORD, W.DWORD, W.DWORD, C.c_size_t]
        kernel.MapViewOfFile.restype = C.c_void_p
        kernel.UnmapViewOfFile.argtypes = [C.c_void_p]
        kernel.CloseHandle.argtypes = [W.HANDLE]
        name = 'Local\\AC6StreamControlTest_' + uuid.uuid4().hex
        handle = kernel.CreateFileMappingW(W.HANDLE(-1), None, 4, 0, 120, name)
        self.assertTrue(handle)
        view = kernel.MapViewOfFile(handle, 0xf001f, 0, 0, 120)
        self.assertTrue(view)
        try:
            W.LONG.from_address(view).value = 9
            W.DWORD.from_address(view + 4).value = os.getpid()
            self.assertFalse(request_stop(name), 'Wrong protocol must not be modified')
            W.LONG.from_address(view).value = 1
            before = C.string_at(view, 120)
            self.assertFalse(request_stop(name, os.getpid() + 1))
            self.assertEqual(C.string_at(view, 120), before)
            self.assertTrue(request_stop(name))
            expected = bytearray(before)
            expected[36:40] = (1).to_bytes(4, 'little')
            self.assertEqual(C.string_at(view, 120), bytes(expected))
        finally:
            kernel.UnmapViewOfFile(view)
            kernel.CloseHandle(handle)
        self.assertFalse(request_stop(name), 'Absent mapping is not an error during cleanup')

    def test_log_reader_survives_malformed_complete_lines(self):
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder) / 'log.jsonl'
            path.write_text('{"phase":"ready"}\n{"phase": "broken",}\nnull\n{"phase":"done"}\n{"phase":', encoding='utf-8')
            self.assertEqual(rows(path), [{'phase': 'ready'}, {'phase': 'done'}])

    def test_log_reader_ignores_incomplete_final_line(self):
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder) / 'log.jsonl'
            self.assertEqual(rows(path), [])
            path.write_text('{"phase":"stream_ready"}\n{"phase":', encoding='utf-8')
            self.assertEqual(rows(path), [{'phase': 'stream_ready'}])

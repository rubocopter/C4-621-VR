import ctypes as C
from ctypes import wintypes as W
import os
from pathlib import Path
import sys
import tempfile
import unittest
import uuid
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
from run_live_stereo_trial import request_stop, rows, consumer_command


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

    def test_log_reader_ignores_incomplete_final_line(self):
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder) / 'log.jsonl'
            self.assertEqual(rows(path), [])
            path.write_text('{"phase":"stream_ready"}\n{"phase":', encoding='utf-8')
            self.assertEqual(rows(path), [{'phase': 'stream_ready'}])

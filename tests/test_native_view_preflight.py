"""Preflight regressions using local ctypes callbacks, never process APIs.

Only the Windows x64 ABI is exercised. Every test owns fresh API callbacks,
dependency mocks and an in-memory output file; no game files are inspected.
"""
from contextlib import ExitStack
import ctypes as C
from ctypes import wintypes as W
import io
import json
from pathlib import Path
import sys
from types import SimpleNamespace
import unittest
from unittest.mock import Mock, mock_open, patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
import observe_native_views as observer


@unittest.skipUnless(sys.platform == "win32" and C.sizeof(C.c_void_p) == 8,
                     "Requires the Windows x64 ctypes ABI, without live APIs")
class NativeViewPreflightTests(unittest.TestCase):
    HANDLE = 0x123456789AB
    BASE = 0x140000000
    LOADER = 0x7FFB12340000
    EAC = 0x7FFA56780000
    CREATION = 0x1234567800000001
    SITES = (0x1292BFB, 0x2466510, 0x2466470, 0x2524790, 0x2524A90)

    def setUp(self):
        self.case = "ok"
        self.after_inspection = False
        self.events = []
        self.handles = []
        self.enumerations = []
        self.module_names = []
        self.reads = []
        self.closed = []
        self.image = Path(__file__).resolve().parent / "synthetic-only.exe"
        self.output = io.StringIO()

        # Output string buffers use c_void_p on the callback side: LPWSTR
        # callbacks otherwise convert the writable pointer to a Python string.
        # main() still assigns its real LPWSTR argtypes on the calling side.
        F = C.WINFUNCTYPE
        self.kernel = SimpleNamespace(
            GetTickCount64=F(C.c_ulonglong)(lambda: 0x123456789),
            OpenProcess=F(W.HANDLE, W.DWORD, W.BOOL, W.DWORD)(self.open_process),
            QueryFullProcessImageNameW=F(
                W.BOOL, W.HANDLE, W.DWORD, C.c_void_p, C.POINTER(W.DWORD)
            )(self.query_image),
            GetProcessTimes=F(
                W.BOOL, W.HANDLE, *([C.POINTER(W.FILETIME)] * 4)
            )(self.process_times),
            K32EnumProcessModulesEx=F(
                W.BOOL, W.HANDLE, C.POINTER(C.c_void_p), W.DWORD,
                C.POINTER(W.DWORD), W.DWORD
            )(self.enum_modules),
            K32GetModuleFileNameExW=F(
                W.DWORD, W.HANDLE, C.c_void_p, C.c_void_p, W.DWORD
            )(self.module_filename),
            ReadProcessMemory=F(
                W.BOOL, W.HANDLE, C.c_void_p, C.c_void_p, C.c_size_t,
                C.POINTER(C.c_size_t)
            )(self.read_memory),
            CloseHandle=F(W.BOOL, W.HANDLE)(self.close_handle),
        )
        self.inspect = Mock(side_effect=self.inspect_image)
        self.pe = Mock()
        self.pe.get_data.return_value = b"X" * 16
        self.pe_factory = Mock(return_value=self.pe)
        self.sample = Mock(return_value=dict(
            status="identities_match", count=1, identity_repeat_equal=True,
            atomic_snapshot=False, frame_associated=False,
            render_calls_observed=False,
        ))
        self.files = mock_open()
        self.sleep = Mock(side_effect=AssertionError("Unexpected sampling sleep"))
        stack = ExitStack()
        self.addCleanup(stack.close)
        dependencies = {
            "observe_camera": SimpleNamespace(
                EXPECTED_EXE=self.image, EXPECTED_SHA="synthetic-sha256"),
            "inspect_native_view_route": SimpleNamespace(inspect=self.inspect),
            "pefile": SimpleNamespace(PE=self.pe_factory),
        }
        stack.enter_context(patch.dict(sys.modules, dependencies))
        self.win_dll = stack.enter_context(patch.object(
            C, "WinDLL", return_value=self.kernel))
        stack.enter_context(patch.object(C, "get_last_error", return_value=5))
        stack.enter_context(patch.object(
            C, "WinError", return_value=OSError(5, "Synthetic Win32 failure")))
        stack.enter_context(patch.object(Path, "open", self.files))
        stack.enter_context(patch.object(observer, "sample_views", self.sample))
        stack.enter_context(patch.object(observer.time, "monotonic", side_effect=[0, 1]))
        stack.enter_context(patch.object(observer.time, "sleep", self.sleep))
        stack.enter_context(patch.object(sys, "stdout", self.output))
        stack.enter_context(patch.object(sys, "argv", [
            "synthetic-preflight", "--pid", "42", "--single-player-confirmed",
            "--seconds", "0.01", "--output", "synthetic-output.jsonl",
        ]))

    def note_handle(self, api, handle):
        self.events.append(api)
        self.handles.append((api, handle))

    @staticmethod
    def write_wide(buffer, value):
        C.memmove(buffer, C.create_unicode_buffer(value),
                  (len(value) + 1) * C.sizeof(C.c_wchar))

    def open_process(self, access, inherit, pid):
        self.events.append(("open", access, inherit, pid))
        return self.HANDLE

    def query_image(self, handle, flags, buffer, length):
        self.note_handle("query", handle)
        if flags != 0 or length[0] < len(str(self.image)) + 1:
            return 0
        self.write_wide(buffer, str(self.image))
        length[0] = len(str(self.image))
        return 1

    def process_times(self, handle, creation, exit_time, kernel_time, user_time):
        self.note_handle("times", handle)
        value = self.CREATION
        if self.after_inspection and self.case == "creation_changed":
            value += 1
        creation[0].dwLowDateTime = value & 0xFFFFFFFF
        creation[0].dwHighDateTime = value >> 32
        return 1

    def enum_modules(self, handle, array, size, needed, flags):
        self.note_handle("enumerate", handle)
        self.enumerations.append((handle, size, flags))
        if self.case == "enum_failure" or size != 32768 or flags != 3:
            return 0
        modules = [self.BASE, self.LOADER]
        if self.after_inspection:
            if self.case == "new_eac":
                modules.append(self.EAC)
            elif self.case == "loader_removed":
                modules.pop()
        for index, module in enumerate(modules):
            array[index] = module
        needed[0] = {
            "oversized": size + 8, "partial_count": 9, "zero_modules": 0,
        }.get(self.case, len(modules) * 8)
        return 1

    def module_filename(self, handle, module, buffer, length):
        self.note_handle("module_name", handle)
        self.module_names.append((module, length))
        if self.case == "name_failure":
            return 0
        if self.case == "name_truncated":
            return length
        paths = {
            self.BASE: self.image,
            self.LOADER: self.image.with_name("ac6_coop.dll"),
            self.EAC: self.image.with_name("EasyAntiCheat_EOS.dll"),
        }
        path = paths.get(module)
        if path is None:
            return 0
        if module == self.BASE and self.case == "wrong_primary":
            path = self.image.with_name("other.exe")
        value = str(path)
        if length <= len(value):
            return 0
        self.write_wide(buffer, value)
        return len(value)

    def read_memory(self, handle, address, buffer, size, received):
        self.note_handle("read", handle)
        self.reads.append((address, size))
        if address not in {self.BASE + site for site in self.SITES} or size != 16:
            return 0
        C.memmove(buffer, b"X" * 16, 16)
        received[0] = 16
        return 1

    def close_handle(self, handle):
        self.note_handle("close", handle)
        self.closed.append(handle)
        return 1

    def inspect_image(self, path):
        self.events.append("inspect")
        self.after_inspection = True
        return dict(executable_sha256="synthetic-sha256", all_checks_verified=True)

    def assert_pinned_and_closed(self):
        self.assertEqual(self.events[0], ("open", 0x410, 0, 42))
        self.assertTrue(self.handles)
        self.assertTrue(all(handle == self.HANDLE for _, handle in self.handles),
                        self.handles)
        self.assertEqual(self.closed, [self.HANDLE])
        self.assertEqual(self.events[-1], "close")

    def assert_refused(self, case, message, after_inspection=False, error=ValueError):
        self.case = case
        with self.assertRaisesRegex(error, message):
            observer.main()
        self.assert_pinned_and_closed()
        self.assertEqual(self.reads, [])
        self.sample.assert_not_called()
        self.pe_factory.assert_not_called()
        self.files.assert_not_called()
        self.sleep.assert_not_called()
        self.assertEqual(self.inspect.call_count, int(after_inspection))
        self.assertEqual(len(self.enumerations), 2 if after_inspection else 1)

    def test_same_high_bit_handle_and_module_bases_survive_both_enumerations(self):
        self.assertEqual(observer.main(), 0)
        self.assert_pinned_and_closed()
        self.assertEqual(self.enumerations, [(self.HANDLE, 32768, 3)] * 2)
        self.assertEqual(self.module_names,
                         [(self.BASE, 32768), (self.LOADER, 32768)] * 2)
        self.assertEqual(self.events.count("query"), 2)
        self.assertEqual(self.events.count("times"), 2)
        self.assertLess(self.events.index("enumerate"), self.events.index("inspect"))
        self.assertLess(self.events.index("inspect"),
                        len(self.events) - 1 - self.events[::-1].index("enumerate"))
        self.assertCountEqual(self.reads, [(self.BASE + site, 16) for site in self.SITES])
        self.inspect.assert_called_once_with(self.image)
        self.assertEqual(self.sample.call_count, 1)
        self.assertEqual(self.sample.call_args.args[1], self.BASE)
        self.pe.close.assert_called_once_with()
        metadata = json.loads(self.files().write.call_args_list[0].args[0])
        self.assertEqual(metadata["creation_time"], self.CREATION)
        self.assertEqual(metadata["module_base"], "0x140000000")
        self.assertEqual(metadata["process_access"], "0x410")
        self.assertFalse(metadata["game_writes"])
        self.assertFalse(metadata["hooks_installed"])
        self.assertFalse(metadata["frame_associated"])
        summary = json.loads(self.output.getvalue())
        self.assertEqual(summary["samples"], 1)
        self.assertEqual(summary["statuses"], {"identities_match": 1})
        self.sleep.assert_not_called()

    def test_resource_mode_uses_same_handle_and_verifies_additional_anchor(self):
        extra = 0x1234560
        self.SITES = self.SITES + (extra,)
        route = sys.modules["inspect_native_view_route"]
        route.OWNERSHIP_CHECKS = [("fixture", extra, extra, "mov", None, "")]
        resource_sample = Mock(return_value=dict(status="resource_identities_match", frame_associated=False))
        with patch.dict(sys.modules, {"native_view_resources": SimpleNamespace(sample_resources=resource_sample)}):
            with patch.object(sys, "argv", sys.argv + ["--resources"]):
                self.assertEqual(observer.main(), 0)
        self.assert_pinned_and_closed()
        self.assertIn((self.BASE+extra,16), self.reads)
        resource_sample.assert_called_once()
        self.sample.assert_not_called()
        metadata=json.loads(self.files().write.call_args_list[0].args[0])
        self.assertEqual(metadata["schema"],"native_view_resources_v1")
        self.assertFalse(metadata["game_writes"])

    def test_resource_mode_refuses_code_mismatch_before_sampling(self):
        extra = 0x1234560
        sys.modules["inspect_native_view_route"].OWNERSHIP_CHECKS = [("fixture",extra,extra,"mov",None,"")]
        # Extra site isn't readable by this fixture: preflight must fail closed.
        resource_sample=Mock()
        with patch.dict(sys.modules,{"native_view_resources":SimpleNamespace(sample_resources=resource_sample)}):
            with patch.object(sys,"argv",sys.argv+["--resources"]):
                with self.assertRaises(OSError):observer.main()
        self.assert_pinned_and_closed()
        resource_sample.assert_not_called()
        self.files.assert_not_called()

    def test_failed_module_enumeration_refuses_and_closes(self):
        self.assert_refused("enum_failure", "Synthetic Win32 failure", error=OSError)

    def test_oversized_module_count_refuses_and_closes(self):
        self.assert_refused("oversized", "Incomplete module enumeration")

    def test_partial_module_handle_count_refuses_and_closes(self):
        self.assert_refused("partial_count", "Incomplete module enumeration")

    def test_empty_module_enumeration_refuses_and_closes(self):
        self.assert_refused("zero_modules", "Incomplete module enumeration")

    def test_failed_module_name_refuses_and_closes(self):
        self.assert_refused("name_failure", "Incomplete module identity")

    def test_truncated_module_name_refuses_and_closes(self):
        self.assert_refused("name_truncated", "Incomplete module identity")

    def test_wrong_primary_module_refuses_and_closes(self):
        self.assert_refused("wrong_primary", "Primary module does not match executable")

    def test_creation_change_after_inspection_refuses_before_memory_reads(self):
        self.assert_refused("creation_changed", "Process instance changed", True)

    def test_new_eac_after_inspection_refuses_before_memory_reads(self):
        self.assert_refused("new_eac", "Anti-cheat module present", True)

    def test_loader_removal_after_inspection_refuses_before_memory_reads(self):
        self.assert_refused("loader_removed", "Expected offline mod host absent", True)


if __name__ == "__main__":
    unittest.main()

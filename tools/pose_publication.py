"""Local named-memory transport for a bounded tracking experiment.

Windows mutex protects a 64-byte packet. Game callback uses a zero-timeout lock
and a short-lived cached pose, never waiting for OpenXR or the Python producer.
"""

import ctypes as C
import mmap
import struct

from camera_math import compose_head_orientation, relative_head_quaternion

IDENTITY = [1.,0.,0.,0., 0.,1.,0.,0., 0.,0.,1.,0., 0.,0.,0.,1.]


class PosePublication:
    def __init__(self, pid, flip_z=False):
        self.kernel = C.WinDLL("kernel32", use_last_error=True)
        k = self.kernel
        for name, result, args in [
            ("CreateMutexW", C.c_void_p, [C.c_void_p, C.c_bool, C.c_wchar_p]),
            ("WaitForSingleObject", C.c_uint32, [C.c_void_p, C.c_uint32]),
            ("ReleaseMutex", C.c_bool, [C.c_void_p]),
            ("CloseHandle", C.c_bool, [C.c_void_p]),
            ("GetTickCount64", C.c_uint64, [])]:
            function = getattr(k, name)
            function.restype, function.argtypes = result, args
        self.mutex = k.CreateMutexW(None, False, f"Local\\AC6VRPoseMutex-{pid}")
        if not self.mutex:
            raise C.WinError(C.get_last_error())
        self.memory = mmap.mmap(-1, 128, tagname=f"Local\\AC6VRPose-{pid}", access=mmap.ACCESS_WRITE)
        self.reference = None
        self.flip_z = flip_z
        self.sequence = 0
        self._write(False, [1,0,0,0,1,0,0,0,1])

    def _write(self, valid, delta):
        waited = self.kernel.WaitForSingleObject(self.mutex, 1000)
        if waited not in (0, 0x80):
            raise RuntimeError("Timed out publishing tracking pose")
        try:
            self.sequence += 1
            packet = struct.pack("<IIIIQ9f", 0x36565241, 1, int(valid), self.sequence,
                                 self.kernel.GetTickCount64(), *delta)
            self.memory.seek(0)
            self.memory.write(packet)
        finally:
            self.kernel.ReleaseMutex(self.mutex)

    def update(self, quaternion, valid):
        if not valid:
            self._write(False, [1,0,0,0,1,0,0,0,1])
            return
        if self.reference is None:
            self.reference = list(quaternion)
        relative = relative_head_quaternion(self.reference, quaternion, max_degrees=25)
        matrix = compose_head_orientation(IDENTITY, [0,0,0,1], relative, flip_z=self.flip_z)
        self._write(True, [matrix[i] for i in (0,1,2,4,5,6,8,9,10)])

    def close(self):
        try:
            self._write(False, [1,0,0,0,1,0,0,0,1])
        finally:
            self.memory.close()
            self.kernel.CloseHandle(self.mutex)

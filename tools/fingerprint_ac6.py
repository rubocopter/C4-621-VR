"""Read-only executable/parameter fingerprints for the local AC6 VR investigation.

Runs with the Python standard library; does not launch or modify the game.
"""

from __future__ import annotations

import hashlib
import mmap
from pathlib import Path
import struct


GAME = Path(r"C:\Program Files (x86)\Steam\steamapps\common\ARMORED CORE VI FIRES OF RUBICON\Game")
ROOT = Path(__file__).resolve().parents[1]
FILES = (
    GAME / "armoredcore6.exe",
    GAME / "ac6_for_coop_launcher.exe",
    GAME / "AC6Coop" / "ac6_coop.dll",
    GAME / "regulation.bin",
    GAME / "regulation.bin.bck",
    ROOT / "mods" / "FIrst Person Camera" / "regulation.bin",
)


def sha256_file(path: Path) -> str:
    hasher = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            hasher.update(chunk)
    return hasher.hexdigest()


def import_dlls(path: Path) -> list[str]:
    with path.open("rb") as stream, mmap.mmap(stream.fileno(), 0, access=mmap.ACCESS_READ) as data:
        if data[:2] != b"MZ":
            return []
        pe = struct.unpack_from("<I", data, 0x3C)[0]
        if data[pe : pe + 4] != b"PE\0\0":
            return []
        sections_count = struct.unpack_from("<H", data, pe + 6)[0]
        optional_size = struct.unpack_from("<H", data, pe + 20)[0]
        optional = pe + 24
        magic = struct.unpack_from("<H", data, optional)[0]
        directories = optional + (112 if magic == 0x20B else 96)
        if magic not in (0x10B, 0x20B):
            return []
        imports_rva, _ = struct.unpack_from("<II", data, directories + 8)
        sections = []
        for i in range(sections_count):
            entry = optional + optional_size + i * 40
            virtual_size, virtual_address, raw_size, raw_ptr = struct.unpack_from("<IIII", data, entry + 8)
            sections.append((virtual_address, max(virtual_size, raw_size), raw_ptr))

        def offset(rva: int) -> int:
            for address, size, raw in sections:
                if address <= rva < address + size:
                    return raw + rva - address
            raise ValueError(f"RVA outside sections: 0x{rva:x}")

        if not imports_rva:
            return []
        descriptors = offset(imports_rva)
        imports = []
        for i in range(2048):
            record = struct.unpack_from("<IIIII", data, descriptors + i * 20)
            if not any(record):
                break
            name_start = offset(record[3])
            name_end = data.find(b"\0", name_start, name_start + 260)
            if name_end < 0:
                raise ValueError("Unterminated import DLL name")
            imports.append(data[name_start:name_end].decode("ascii", errors="replace"))
        return imports


if __name__ == "__main__":
    for path in FILES:
        if not path.is_file():
            print(f"MISSING {path}")
            continue
        print(f"{path}\n  bytes={path.stat().st_size}\n  sha256={sha256_file(path)}")
        if path.suffix.lower() in (".exe", ".dll"):
            print("  imports=" + ", ".join(import_dlls(path)))

"""Compare published IGCS AC6 signatures with a local PE, without executing it.

Reports file offsets, preferred addresses and PE unwind entries. A signature
match establishes bytes only: camera identity and live pointers remain unproven.
Pattern strings are read from an unmodified, credited reference checkout.
"""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import re
import struct


ROOT = Path(__file__).resolve().parents[1]
DEFAULT_GAME = Path(r"C:\Program Files (x86)\Steam\steamapps\common\ARMORED CORE VI FIRES OF RUBICON\Game\armoredcore6.exe")
REFERENCE = ROOT / "reference/IGCS-GITC/Cameras/Armored Core 6/InjectableGenericCameraSystem/InterceptorHelper.cpp"


def inspect(binary: Path, source: Path) -> dict:
    data = binary.read_bytes()
    pe = struct.unpack_from("<I", data, 0x3C)[0]
    if data[:2] != b"MZ" or data[pe:pe + 4] != b"PE\0\0":
        raise ValueError("Expected PE image")
    count, = struct.unpack_from("<H", data, pe + 6)
    optional_size, = struct.unpack_from("<H", data, pe + 20)
    optional = pe + 24
    if struct.unpack_from("<H", data, optional)[0] != 0x20B:
        raise ValueError("Expected PE32+ image")
    image_base, = struct.unpack_from("<Q", data, optional + 24)
    sections = []
    for i in range(count):
        at = optional + optional_size + i * 40
        name = data[at:at + 8].split(b"\0")[0].decode("ascii", errors="replace")
        vsize, rva, raw_size, raw_offset = struct.unpack_from("<IIII", data, at + 8)
        flags, = struct.unpack_from("<I", data, at + 36)
        sections.append(dict(name=name, rva=rva, vsize=vsize, raw_size=raw_size,
                             raw_offset=raw_offset, executable=bool(flags & 0x20000000)))

    def raw_for_rva(rva):
        for s in sections:
            if s["rva"] <= rva < s["rva"] + s["raw_size"]:
                return s["raw_offset"] + rva - s["rva"]
        raise ValueError(f"RVA is not file backed: {rva:x}")

    exception_rva, exception_size = struct.unpack_from("<II", data, optional + 112 + 3 * 8)
    runtime_functions = []
    if exception_rva:
        exception_raw = raw_for_rva(exception_rva)
        for at in range(exception_raw, exception_raw + exception_size - 11, 12):
            start, end, unwind = struct.unpack_from("<III", data, at)
            if start < end:
                runtime_functions.append((start, end, unwind))

    results = []
    for line in source.read_text(encoding="utf-8-sig").splitlines():
        if line.lstrip().startswith("//"):
            continue
        match = re.search(r'new AOBBlock\((\w+),\s*"([^"]+)",\s*(\d+)\)', line)
        if not match:
            continue
        key, pattern, occurrence = match.groups()
        tokens = pattern.split()
        marker = tokens.index("|") if "|" in tokens else 0
        tokens = [t for t in tokens if t != "|"]
        expression = b"".join(b"." if t == "??" else re.escape(bytes.fromhex(t)) for t in tokens)
        regex = re.compile(b"(?=(" + expression + b"))", re.DOTALL)
        hits = []
        for s in sections:
            if not s["executable"]:
                continue
            start = s["raw_offset"]
            end = start + s["raw_size"]
            for found in regex.finditer(data, start, end):
                raw = found.start() + marker
                rva = s["rva"] + raw - start
                functions = [dict(start_rva=hex(a), end_rva_exclusive=hex(b),
                                  preferred_start=hex(image_base + a), unwind_rva=hex(u))
                             for a, b, u in runtime_functions if a <= rva < b]
                hits.append(dict(file_offset=hex(raw), rva=hex(rva),
                                 preferred_va=hex(image_base + rva), section=s["name"],
                                 bytes_at_site=data[raw:raw + 48].hex(" "),
                                 containing_unwind_entries=functions))
        results.append(dict(key=key, pattern=pattern, source_occurrence=int(occurrence),
                            match_count=len(hits), matches=hits))
    return dict(binary=str(binary), sha256=hashlib.sha256(data).hexdigest(),
                image_base=hex(image_base), reference=str(source),
                reference_sha256=hashlib.sha256(source.read_bytes()).hexdigest(),
                scope="Static bytes in executable PE sections; no live camera validation or writes",
                results=results)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=Path, default=DEFAULT_GAME)
    parser.add_argument("--source", type=Path, default=REFERENCE)
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    result = inspect(args.binary, args.source)
    if args.output:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    for row in result["results"]:
        sites = ", ".join(h["rva"] for h in row["matches"])
        print(f"{row['key']}: {row['match_count']} match(es) {sites}")

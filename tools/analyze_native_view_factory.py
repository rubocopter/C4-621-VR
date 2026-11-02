"""Validate a bounded factory snapshot; captured pointers are never replayable."""
import argparse
import hashlib
import json
from pathlib import Path
import re

from analyze_native_view_trace import decode_rows, uint


def pointer(value, optional=False):
    return uint(value) and ((optional and value == 0) or
                           (0x10000 <= value <= 0x7FFFFFFFFFFF and value % 8 == 0))


def analyze(rows, census_rows):
    errors = []
    report = dict(factory_arguments_observed=False, result_matches_main_holder=False,
                  frame_associated=False, gpu_execution_proven=False,
                  second_view_constructed=False, safe_to_replay=False, errors=errors)
    if (not isinstance(rows, list) or len(rows) != 4 or
            any(not isinstance(r, dict) for r in rows)):
        errors.append("exactly four object rows required")
        return report
    expected = ("observing", "restoration", "factory_event", "disabled")
    if tuple(row.get("phase") for row in rows) != expected:
        errors.append("invalid phase sequence")
        return report
    header, restored, event, summary = rows
    if (header.get("schema") != "native_view_factory_trace_v1" or
            not pointer(header.get("image_base")) or not uint(header.get("pid"), True) or
            not uint(header.get("process_creation"), True) or not uint(header.get("closed_tick_ms"), True)):
        errors.append("invalid factory header")
    if not uint(restored.get("rva")) or restored["rva"] != 0x22F0C50 or restored.get("restored") is not True:
        errors.append("factory restoration required")
    if any(summary.get(key) != "MH_OK" for key in ("status", "disable_status")):
        errors.append("hook lifecycle failed")
    if (summary.get("hooks_restored") is not True or summary.get("dll_retained_inactive") is not True
            or summary.get("frame_associated") is not False):
        errors.append("inactive restored observer required")
    for key, value in (("attempts", 1), ("completed", 1), ("pending_at_shutdown", 0)):
        if not uint(summary.get(key)) or summary[key] != value:
            errors.append("invalid summary " + key)
    for key in ("seq", "thread", "valid"):
        if not uint(event.get(key), True):
            errors.append("invalid event " + key)
    if event.get("seq") != 1 or event.get("valid") != 15:
        errors.append("incomplete capture")
    for key in ("factory", "descriptor_address", "auxiliary_address", "result", "view_vtable",
                "targets", "scene", "camera", "pipeline_owner", "pipeline_state"):
        if not pointer(event.get(key), optional=key == "camera"):
            errors.append("invalid pointer " + key)
    if uint(header.get("image_base")) and event.get("view_vtable") != header["image_base"] + 0x3E3E7E0:
        errors.append("unexpected returned view type")
    for key, size in (("descriptor_hex", 0x70), ("auxiliary_hex", 0x18), ("target_params_hex", 0x78)):
        value = event.get(key)
        if type(value) is not str or re.fullmatch("[0-9a-fA-F]{%d}" % (2 * size), value) is None:
            errors.append("invalid payload " + key)
    if errors:
        return report
    report["factory_arguments_observed"] = True
    report["payload_sha256"] = {key: hashlib.sha256(bytes.fromhex(event[key])).hexdigest()
                                for key in ("descriptor_hex", "auxiliary_hex", "target_params_hex")}
    report["result"] = hex(event["result"])
    # This is a repeated pointer relationship in one process, not lifetime or
    # ABA proof. Never fill a missing current holder from a nearby capture.
    if not isinstance(census_rows, list) or not census_rows or any(not isinstance(r, dict) for r in census_rows):
        return report
    metadata = [r for r in census_rows if r.get("kind") == "metadata"]
    samples = [r for r in census_rows if r.get("kind") == "sample"]
    if len(metadata) != 1 or not samples or len(samples) > 128 or len(metadata) + len(samples) != len(census_rows):
        return report
    meta = metadata[0]
    if (meta.get("schema") != "native_view_resources_v1" or not uint(meta.get("pid")) or
            not uint(meta.get("creation_time")) or meta["pid"] != header["pid"] or
            meta["creation_time"] != header["process_creation"] or
            meta.get("module_base") != hex(header["image_base"]) or
            meta.get("process_access") != "0x410" or meta.get("game_writes") is not False or
            meta.get("hooks_installed") is not False or meta.get("frame_associated") is not False):
        return report
    report["result_matches_main_holder"] = all(
        sample.get("status") == "resource_identities_match" and sample.get("identity_repeat_equal") is True
        and sample.get("main_view") == hex(event["result"])
        and uint(sample.get("sample_begin_tick_ms"), True)
        and sample["sample_begin_tick_ms"] > header["closed_tick_ms"]
        and all(sample.get(k) is False for k in
                ("atomic_snapshot", "frame_associated", "gpu_resource_identity_proven", "independent_allocations_proven"))
        for sample in samples)
    report["holder_samples"] = len(samples)
    return report


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("trace", type=Path)
    parser.add_argument("--census", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    data = []
    for path in (args.trace, args.census):
        if path.stat().st_size > 4 * 1024 * 1024:
            parser.error("evidence exceeds 4 MiB")
        data.append(path.read_bytes())
    try:
        report = analyze(*(decode_rows(raw) for raw in data))
    except (UnicodeError, ValueError) as exc:
        parser.error("invalid JSONL: " + str(exc))
    report.update(trace_sha256=hashlib.sha256(data[0]).hexdigest(), census_sha256=hashlib.sha256(data[1]).hexdigest())
    with args.output.open("x", encoding="utf-8") as output:
        json.dump(report, output, indent=2)
        output.write("\n")
    print(json.dumps(report, indent=2))
    return 0 if report["factory_arguments_observed"] and report["result_matches_main_holder"] else 1


if __name__ == "__main__":
    raise SystemExit(main())

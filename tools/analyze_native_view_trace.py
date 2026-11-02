"""Validate bounded CPU scopes; these records cannot establish GPU provenance."""
import argparse
import hashlib
import json
from pathlib import Path

HOOKS = {0x2466510, 0x2524790}
MAX_U64 = (1 << 64) - 1


def uint(value, positive=False):
    return type(value) is int and (1 if positive else 0) <= value <= MAX_U64


def decode_rows(raw):
    def object_from_pairs(pairs):
        result = {}
        for key, value in pairs:
            if key in result:
                raise ValueError("duplicate JSON key: " + key)
            result[key] = value
        return result
    def reject_constant(value):
        raise ValueError("nonfinite JSON constant: " + value)
    return [json.loads(line, object_pairs_hook=object_from_pairs, parse_constant=reject_constant)
            for line in raw.decode("utf-8").splitlines() if line.strip()]


def analyze(rows):
    errors, chain_errors = [], []
    report = dict(recording_complete=False, native_scope_chain_observed=False,
                  main_scope_count=0, gpu_execution_proven=False, frame_associated=False,
                  errors=errors, chain_errors=chain_errors)
    if not isinstance(rows, list) or any(not isinstance(r, dict) for r in rows):
        errors.append("rows must be JSON objects")
        return report
    headers = [r for r in rows if r.get("phase") == "observing"]
    footers = [r for r in rows if r.get("phase") == "disabled"]
    events = [r for r in rows if r.get("phase") == "event"]
    restores = [r for r in rows if r.get("phase") == "restoration"]
    if any(type(r.get("phase")) is not str or r["phase"] not in
           {"observing", "disabled", "event", "restoration"} for r in rows):
        errors.append("unknown row phase")
    if len(headers) != 1 or len(footers) != 1:
        errors.append("exactly one header and summary required")
        return report
    header, footer = headers[0], footers[0]
    if rows[0] is not header or rows[-1] is not footer:
        errors.append("header and summary must bound the recording")
    if header.get("schema") != "native_view_trace_v1":
        errors.append("unsupported schema")
    for field in ("image_base", "pid", "expected_list", "expected_main_view"):
        if not uint(header.get(field), True):
            errors.append("invalid header " + field)
    for field in ("saved_events", "event_attempts", "event_overflow", "pending_at_shutdown",
                  "completed_frames", "recording_cutoff", "first_loss_sequence",
                  "unmatched_lists", "unscoped_views"):
        if not uint(footer.get(field)):
            errors.append("invalid summary " + field)
    if errors:
        return report
    for field in ("event_overflow", "pending_at_shutdown", "first_loss_sequence",
                  "unmatched_lists", "unscoped_views"):
        if footer[field] != 0:
            errors.append("nonzero " + field)
    if footer.get("status") != "MH_OK" or footer.get("disable_status") != "MH_OK":
        errors.append("hook lifecycle did not succeed")
    if footer.get("hooks_restored") is not True or footer.get("dll_retained_inactive") is not True:
        errors.append("restored inactive hooks required")
    if footer.get("frame_associated") is not False:
        errors.append("producer must not claim frame association")
    if (len(restores) != 2 or any(not uint(r.get("rva")) for r in restores)
            or {r.get("rva") for r in restores} != HOOKS
            or any(r.get("restored") is not True for r in restores)):
        errors.append("both restoration rows required")
    if not len(events) == footer["saved_events"] == footer["event_attempts"] or len(events) > 128:
        errors.append("event count mismatch or capacity exceeded")
    numeric = ("kind", "begin", "end", "parent", "thread", "list", "view", "context",
               "vtable", "targets", "scene", "camera", "valid")
    for event in events:
        if any(not uint(event.get(key)) for key in numeric):
            errors.append("invalid event integer")
            continue
        if (event["kind"] not in (1, 2) or not 0 < event["begin"] < event["end"]
                or event["end"] >= footer["recording_cutoff"]):
            errors.append("invalid event interval or kind")
    if errors:
        return report
    ordinals = [e[k] for e in events for k in ("begin", "end")]
    if len(set(ordinals)) != len(ordinals):
        errors.append("duplicate scope ordinal")
    if sorted(ordinals) != list(range(1, 2 * len(events) + 1)) or footer["recording_cutoff"] != 2 * len(events) + 1:
        errors.append("scope ordinals must cover the complete prefix before cutoff")
    threads = {e["thread"] for e in events}
    for thread in threads:
        stack = []
        for event in sorted((e for e in events if e["thread"] == thread), key=lambda e: e["begin"]):
            while stack and stack[-1] < event["begin"]:
                stack.pop()
            if stack and event["end"] >= stack[-1]:
                errors.append("crossing callback intervals on one thread")
            stack.append(event["end"])
    roots = {e["begin"]: e for e in events if e["kind"] == 1}
    for thread in threads:
        previous_end = 0
        for root in sorted((e for e in roots.values() if e["thread"] == thread), key=lambda e: e["begin"]):
            if root["begin"] <= previous_end:
                errors.append("flat RenderAll roots overlap on one thread")
            previous_end = max(previous_end, root["end"])
    if footer["completed_frames"] != len(roots):
        errors.append("completed scope count mismatch")
    if len(roots) > 4:
        errors.append("four-scope limit exceeded")
    report["recording_complete"] = not errors
    children = {key: [] for key in roots}
    for root in roots.values():
        if (root["parent"] != 0 or root["list"] != header["expected_list"]
                or root["view"] != 0 or not root["context"] or not root["thread"]):
            chain_errors.append("invalid root scope")
    for child in (e for e in events if e["kind"] == 2):
        parent = roots.get(child["parent"])
        if parent is None:
            chain_errors.append("view without recorded parent")
            continue
        if not (parent["begin"] < child["begin"] < child["end"] < parent["end"]):
            chain_errors.append("view outside parent interval")
        if any(child[key] != parent[key] for key in ("thread", "context", "list")):
            chain_errors.append("view identity differs from parent")
        if (child["valid"] != 15 or not all(child[k] for k in ("view", "targets", "scene", "camera"))
                or child["vtable"] != header["image_base"] + 0x3E3E7E0):
            chain_errors.append("invalid composite view snapshot")
        if child["view"] == header["expected_main_view"]:
            children[parent["begin"]].append(child)
    report["main_scope_count"] = sum(len(group) == 1 for group in children.values())
    if any(len(group) != 1 for group in children.values()):
        chain_errors.append("each root must contain exactly one main-view invocation")
    report["native_scope_chain_observed"] = bool(roots) and not errors and not chain_errors
    return report


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("trace", type=Path)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    if args.trace.stat().st_size > 4 * 1024 * 1024:
        parser.error("trace exceeds 4 MiB")
    raw = args.trace.read_bytes()
    try:
        rows = decode_rows(raw)
    except (UnicodeError, ValueError) as exc:
        parser.error("invalid JSONL: " + str(exc))
    result = analyze(rows)
    result.update(source=str(args.trace.resolve()), source_sha256=hashlib.sha256(raw).hexdigest())
    with args.output.open("x", encoding="utf-8") as stream:
        json.dump(result, stream, indent=2)
        stream.write("\n")
    print(json.dumps(result, indent=2))
    return 0 if result["native_scope_chain_observed"] else 1


if __name__ == "__main__":
    raise SystemExit(main())

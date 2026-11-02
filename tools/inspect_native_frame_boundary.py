"""Read-only, fingerprint-gated AC6 CPU frame/render boundary anchor census.

The output verifies reachable instructions in the selected static CFGs. It
cannot prove same-path execution, task scheduling, GPU completion or safe replay.
Never starts a process or calls into the game.
"""
from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path

import pefile

from inspect_native_view_route import (DEFAULT_IMAGE, EXPECTED_SHA256, Image,
                                       Section, verify)


# Human-reviewed RVAs for one exact PE. Entries are intentionally limited to
# relevant blocks; no all-executable disassembly or heuristic RVA fallback.
# (name, root, site, mnemonic, direct call target, exact operands)
FRAME_ANCHORS = (
    ("first_context_getter", 0x1292B94, 0x1292B99, "call", 0x23A87C0, None),
    ("first_context_identity", 0x1292B94, 0x1292B9E, "mov", None, "rbx, rax"),
    ("first_context_active_set", 0x1292B94, 0x1292BA1, "mov", None, "byte ptr [rax + 0x25f0], 1"),
    ("manager_nonnull_branch", 0x1292B94, 0x1292C05, "je", 0x1292C0F, None),
    ("pass_first_context", 0x1292B94, 0x1292C07, "mov", None, "rdx, rbx"),
    ("manager_render_call", 0x1292B94, 0x1292C0A, "call", 0x15194D0, None),
    ("first_context_close", 0x1292B94, 0x1292C12, "call", 0x23A89B0, None),
    ("next_context_getter", 0x1292B94, 0x1292C17, "call", 0x23A87C0, None),
    ("next_context_identity", 0x1292B94, 0x1292C23, "mov", None, "rbx, rax"),
    ("next_manager_call", 0x1292B94, 0x1292C2E, "call", 0x1519240, None),
    ("next_context_close", 0x1292B94, 0x1292CD9, "call", 0x23A89B0, None),
)

RENDER_ANCHORS = (
    ("save_frame_context", 0x15194D0, 0x15194FE, "mov", None, "rsi, rdx"),
    ("prepare_owner_field", 0x15194D0, 0x1519504, "mov", None, "rcx, qword ptr [rcx + 0x10]"),
    ("optional_owner_prepass", 0x15194D0, 0x151950D, "call", 0x151D580, None),
    ("shared_frame_context_store", 0x15194D0, 0x1519519, "mov", None, "qword ptr [rbx + 0x30], rsi"),
    ("pre_view_state_set", 0x15194D0, 0x1519557, "call", 0x1297340, None),
    ("view_context_arg", 0x15194D0, 0x151955C, "lea", None, "rdx, [rbx + 0x30]"),
    ("native_view_list", 0x15194D0, 0x1519560, "mov", None, "rcx, qword ptr [rdi + 8]"),
    ("native_view_render", 0x15194D0, 0x1519564, "call", 0x2466510, None),
    ("post_view_state_clear", 0x15194D0, 0x15195A3, "call", 0x1297350, None),
    ("optional_post_views", 0x15194D0, 0x15195A8, "cmp", None, "qword ptr [rdi + 0x28], 0"),
    ("post_view_dispatch", 0x15194D0, 0x15196C6, "call", 0x321BBF0, None),
    ("post_view_context_update", 0x15194D0, 0x1519724, "call", 0x237A6F0, None),
    ("post_view_context_finish", 0x15194D0, 0x1519733, "call", 0x237C430, None),
)

STATE_ANCHORS = (
    ("render_state_set", 0x1297340, 0x1297340, "mov", None, "dword ptr [rcx + 0xcc], 1"),
    ("render_state_clear", 0x1297350, 0x1297350, "mov", None, "dword ptr [rcx + 0xcc], 0"),
    ("close_may_be_conditional", 0x23A89B0, 0x23A89B0, "test", None, "rcx, rcx"),
    ("context_active_clear", 0x23A89B0, 0x23A89E3, "mov", None, "byte ptr [rbx + 0x25f0], 0"),
    ("context_close_dispatch_a", 0x23A89B0, 0x23A89F8, "call", 0x22DBBF0, None),
    ("context_close_dispatch_b", 0x23A89B0, 0x23A8A07, "call", 0x22DBE60, None),
)

PREPASS_ANCHORS = (
    ("prepass_view_holder_read", 0x151D580, 0x151D5B3, "mov", None, "rcx, qword ptr [rbx + 0x3c8]"),
    ("prepass_view_target_dispatch", 0x151D580, 0x151D5CE, "call", None, "qword ptr [rax + 0x18]"),
    ("prepass_target_byte_clear", 0x151D580, 0x151D5E7, "mov", None, "byte ptr [rax + 0xc16e], 0"),
    ("prepass_owner_counter_decrement", 0x151D580, 0x151D6B7, "mov", None, "dword ptr [rbx + rax*4 + 0x780], ecx"),
    ("prepass_owner_flag_set", 0x151D580, 0x151D705, "mov", None, "byte ptr [rbx + 0x790], 1"),
    ("prepass_conditional_dispatch", 0x151D580, 0x151D700, "call", 0x231EAF0, None),
)

# Follow the conditional prepass calls into the shared four-slot state helper.
# A direct jump from 0x231eaf0 reaches 0x23f44f0. All are static RVA labels,
# not inferred eye IDs, GPU resources, or an assertion that a branch executes.
DISPATCH_ANCHORS = (
    ("prepass_optional_aux", 0x151D580, 0x151D5AE, "call", 0x23891D0, None),
    ("aux_global_gate", 0x23891D0, 0x23891F4, "cmp", 0x4D6E371, None),
    ("aux_global_owner", 0x23891D0, 0x23891FD, "mov", 0x4F71B70, None),
    ("aux_global_dispatch", 0x23891D0, 0x238922A, "call", 0x2387580, None),
    ("aux_conditional_finish", 0x23891D0, 0x238924F, "call", 0x249FBB0, None),
    ("aux_fallback", 0x23891D0, 0x2389256, "call", 0x238A620, None),
    ("prepass_owner_gate", 0x151D580, 0x151D5EE, "cmp", None, "byte ptr [rbx + 0x760], dil"),
    ("prepass_param_initialize", 0x151D580, 0x151D60E, "call", 0x151D340, None),
    ("prepass_owner_param_copy", 0x151D580, 0x6663EC0, "movups", None, "xmm0, xmmword ptr [rbx + 0x770]"),
    ("prepass_target_gate", 0x151D580, 0x6663F10, "cmp", None, "byte ptr [rsi + 0xc16d], dil"),
    ("prepass_target_param_dest", 0x151D580, 0x151D66C, "lea", None, "rcx, [rsi + 0xc0]"),
    ("prepass_target_param_copy", 0x151D580, 0x151D678, "call", 0x151CC10, None),
    ("target_param_copy_out_of_line", 0x151CC10, 0x151CC10, "jmp", 0x5A6C31E, None),
    ("prepass_full_mask_arg", 0x151D580, 0x151D680, "mov", None, "dl, 0xff"),
    ("prepass_full_mask_call", 0x151D580, 0x151D685, "call", 0x231EAF0, None),
    ("prepass_counter_mask", 0x151D580, 0x151D6CA, "or", None, "edi, edx"),
    ("prepass_prior_flag", 0x151D580, 0x151D6F1, "movzx", None, "r8d, byte ptr [rbx + 0x790]"),
    ("prepass_conditional_mask", 0x151D580, 0x151D6F9, "movzx", None, "edx, dil"),
    ("target_dispatch_gate", 0x231EAF0, 0x231EAF0, "cmp", None, "byte ptr [rcx + 0xc157], 0"),
    ("target_dispatch_offset", 0x231EAF0, 0x231EAF9, "add", None, "rcx, 0x3e80"),
    ("target_dispatch_tail_jump", 0x231EAF0, 0x231EB00, "jmp", 0x23F44F0, None),
    ("shared_mask_zero_exit", 0x23F44F0, 0x23F44F0, "test", None, "dl, dl"),
    ("shared_slot_reference", 0x23F44F0, 0x23F4543, "cmp", None, "qword ptr [rbp], 0"),
    ("shared_slot_global_gate", 0x23F44F0, 0x23F4557, "cmp", None, "byte ptr [rsi], 0"),
    ("shared_slot_reset_call", 0x23F44F0, 0x23F4566, "call", 0x23F80B0, None),
    ("shared_slot_pending_set", 0x23F44F0, 0x23F457B, "mov", None, "byte ptr [rdi], 1"),
    ("shared_slot_count", 0x23F44F0, 0x23F458E, "cmp", None, "ebx, 4"),
    ("slot_reset_field", 0x23F80B0, 0x23F80BC, "mov", None, "dword ptr [r8 + rcx + 0xee0], r9d"),
    ("slot_reset_current_index_check", 0x23F80B0, 0x23F80F0, "cmp", None, "edx, dword ptr [rcx + 0x15d0]"),
    ("slot_reset_owner_mark", 0x23F80B0, 0x23F80F8, "mov", None, "qword ptr [rcx + 0x14d0], 1"),
)


def inspect_image(image: Image) -> dict:
    groups = {"frame": FRAME_ANCHORS, "render": RENDER_ANCHORS,
              "state": STATE_ANCHORS, "prepass": PREPASS_ANCHORS,
              "dispatch": DISPATCH_ANCHORS}
    graph = {entry: image.walk(entry, 1500)
             for entry in {row[1] for rows in groups.values() for row in rows}}
    results = {}
    for group, anchors in groups.items():
        results[group] = [dict(label=label, **verify(image, entry, site, mnemonic,
                               target=target, operands=operands, graph=graph[entry]))
                          for label, entry, site, mnemonic, target, operands in anchors]
    complete_graph = all(not limitations for _, limitations in graph.values())
    all_verified = complete_graph and all(r["verified"] for rows in results.values() for r in rows)
    return {
        "schema": "ac6_native_frame_boundary_static_v1",
        "authority": "fingerprinted-static-capstone",
        "groups": results,
        "graph_limitations": {hex(k): v for k, (_, v) in graph.items() if v},
        "complete_graph": complete_graph,
        "verified_anchors": {name: sum(row["verified"] for row in rows)
                             for name, rows in results.items()},
        "all_anchors_verified": bool(all_verified),
        "runtime_observed": False,
        "safe_to_reenter_proven": False,
        "second_eye_render_proven": False,
        "limitations": [
            "Anchors confirm reachability within selected CFGs, not execution or identical paths.",
            "Close/reset calls have conditional branches and may be skipped.",
            "Owner prepass target/counter updates are conditional and do not establish eye-local state.",
            "Prepass can copy mutable target parameters and update four-slot state; neither is proven eye-local.",
            "Four-slot helper uses a process-global gate; slot identity and GPU relation are unproven.",
            "An optional owner auxiliary path also references a global owner and contains conditional dispatches.",
            "No call graph closure, alias proof, frame scheduling, GPU or callback-lifetime proof.",
            "No dynamic or indirect call census was performed.",
        ],
    }


def inspect(path: Path) -> dict:
    raw = path.read_bytes()
    digest = hashlib.sha256(raw).hexdigest()
    if digest != EXPECTED_SHA256:
        raise ValueError("Executable fingerprint mismatch; no offsets will be interpreted")
    pe = pefile.PE(data=raw, fast_load=True)
    if pe.FILE_HEADER.Machine != 0x8664 or pe.OPTIONAL_HEADER.Magic != 0x20B:
        raise ValueError("Requires x64 PE32+")
    image = Image(pe.OPTIONAL_HEADER.ImageBase,
                  [Section(s.VirtualAddress, s.get_data(), bool(s.Characteristics & 0x20000000))
                   for s in pe.sections if s.SizeOfRawData])
    return {"image": str(path.resolve()), "sha256": digest, **inspect_image(image)}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--image", type=Path, default=DEFAULT_IMAGE)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    report = inspect(args.image)
    # No silent overwrite of evidence already collected.
    with args.output.open("x", encoding="utf-8") as fh:
        json.dump(report, fh, indent=2, allow_nan=False)
        fh.write("\n")
    print(json.dumps({"output": str(args.output.resolve()),
                      "anchors": report["verified_anchors"],
                      "complete_graph": report["complete_graph"],
                      "all_anchors_verified": report["all_anchors_verified"],
                      "safe_to_reenter_proven": False}))
    return 0 if report["all_anchors_verified"] else 2


if __name__ == "__main__":
    raise SystemExit(main())

"""Verify the fingerprinted AC6 native-view route without executing the game.

Reports static instruction reachability, not runtime coverage or reentrancy.
Uses bounded control-flow decoding across every file-backed executable section;
calls are recorded but never followed. Return-based dispatch remains unresolved.
Reports belong in ignored/private evidence, never beside distributable sources.
"""
from __future__ import annotations

import argparse
from dataclasses import dataclass
import hashlib
import json
from pathlib import Path
import struct

import capstone
from capstone.x86 import X86_REG_RIP

EXPECTED_SHA256 = "e003771e5548bca91882c1d7dfe2df3de97cee05c2c989d0c0e34b036ea610d6"
DEFAULT_IMAGE = Path(r"C:\Program Files (x86)\Steam\steamapps\common\ARMORED CORE VI FIRES OF RUBICON\Game\armoredcore6.exe")


@dataclass(frozen=True)
class Section:
    rva: int
    data: bytes
    executable: bool


class Image:
    def __init__(self, base: int, sections: list[Section]):
        self.base = base
        self.sections = sorted(sections, key=lambda s: s.rva)
        for index, section in enumerate(self.sections):
            if section.rva < 0 or (index and self.sections[index - 1].rva +
                                  len(self.sections[index - 1].data) > section.rva):
                raise ValueError("Invalid or overlapping file-backed sections")
        self.decoder = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
        self.decoder.detail = True

    def section(self, rva: int) -> Section:
        for section in self.sections:
            if section.rva <= rva < section.rva + len(section.data):
                return section
        raise ValueError(f"Unbacked RVA 0x{rva:x}")

    def read(self, rva: int, size: int) -> bytes:
        if size < 0:
            raise ValueError("Negative read size")
        section = self.section(rva)
        offset = rva - section.rva
        if offset + size > len(section.data):
            raise ValueError("Read crosses file-backed section boundary")
        return section.data[offset:offset + size]

    def walk(self, entry: int, budget: int = 4096):
        if not 0 < budget <= 65536:
            raise ValueError("Instruction budget must be 1..65536")
        pending, decoded, occupied, limitations = [entry], {}, {}, set()
        while pending:
            rva = pending.pop()
            if rva in decoded:
                continue
            if len(decoded) >= budget:
                limitations.add("instruction_budget")
                break
            if rva in occupied:
                limitations.add("overlapping_instruction_streams")
                continue
            try:
                section = self.section(rva)
                if not section.executable:
                    limitations.add("non_executable_target")
                    continue
                count = min(15, section.rva + len(section.data) - rva)
                ins = next(self.decoder.disasm(self.read(rva, count), self.base + rva, count=1), None)
            except ValueError:
                limitations.add("unbacked_target")
                continue
            if ins is None:
                limitations.add("invalid_or_partial_instruction")
                continue
            if any(address in occupied for address in range(rva, rva + ins.size)):
                limitations.add("overlapping_instruction_streams")
                continue
            decoded[rva] = ins
            occupied.update((address, rva) for address in range(rva, rva + ins.size))
            if ins.group(capstone.CS_GRP_RET):
                # A ret may implement an opaque dispatch. Never assume a target.
                continue
            if ins.mnemonic in ("int3", "ud2", "hlt"):
                continue
            if ins.group(capstone.CS_GRP_JUMP) or ins.mnemonic in ("loop", "loope", "loopne"):
                if ins.operands and ins.operands[0].type == capstone.CS_OP_IMM:
                    pending.append(ins.operands[0].imm - self.base)
                else:
                    limitations.add("indirect_jump")
                if ins.mnemonic in ("jmp", "ljmp"):
                    continue
            pending.append(rva + ins.size)
        return decoded, sorted(limitations)


def target_of(image: Image, ins) -> int | None:
    if (ins.group(capstone.CS_GRP_CALL) or ins.group(capstone.CS_GRP_JUMP) or
            ins.mnemonic in ("loop", "loope", "loopne")) and ins.operands:
        if ins.operands[0].type == capstone.CS_OP_IMM:
            return ins.operands[0].imm - image.base
    for operand in ins.operands:
        if operand.type == capstone.CS_OP_MEM and operand.mem.base == X86_REG_RIP:
            return ins.address + ins.size + operand.mem.disp - image.base
    return None


def verify(image: Image, entry: int, site: int, mnemonic: str, *, target: int | None = None,
           operands: str | None = None, budget: int = 4096, graph=None) -> dict:
    decoded, limitations = graph if graph is not None else image.walk(entry, budget)
    ins = decoded.get(site)
    result = dict(entry=hex(entry), site=hex(site), verified=False,
                  limitations=limitations, reason="site_not_reached")
    if ins is None:
        return result
    actual_target = target_of(image, ins)
    result.update(mnemonic=ins.mnemonic, operands=ins.op_str,
                  target=hex(actual_target) if actual_target is not None else None)
    matches = ins.mnemonic == mnemonic and (target is None or actual_target == target)
    matches &= operands is None or ins.op_str == operands
    # Conflicting decodings anywhere in this graph make its interpretation ambiguous.
    matches &= "overlapping_instruction_streams" not in limitations
    result.update(verified=bool(matches), reason="matched" if matches else "instruction_mismatch")
    return result


def verify_vtable_slot(image: Image, table: int, slot: int, expected: int) -> dict:
    """Read a file-backed vtable entry and require its exact executable RVA."""
    result = dict(table=hex(table), slot=slot, target=None, verified=False)
    try:
        target, = struct.unpack("<Q", image.read(table + 8 * slot, 8))
        actual = target - image.base
        result["target"] = hex(actual)
        result["verified"] = actual == expected and image.section(actual).executable
    except ValueError:
        # Unbacked/invalid vtable or target is a failed verification, not proof.
        pass
    return result


# Entries/sites were established on this exact build. Labels are analyst roles,
# not debug symbols or runtime observations. Null target means exact operands.
CHECKS = [
    ("render_manager_global", 0x1292B94, 0x1292BFB, "mov", 0x4E23028, None),
    ("render_manager_draw_call", 0x1292B94, 0x1292C0A, "call", 0x15194D0, None),
    ("manager_view_list", 0x15194D0, 0x1519560, "mov", None, "rcx, qword ptr [rdi + 8]"),
    ("render_all_views_call", 0x15194D0, 0x1519564, "call", 0x2466510, None),
    ("view_list_begin", 0x2466510, 0x2466586, "mov", None, "rbx, qword ptr [r13 + 8]"),
    ("view_list_end", 0x2466510, 0x2466592, "cmp", None, "rbx, qword ptr [r13 + 0x10]"),
    ("view_priority_getter", 0x2466510, 0x24665A9, "call", 0x2466F40, None),
    ("view_target_getter_dispatch", 0x2466510, 0x246678B, "call", None, "qword ptr [rax + 0x18]"),
    ("view_render_dispatch", 0x2466510, 0x24667DC, "call", None, "qword ptr [rax + 0x38]"),
    ("priority_field", 0x2466F40, 0x2466F40, "mov", None, "eax, dword ptr [rcx + 0x10]"),
    ("add_view_duplicate_scan", 0x2466470, 0x24664A0, "cmp", None, "qword ptr [rcx], rdx"),
    ("add_view_reference", 0x2466470, 0x24664B8, "inc", None, "dword ptr [rdx + 8]"),
    ("remove_view_compaction", 0x2466850, 0x246688D, "call", 0x2465EA0, None),
    ("main_factory_table", 0x151B1E0, 0x151BC0D, "lea", 0x3762DD8, None),
    ("main_factory_wrapper", 0x151B1E0, 0x151BC22, "call", 0x22F0BD0, None),
    ("main_view_holder", 0x151B1E0, 0x151BC6A, "mov", None, "qword ptr [rdi + 0x3c8], r13"),
    ("main_view_set_camera", 0x151B1E0, 0x151BC99, "call", None, "qword ptr [rax + 0x28]"),
    ("camera_link_allocation_size", 0x151B1E0, 0x151B5D0, "mov", None, "ecx, 0x1b0"),
    ("camera_link_previous_record", 0x151B1E0, 0x151B5EF, "lea", None, "rcx, [rbx + 0xd0]"),
    ("main_view_list", 0x151B1E0, 0x151BCA3, "mov", None, "rcx, qword ptr [rdi + 0x3c0]"),
    ("main_view_add", 0x151B1E0, 0x151BCAA, "call", 0x2466470, None),
    ("factory_virtual_dispatch", 0x22F0BD0, 0x22F0C3C, "call", None, "qword ptr [rax]"),
    ("factory_target_set", 0x22F0C50, 0x22F0CE5, "call", 0x22F10F0, None),
    ("factory_view_size", 0x22F0C50, 0x22F0D51, "mov", None, "ecx, 0x140"),
    ("factory_view_constructor", 0x22F0C50, 0x22F0D75, "call", 0x22EFB70, None),
    ("target_set_size", 0x22F10F0, 0x22F1137, "mov", None, "ecx, 0xc640"),
    ("view_base_constructor", 0x22EFB70, 0x22EFBB0, "call", 0x2524260, None),
    ("view_vtable", 0x22EFB70, 0x22EFBB6, "lea", 0x3E3E7E0, None),
    ("view_target_field", 0x2524260, 0x25242E9, "mov", None, "qword ptr [rbx + 0x38], rbp"),
    ("view_scene_field", 0x2524260, 0x25242ED, "mov", None, "qword ptr [rbx + 0x40], rdi"),
    ("view_camera_field", 0x2524A90, 0x2524A9F, "mov", None, "qword ptr [rcx + 0x48], rdx"),
    ("view_camera_forwarding", 0x2524A90, 0x2524ABC, "call", None, "qword ptr [rax + 0x28]"),
    ("view_render_camera_read", 0x2524790, 0x25247D1, "mov", None, "rax, qword ptr [rcx + 0x48]"),
    ("destructor_camera_pointer", 0x25243B0, 0x2524441, "mov", None, "rbx, qword ptr [rdi + 0x48]"),
    ("destructor_camera_allocator", 0x25243B0, 0x252444D, "call", 0x2921E60, None),
    ("destructor_camera_deallocate", 0x25243B0, 0x252445C, "call", None, "qword ptr [r8 + 0x68]"),
]


# Resource ownership routes, separate from the retained 36-anchor census.
OWNERSHIP_CHECKS = [
    ("target_vtable", 0x2319220, 0x231924D, "lea", 0x3E44D90, None),
    ("target_configuration_prefix", 0x23277A0, 0x2327836, "movups", None, "xmmword ptr [rcx + 0x20], xmm6"),
    ("target_constructor", 0x22F10F0, 0x22F1154, "call", 0x2319220, None),
    ("output_manager_global", 0x22F10F0, 0x22F1161, "mov", 0x4F71B70, None),
    ("output_table", 0x22F10F0, 0x22F1168, "mov", None, "rdi, qword ptr [rax + 0x1c8]"),
    ("global_depth_reference", 0x22F10F0, 0x22F116F, "mov", None, "r15, qword ptr [rdi + 0xe8]"),
    ("global_color_reference", 0x22F10F0, 0x22F1176, "mov", None, "r12, qword ptr [rdi + 0xf0]"),
    ("color_input_reference", 0x22F10F0, 0x22F11D1, "mov", None, "qword ptr [rbp - 0x41], r12"),
    ("depth_input_reference", 0x22F10F0, 0x22F11D5, "mov", None, "qword ptr [rbp - 0x39], r15"),
    ("target_resource_manager", 0x22F10F0, 0x22F1291, "mov", 0x4F71C70, None),
    ("target_initialize_wrapper", 0x22F10F0, 0x22F129F, "call", 0x231EAD0, None),
    ("target_initialize", 0x231EAD0, 0x231EAE5, "call", 0x23277A0, None),
    ("view_pipeline_owner", 0x22EFB70, 0x22EFBC0, "mov", None, "qword ptr [rdi + 0x130], rbx"),
    ("factory_pipeline_owner_argument", 0x22F0C50, 0x22F0D6B, "mov", 0x4F71C70, None),
    ("view_pipeline_acquire", 0x22EFB70, 0x22EFBCA, "call", 0x22E4EA0, None),
    ("view_pipeline_state", 0x22EFB70, 0x22EFBCF, "mov", None, "qword ptr [rdi + 0x138], rax"),
    ("pipeline_pool_end", 0x22E4EA0, 0x22E4EB7, "mov", None, "rax, qword ptr [rcx + 0x38]"),
    ("pipeline_pool_pop", 0x22E4EA0, 0x22E4ED4, "mov", None, "qword ptr [rbx + 0x38], rcx"),
    ("pipeline_allocation_size", 0x22E4EA0, 0x22E4EEB, "mov", None, "ecx, 0x340"),
    ("pipeline_constructor", 0x22E4EA0, 0x22E4F09, "call", 0x23E24A0, None),
    ("pipeline_initialize", 0x22E4EA0, 0x22E4F2E, "call", 0x23E4A60, None),
    ("pipeline_release_state", 0x22EFEE0, 0x22EFF03, "mov", None, "rdx, qword ptr [rcx + 0x138]"),
    ("pipeline_release_owner", 0x22EFEE0, 0x22EFF0A, "mov", None, "rcx, qword ptr [rcx + 0x130]"),
    ("pipeline_release", 0x22EFEE0, 0x22EFF11, "call", 0x22EACB0, None),
    ("pipeline_return_vector", 0x22EACB0, 0x22EACBE, "add", None, "rcx, 0x28"),
    ("pipeline_return_append", 0x22EACB0, 0x22EACC7, "call", 0x22EF850, None),
    ("primary_bundle_initialize", 0x23277A0, 0x2327DDA, "call", 0x24A7F20, None),
    ("velocity_bundle", 0x23277A0, 0x2327F16, "lea", None, "rcx, [rsi + 0x798]"),
    ("velocity_initialize", 0x23277A0, 0x2327F27, "call", 0x24A7F20, None),
    ("velocity_texture_reference", 0x23277A0, 0x2327F2C, "mov", None, "rcx, qword ptr [rsi + 0x7b0]"),
    ("velocity_debug_label", 0x23277A0, 0x2327F36, "lea", 0x3E45030, None),
    ("depth_texture_initialize", 0x23277A0, 0x232822C, "call", 0x24720C0, None),
    ("depth_debug_label", 0x23277A0, 0x2328234, "lea", 0x3E45048, None),
    ("depth_texture_reference", 0x23277A0, 0x2328244, "mov", None, "qword ptr [rsi + 0xa88], rbx"),
    ("bundle_borrow_color", 0x24A7F20, 0x24A8393, "mov", None, "r14, qword ptr [rdi + 0x10]"),
    ("bundle_store_color", 0x24A7F20, 0x24A83D8, "mov", None, "qword ptr [rbx + 0x18], r14"),
    ("bundle_borrow_depth", 0x24A7F20, 0x24A85AD, "mov", None, "rsi, qword ptr [rdi + 0x18]"),
    ("bundle_store_depth", 0x24A7F20, 0x24A85FA, "mov", None, "qword ptr [rbx + 0x20], rsi"),
    ("owner_embedded_factory_dependency", 0x151B1E0, 0x151B2DC, "lea", None, "r13, [rdi + 0x6d8]"),
    ("factory_descriptor_dependency", 0x151B1E0, 0x151BC00, "mov", None, "qword ptr [rbp + 0xa8], r13"),
    ("factory_dependency_read", 0x22F0C50, 0x22F0E1F, "mov", None, "r14, qword ptr [rbp + 0xf]"),
]


# The descriptor's +0x68 pointer originates in the owning scene's embedded
# +0x6d8 subobject. The dependency takes an out-of-line constructor thunk.
# These checks prove exact static instructions/slots, not callback activation,
# a subobject refcount, or safe lifetime across multiple native views.
DEPENDENCY_CHECKS = [
    ("owner_dependency_constructor", 0x151B1E0, 0x151B2E6, "call", 0x1517DE0, None),
    ("dependency_constructor_thunk", 0x1517DE0, 0x1517DE0, "jmp", 0x654633B, None),
    ("dependency_base_vtable_initialize", 0x1517DE0, 0x654634C, "lea", 0x3AE0870, None),
    ("dependency_derived_vtable_initialize", 0x1517DE0, 0x6546356, "lea", 0x3AE0898, None),
    ("dependency_derived_vtable_store", 0x1517DE0, 0x654635D, "mov", None, "qword ptr [rcx], rax"),
    ("dependency_byte_flag_reset", 0x1517DE0, 0x6546360, "mov", None, "word ptr [rcx + 8], 0"),
    ("dependency_thunk_return", 0x1517DE0, 0x6546366, "jmp", 0x1517E0B, None),
    ("owner_view_registration_flag", 0x151B1E0, 0x151B2EC, "mov", None, "dword ptr [rdi + 0x6e8], esi"),
    ("factory_descriptor_last_16_bytes", 0x22F0C50, 0x22F0CBB, "movups", None, "xmm1, xmmword ptr [rdx + 0x60]"),
    ("factory_descriptor_last_16_stack", 0x22F0C50, 0x22F0CBF, "movaps", None, "xmmword ptr [rbp + 7], xmm1"),
    ("factory_dependent_auxiliary_1", 0x22F0C50, 0x22F0E67, "mov", None, "qword ptr [rdi + 0x18], r14"),
    ("factory_dependent_auxiliary_2", 0x22F0C50, 0x22F0F08, "mov", None, "qword ptr [rdi + 0x18], r14"),
    ("factory_dependent_auxiliary_3", 0x22F0C50, 0x22F0FAE, "mov", None, "qword ptr [rdi + 0x18], r14"),
    ("factory_dependent_auxiliary_4", 0x22F0C50, 0x22F105F, "mov", None, "qword ptr [rbx + 0x30], r14"),
    ("dependency_flag_9_gate", 0x1517F20, 0x1517F20, "cmp", None, "byte ptr [rcx + 9], 0"),
    ("dependency_flag_9_delegation", 0x1517F20, 0x1517F29, "jmp", 0x24719E0, None),
    ("dependency_flag_8_read", 0x1517F30, 0x1517F30, "movzx", None, "eax, byte ptr [rcx + 8]"),
    ("owner_view_unregister", 0x151C410, 0x151C609, "call", 0x2466850, None),
    ("owner_view_release", 0x151C410, 0x151C617, "add", None, "dword ptr [rsi + 8], -1"),
    ("owner_view_registration_flag_clear", 0x151C410, 0x151C646, "mov", None, "byte ptr [rdi + 0x6e8], bpl"),
    ("owner_embedded_dependency_destructor", 0x151C410, 0x151C9CB, "call", 0x1517E20, None),
    ("dependency_destructor_base_vtable", 0x1517E20, 0x1517E3B, "lea", 0x3AE0870, None),
    ("dependency_destructor_base_store", 0x1517E20, 0x1517E42, "mov", None, "qword ptr [rcx], rax"),
]


# The factory registers auxiliary wrappers with the view. Destruction of the
# view drains their list, but owner release invokes the view destructor only if
# its reference count reaches zero. These are conditional static paths; do not
# infer cancellation of external callbacks or the embedded dependency's life.
AUX_CLEANUP_CHECKS = [
    ("factory_first_aux_insert", 0x22F0C50, 0x22F0E1A, "call", 0x2524640, None),
    ("factory_second_aux_insert", 0x22F0C50, 0x22F0EBF, "call", 0x2524640, None),
    ("factory_third_aux_insert", 0x22F0C50, 0x22F0F60, "call", 0x2524640, None),
    ("factory_fourth_aux_insert", 0x22F0C50, 0x22F100D, "call", 0x2524640, None),
    ("factory_fifth_aux_insert", 0x22F0C50, 0x22F10B0, "call", 0x2524640, None),
    ("view_aux_acquire_ref", 0x2524640, 0x2524688, "inc", None, "dword ptr [rdx + 8]"),
    ("view_destructor_aux_cleanup", 0x25243B0, 0x25243F7, "call", 0x15180D0, None),
    ("view_destructor_aux_empty", 0x25243B0, 0x2524401, "mov", None, "qword ptr [rdi + 0x28], rax"),
    ("aux_cleanup_dec_ref", 0x15180D0, 0x15180FF, "add", None, "dword ptr [rsi + 8], -1"),
    ("aux_cleanup_virtual_destroy", 0x15180D0, 0x1518118, "call", None, "qword ptr [r8]"),
    ("aux_cleanup_clear_slot", 0x15180D0, 0x1518128, "mov", None, "qword ptr [rbx], r14"),
    ("owner_view_zero_gate", 0x151C410, 0x151C61B, "jne", 0x151C643, None),
    ("owner_view_virtual_destructor", 0x151C410, 0x151C631, "call", None, "qword ptr [r8]"),
    ("derived_view_base_destructor", 0x22EFEE0, 0x22EFF25, "call", 0x25243B0, None),
    ("aux_wrapper_nested_release", 0x22EFF80, 0x22EFF8F, "call", 0x2466E80, None),
    ("nested_first_helper_load", 0x2466E80, 0x2466EA6, "mov", None, "rsi, qword ptr [rcx + 0x18]"),
    ("nested_first_helper_destroy", 0x2466E80, 0x2466EC3, "call", None, "qword ptr [r8]"),
    ("nested_first_helper_allocator_release", 0x2466E80, 0x2466ED0, "call", None, "qword ptr [r8 + 0x68]"),
    ("nested_first_helper_clear", 0x2466E80, 0x2466ED5, "mov", None, "qword ptr [rdi + 0x18], 0"),
    ("nested_second_helper_load", 0x2466E80, 0x2466EDD, "mov", None, "rsi, qword ptr [rdi + 0x20]"),
    ("nested_second_helper_destroy", 0x2466E80, 0x2466EFA, "call", None, "qword ptr [r8]"),
    ("nested_second_helper_allocator_release", 0x2466E80, 0x2466F07, "call", None, "qword ptr [r8 + 0x68]"),
    ("nested_second_helper_clear", 0x2466E80, 0x2466F0C, "mov", None, "qword ptr [rdi + 0x20], 0"),
]


def inspect(path: Path) -> dict:
    raw = path.read_bytes()
    digest = hashlib.sha256(raw).hexdigest()
    if digest != EXPECTED_SHA256:
        raise ValueError("Executable fingerprint mismatch; no offsets will be interpreted")
    import pefile
    pe = pefile.PE(data=raw, fast_load=True)
    if pe.FILE_HEADER.Machine != 0x8664 or pe.OPTIONAL_HEADER.Magic != 0x20B:
        raise ValueError("Requires x64 PE32+")
    image = Image(pe.OPTIONAL_HEADER.ImageBase,
                  [Section(s.VirtualAddress, s.get_data(), bool(s.Characteristics & 0x20000000))
                   for s in pe.sections if s.SizeOfRawData])
    graphs = {entry: image.walk(entry) for entry in {row[1] for row in CHECKS + OWNERSHIP_CHECKS + DEPENDENCY_CHECKS + AUX_CLEANUP_CHECKS}}
    checks = []
    for label, entry, site, mnemonic, target, operands in CHECKS:
        checks.append(dict(label=label, **verify(image, entry, site, mnemonic,
                      target=target, operands=operands, graph=graphs[entry])))
    ownership = [dict(label=label, **verify(image, entry, site, mnemonic,
                 target=target, operands=operands, graph=graphs[entry]))
                 for label, entry, site, mnemonic, target, operands in OWNERSHIP_CHECKS]
    dependency = [dict(label=label, **verify(image, entry, site, mnemonic,
                  target=target, operands=operands, graph=graphs[entry]))
                  for label, entry, site, mnemonic, target, operands in DEPENDENCY_CHECKS]
    aux_cleanup = [dict(label=label, **verify(image, entry, site, mnemonic,
                   target=target, operands=operands, graph=graphs[entry]))
                   for label, entry, site, mnemonic, target, operands in AUX_CLEANUP_CHECKS]
    expected_slots = {0: 0x22F0C50, 1: 0x22F0BD0}
    view_slots = {0: 0x22EFEE0, 1: 0x2524B00, 3: 0x25249D0, 4: 0x2524AF0,
                  5: 0x2524A90, 6: 0x2524AE0, 7: 0x2524790}
    tables = []
    for table, slots in ((0x3762DD8, expected_slots), (0x3E3E7E0, view_slots)):
        for slot, expected in slots.items():
            tables.append(verify_vtable_slot(image, table, slot, expected))
    # Five slots per table: final derived table follows the base table in PE.
    dependency_tables = []
    for table, slots in ((0x3AE0870, (0x1517EB0, 0x1517F00, 0x1517F40, 0x2471930, 0x24719E0)),
                         (0x3AE0898, (0x1517E60, 0x1517EE0, 0x1517F30, 0x2471930, 0x1517F20))):
        for slot, expected in enumerate(slots):
            dependency_tables.append(verify_vtable_slot(image, table, slot, expected))
    auxiliary_tables = [verify_vtable_slot(image, table, 0, destructor)
                        for table, destructor in ((0x3E3E790, 0x22EFF80),
                                                  (0x3E3E8A8, 0x22F00F0),
                                                  (0x3E3E8E8, 0x22F0020),
                                                  (0x3E3EA68, 0x22EFFF0),
                                                  (0x3E3E928, 0x22EFEB0))]
    return dict(schema="native_view_route_static_v1", executable_sha256=digest,
                image=str(path.resolve()), authority="local-static-capstone",
                executable_sections=[hex(s.rva) for s in image.sections if s.executable],
                checks=checks, vtable_slots=tables,
                verified_checks=sum(c["verified"] for c in checks),
                ownership_checks=ownership,
                verified_ownership_checks=sum(c["verified"] for c in ownership),
                dependency_checks=dependency,
                verified_dependency_checks=sum(c["verified"] for c in dependency),
                auxiliary_cleanup_checks=aux_cleanup,
                verified_auxiliary_cleanup_checks=sum(c["verified"] for c in aux_cleanup),
                dependency_vtable_slots=dependency_tables,
                auxiliary_vtable_slots=auxiliary_tables,
                all_checks_verified=all(c["verified"] for c in checks + tables + ownership + dependency + aux_cleanup + dependency_tables + auxiliary_tables),
                runtime_observed=False, reentrant_render_proven=False,
                frame_associated=False,
                limitations=["Static decoder reachability is not observed execution.",
                             "Calls are not traversed; each explicitly seeded entry is analyzed separately.",
                             "Indirect jumps and return-based dispatch remain unresolved.",
                             "The owner destroys the view only if the view reference count reaches zero.",
                             "Auxiliary destruction and embedded dependency +0x6d8 lifetime across external callbacks are unproven.",
                             "Ownership/lifetime and GPU output identity need independent verification."])


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--image", type=Path, default=DEFAULT_IMAGE)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    report = inspect(args.image)
    with args.output.open("x", encoding="utf-8") as out:
        json.dump(report, out, indent=2, allow_nan=False)
        out.write("\n")
    print(json.dumps(dict(output=str(args.output.resolve()),
                          verified_checks=report["verified_checks"], total_checks=len(CHECKS),
                          verified_ownership_checks=report["verified_ownership_checks"],
                          verified_dependency_checks=report["verified_dependency_checks"],
                          total_dependency_checks=len(DEPENDENCY_CHECKS),
                          verified_auxiliary_cleanup_checks=report["verified_auxiliary_cleanup_checks"],
                          total_auxiliary_cleanup_checks=len(AUX_CLEANUP_CHECKS),
                          verified_dependency_slots=sum(s["verified"] for s in report["dependency_vtable_slots"]),
                          verified_auxiliary_slots=sum(s["verified"] for s in report["auxiliary_vtable_slots"]),
                          all_checks_verified=report["all_checks_verified"],
                          runtime_observed=False)))
    return 0 if report["all_checks_verified"] else 2


if __name__ == "__main__":
    raise SystemExit(main())

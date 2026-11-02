"""Read selected native resource references; never dereference GPU objects."""
import struct

from observe_native_views import sample_views, MAIN_VIEW_VTABLE_RVA

OUTPUT_MANAGER_RVA = 0x4F71B70
PIPELINE_OWNER_RVA = 0x4F71C70
TARGET_VTABLE_RVA = 0x3E44D90


def sample_resources(read, base: int) -> dict:
    result = dict(status="view_not_coherent", atomic_snapshot=False,
                  frame_associated=False, gpu_resource_identity_proven=False,
                  independent_allocations_proven=False, identity_repeat_equal=False)
    before = sample_views(read, base)
    result["view_census"] = before
    if before["status"] != "identities_match":
        return result

    def pointer(value, optional=False):
        if optional and value == 0:
            return value
        if not 0x10000 <= value <= 0x7FFFFFFFFFFF or value & 7:
            raise ValueError("Invalid native resource pointer")
        return value

    def exact(address, size):
        pointer(address)
        if not 0 < size <= 0xA90 or address + size - 1 > 0x7FFFFFFFFFFF:
            raise ValueError("Resource snapshot range exceeds bounds")
        data = read(address, size)
        if len(data) != size:
            raise ValueError("Partial resource snapshot")
        return data

    def q(data, offset=0):
        return struct.unpack_from("<Q", data, offset)[0]

    tracked = []
    def fields(address, size, offsets):
        data = exact(address, size)
        values = tuple(q(data, offset) for offset in offsets)
        tracked.append((address, size, offsets, values))
        return values

    main = pointer(int(before["main_view"], 16))
    vt, targets, scene, camera, owner, state = fields(main, 0x140, (0, 0x38, 0x40, 0x48, 0x130, 0x138))
    main_before = next(view for view in before["views"] if view["is_main"])
    if (vt != base + MAIN_VIEW_VTABLE_RVA or
            any(hex(value) != main_before[key] for key, value in
                (("targets", targets), ("scene", scene), ("camera_link", camera)))):
        result["status"] = "resource_identity_changed"
        return result
    for value in (targets, scene, camera):
        pointer(value)
    pointer(owner, True);pointer(state, True)
    tvt, extent, flags, color, depth, velocity, named_depth = fields(
        targets, 0xA90, (0, 0x20, 0x48, 0x260, 0x268, 0x7B0, 0xA88))
    if tvt != base + TARGET_VTABLE_RVA:
        result["status"] = "unexpected_target_type"
        return result
    for value in (color, depth, velocity, named_depth):
        pointer(value, True)
    output_manager, = fields(base + OUTPUT_MANAGER_RVA, 8, (0,))
    if not output_manager:
        result["status"] = "output_manager_absent"
        return result
    pointer(output_manager)
    output_table, = fields(output_manager, 0x1D0, (0x1C8,))
    if not output_table:
        result["status"] = "output_table_absent"
        return result
    pointer(output_table)
    global_depth, global_color = fields(output_table, 0x100, (0xE8, 0xF0))
    global_owner, = fields(base + PIPELINE_OWNER_RVA, 8, (0,))
    for value in (global_color, global_depth, global_owner):
        pointer(value, True)

    def alias(a, b):
        return a == b if a and b else None

    result.update(main_view=hex(main), targets=hex(targets),
        extent=[extent & 0xFFFFFFFF, extent >> 32], configuration_flags=hex(flags),
        pipeline_owner=hex(owner), pipeline_state=hex(state), global_pipeline_owner=hex(global_owner),
        references=dict(color=hex(color), depth=hex(depth), velocity=hex(velocity),
                        named_depth=hex(named_depth)),
        global_references=dict(color=hex(global_color), depth=hex(global_depth)),
        aliases_global_color=alias(color, global_color), aliases_global_depth=alias(depth, global_depth),
        named_depth_matches_primary=alias(named_depth, depth), pipeline_owner_matches_global=alias(owner, global_owner))
    repeated = True
    for address, size, offsets, values in tracked:
        data = exact(address, size)
        repeated &= tuple(q(data, offset) for offset in offsets) == values
    repeated &= sample_views(read, base) == before
    result.update(status="resource_identities_match" if repeated else "resource_identity_changed",
                  identity_repeat_equal=repeated)
    return result

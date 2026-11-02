"""Exact fake memory relationships, without a game or Windows process."""
import struct
import sys
import unittest
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
from native_view_resources import sample_resources
from test_native_view_observation import fixture as view_fixture, BASE


def fixture():
    blocks, read = view_fixture()
    def block(address, size, fields):
        data=bytearray(blocks.get(address,b""));data.extend(bytes(size-len(data)))
        for offset,value in fields.items():struct.pack_into("<Q",data,offset,value)
        blocks[address]=bytes(data)
    block(0x40000,0x140,{0x130:0xC0000,0x138:0xD0000})
    block(0x80000,0xA90,{0:BASE+0x3E44D90,0x20:2560|(1440<<32),
        0x260:0xE0000,0x268:0xF0000,0x7B0:0x100000,0xA88:0x110000})
    block(BASE+0x4F71B70,8,{0:0xA0000})
    block(0xA0000,0x1D0,{0x1C8:0xB0000})
    block(0xB0000,0x100,{0xE8:0xF0000,0xF0:0xE0000})
    # Factory instruction 0x22f0d6b supplies this global as ctor RDX.
    block(BASE+0x4F71C70,8,{0:0xC0000})
    return blocks,read


class NativeViewResourceTests(unittest.TestCase):
    def test_output_alias_is_pointer_evidence_only(self):
        _,read=fixture();r=sample_resources(read,BASE)
        self.assertEqual(r["status"],"resource_identities_match")
        self.assertEqual(r["extent"],[2560,1440])
        self.assertTrue(r["aliases_global_color"])
        self.assertTrue(r["aliases_global_depth"])
        self.assertTrue(r["pipeline_owner_matches_global"])
        self.assertFalse(r["frame_associated"])
        self.assertFalse(r["gpu_resource_identity_proven"])
        self.assertFalse(r["independent_allocations_proven"])
        self.assertFalse(r["atomic_snapshot"])

    def test_distinct_reference_does_not_prove_distinct_backing(self):
        blocks,read=fixture();data=bytearray(blocks[0x80000])
        struct.pack_into("<Q",data,0x260,0x120000);blocks[0x80000]=bytes(data)
        r=sample_resources(read,BASE)
        self.assertFalse(r["aliases_global_color"])
        self.assertFalse(r["independent_allocations_proven"])

    def test_null_references_remain_absent_and_never_alias(self):
        blocks,read=fixture()
        for address,offsets in ((0x80000,(0x260,0x268,0x7B0,0xA88)),(0xB0000,(0xE8,0xF0))):
            data=bytearray(blocks[address])
            for offset in offsets:struct.pack_into("<Q",data,offset,0)
            blocks[address]=bytes(data)
        r=sample_resources(read,BASE)
        self.assertIsNone(r["aliases_global_color"])
        self.assertIsNone(r["aliases_global_depth"])
        self.assertEqual(r["references"]["velocity"],"0x0")

    def test_bad_pointer_rejects_without_dereferencing_it(self):
        for bad in (1,0xE0001,0xFFFFFFFFFFFFFFFF,0x800000000000):
            blocks,read=fixture();data=bytearray(blocks[0x80000])
            struct.pack_into("<Q",data,0x260,bad);blocks[0x80000]=bytes(data)
            with self.assertRaises(ValueError):sample_resources(read,BASE)

    def test_truncated_resource_read_rejects(self):
        _,read=fixture()
        def partial(address,size):
            data=read(address,size)
            return data[:-1] if address==0x80000 else data
        with self.assertRaises(ValueError):sample_resources(partial,BASE)

    def test_wrong_target_or_view_type_never_certifies(self):
        for address in (0x80000,0x40000):
            blocks,read=fixture();data=bytearray(blocks[address])
            struct.pack_into("<Q",data,0,BASE+0x1000);blocks[address]=bytes(data)
            self.assertNotEqual(sample_resources(read,BASE)["status"],"resource_identities_match")

    def test_resource_change_between_reads_rejects(self):
        _,read=fixture();calls=0
        def racing(address,size):
            nonlocal calls
            data=read(address,size)
            if address==0x80000:
                calls+=1
                if calls>1:
                    data=bytearray(data);struct.pack_into("<Q",data,0x260,0x120000);return bytes(data)
            return data
        self.assertEqual(sample_resources(racing,BASE)["status"],"resource_identity_changed")

    def test_global_output_change_rejects(self):
        _,read=fixture();calls=0
        def racing(address,size):
            nonlocal calls
            data=read(address,size)
            if address==0xB0000:
                calls+=1
                if calls>1:
                    data=bytearray(data);struct.pack_into("<Q",data,0xF0,0x120000);return bytes(data)
            return data
        self.assertEqual(sample_resources(racing,BASE)["status"],"resource_identity_changed")

    def test_uninitialized_global_output_is_explicitly_unavailable(self):
        blocks,read=fixture();blocks[BASE+0x4F71B70]=bytes(8)
        r=sample_resources(read,BASE)
        self.assertEqual(r["status"],"output_manager_absent")
        self.assertFalse(r["frame_associated"])

    def test_no_reads_exceed_the_selected_resource_prefix(self):
        _,read=fixture();requests=[]
        def bounded(address,size):requests.append((address,size));return read(address,size)
        sample_resources(bounded,BASE)
        self.assertLessEqual(max(size for _,size in requests),0xA90)
        self.assertNotIn(0xE0000,[address for address,_ in requests])


if __name__=="__main__":unittest.main()

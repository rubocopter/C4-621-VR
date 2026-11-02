"""A factory argument snapshot is never a license to replay native pointers."""
import copy
import sys
import unittest
from pathlib import Path
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/"tools"))
from analyze_native_view_factory import analyze


def fixture():
    rows=[dict(phase="observing",schema="native_view_factory_trace_v1",image_base=0x140000000,pid=42,process_creation=123,closed_tick_ms=100),
          dict(phase="restoration",rva=0x22F0C50,restored=True),
          dict(phase="factory_event",seq=1,thread=55,factory=0x10000,descriptor_address=0x20000,
               auxiliary_address=0x30000,result=0x40000,valid=15,descriptor_hex="00"*0x70,
               auxiliary_hex="00"*0x18,target_params_hex="00"*0x78,view_vtable=0x143E3E7E0,
               targets=0x50000,scene=0x60000,camera=0,pipeline_owner=0x70000,pipeline_state=0x80000),
          dict(phase="disabled",status="MH_OK",disable_status="MH_OK",hooks_restored=True,
               attempts=1,completed=1,pending_at_shutdown=0,dll_retained_inactive=True,frame_associated=False)]
    census=[dict(kind="metadata",schema="native_view_resources_v1",pid=42,creation_time=123,module_base="0x140000000",
                 process_access="0x410",hooks_installed=False,game_writes=False,frame_associated=False),
            dict(kind="sample",status="resource_identities_match",identity_repeat_equal=True,main_view="0x40000",sample_begin_tick_ms=101,
                 atomic_snapshot=False,frame_associated=False,gpu_resource_identity_proven=False,
                 independent_allocations_proven=False)]
    return rows,census


class FactoryAnalysisTests(unittest.TestCase):
    def test_complete_capture_can_match_holder_but_never_proves_gpu_or_clone(self):
        rows,census=fixture();r=analyze(rows,census)
        self.assertTrue(r["factory_arguments_observed"])
        self.assertTrue(r["result_matches_main_holder"])
        for key in ("frame_associated","gpu_execution_proven","second_view_constructed","safe_to_replay"):
            self.assertFalse(r[key])

    def test_missing_duplicate_or_wrong_schema_rows_reject(self):
        for index in range(4):
            rows,census=fixture();rows.pop(index)
            self.assertFalse(analyze(rows,census)["factory_arguments_observed"])
            rows,census=fixture();rows.append(copy.deepcopy(rows[index]))
            self.assertFalse(analyze(rows,census)["factory_arguments_observed"])
        rows,census=fixture();rows[0]["schema"]="legacy"
        self.assertFalse(analyze(rows,census)["factory_arguments_observed"])

    def test_partial_counts_pending_and_restoration_disagreement_reject(self):
        for key in ("attempts","completed","pending_at_shutdown"):
            rows,census=fixture();rows[-1][key]=2
            self.assertFalse(analyze(rows,census)["factory_arguments_observed"])
        rows,census=fixture();rows[1]["restored"]=False
        self.assertFalse(analyze(rows,census)["factory_arguments_observed"])

    def test_hex_payloads_require_exact_size_and_alphabet(self):
        for key in ("descriptor_hex","auxiliary_hex","target_params_hex"):
            for bad in ("", "gg"*112, None, [0], " "+"00"*112):
                rows,census=fixture();rows[2][key]=bad
                self.assertFalse(analyze(rows,census)["factory_arguments_observed"])

    def test_read_masks_types_and_native_pointers_are_strict(self):
        for key in ("seq","thread","result","valid","targets","pipeline_state"):
            for bad in (True, "1", 1.0, -1, 0):
                rows,census=fixture();rows[2][key]=bad
                self.assertFalse(analyze(rows,census)["factory_arguments_observed"])
        rows,census=fixture();rows[2]["valid"]=7
        self.assertFalse(analyze(rows,census)["factory_arguments_observed"])

    def test_different_process_instance_or_holder_cannot_join(self):
        for key,value in (("pid",43),("creation_time",124),("module_base","0x150000000")):
            rows,census=fixture();census[0][key]=value
            self.assertTrue(analyze(rows,census)["factory_arguments_observed"])
            self.assertFalse(analyze(rows,census)["result_matches_main_holder"])
        rows,census=fixture();census[1]["main_view"]="0x90000"
        self.assertFalse(analyze(rows,census)["result_matches_main_holder"])

    def test_absent_incoherent_and_mixed_census_never_join(self):
        rows,census=fixture()
        self.assertFalse(analyze(rows,[])["result_matches_main_holder"])
        census[1]["status"]="resource_identity_changed"
        self.assertFalse(analyze(rows,census)["result_matches_main_holder"])

    def test_earlier_equal_unknown_or_malformed_postflight_tick_cannot_join(self):
        for tick in (99,100,None,True,"101",101.0):
            rows,census=fixture();census[1]["sample_begin_tick_ms"]=tick
            self.assertFalse(analyze(rows,census)["result_matches_main_holder"])
        rows,census=fixture();del rows[0]["closed_tick_ms"]
        self.assertFalse(analyze(rows,census)["factory_arguments_observed"])

    def test_restoration_rva_does_not_accept_a_float(self):
        rows,census=fixture();rows[1]["rva"]=float(0x22F0C50)
        self.assertFalse(analyze(rows,census)["factory_arguments_observed"])
        rows,census=fixture();other=dict(census[1],main_view="0x90000");census.append(other)
        self.assertFalse(analyze(rows,census)["result_matches_main_holder"])


if __name__=="__main__":unittest.main()

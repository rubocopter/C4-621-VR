"""Join native scopes only by explicit tokens, never time or pointer similarity."""
import copy
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
from analyze_native_view_trace import analyze, decode_rows


def fixture():
    rows = [dict(phase="observing", schema="native_view_trace_v1", image_base=0x140000000,
                 pid=123, expected_list=0x20000, expected_main_view=0x50000)]
    for n in range(4):
        begin = n * 4 + 1
        common = dict(phase="event", thread=1, list=0x20000, context=0x30000,
                      vtable=0, targets=0, scene=0, camera=0, valid=0, view=0)
        rows += [dict(common, kind=1, begin=begin, end=begin+3, parent=0),
                 dict(common, kind=2, begin=begin+1, end=begin+2, parent=begin,
                      view=0x50000, vtable=0x143E3E7E0, targets=0x40000,
                      scene=0x60000, camera=0x70000, valid=15)]
    rows += [dict(phase="restoration", rva=rva, restored=True) for rva in (0x2466510, 0x2524790)]
    rows += [dict(phase="disabled", status="MH_OK", disable_status="MH_OK", hooks_restored=True,
                  saved_events=8, event_attempts=8, event_overflow=0, pending_at_shutdown=0,
                  completed_frames=4, recording_cutoff=17, first_loss_sequence=0,
                  unmatched_lists=0, unscoped_views=0, dll_retained_inactive=True,
                  frame_associated=False)]
    return rows


class NativeViewTraceAnalysisTests(unittest.TestCase):
    def test_complete_native_scopes_never_promote_gpu_or_frame_association(self):
        r = analyze(fixture())
        self.assertTrue(r["native_scope_chain_observed"])
        self.assertEqual(r["main_scope_count"], 4)
        self.assertFalse(r["gpu_execution_proven"])
        self.assertFalse(r["frame_associated"])

    def test_previous_or_next_scope_is_not_selected_by_proximity(self):
        for parent in (0, 5, 9, 999):
            rows=fixture();rows[2]["parent"]=parent
            self.assertFalse(analyze(rows)["native_scope_chain_observed"])

    def test_thread_context_and_list_must_match_parent(self):
        for key in ("thread", "context", "list"):
            rows=fixture();rows[2][key]+=1
            self.assertFalse(analyze(rows)["native_scope_chain_observed"])

    def test_wrong_view_type_empty_fields_or_partial_read_reject(self):
        for key in ("view", "vtable", "targets", "scene", "camera", "valid"):
            rows=fixture();rows[2][key]=0
            self.assertFalse(analyze(rows)["native_scope_chain_observed"])

    def test_lost_events_pending_callbacks_and_disagreed_counts_reject(self):
        for key in ("event_overflow", "pending_at_shutdown", "first_loss_sequence",
                    "unmatched_lists", "unscoped_views"):
            rows=fixture();rows[-1][key]=1
            self.assertFalse(analyze(rows)["recording_complete"])
        rows=fixture();rows[-1]["saved_events"]=9
        self.assertFalse(analyze(rows)["recording_complete"])

    def test_restore_rows_must_agree_with_summary(self):
        rows=fixture();rows[-3]["restored"]=False
        self.assertFalse(analyze(rows)["recording_complete"])
        rows=fixture();rows.pop(-3)
        self.assertFalse(analyze(rows)["recording_complete"])

    def test_duplicate_ordinals_and_straddled_cutoff_reject(self):
        rows=fixture();rows[2]["begin"]=rows[1]["begin"]
        self.assertFalse(analyze(rows)["recording_complete"])
        rows=fixture();rows[-1]["recording_cutoff"]=15
        self.assertFalse(analyze(rows)["recording_complete"])

    def test_missing_duplicate_and_wrong_schema_headers_reject(self):
        rows=fixture();rows.pop(0)
        self.assertFalse(analyze(rows)["recording_complete"])
        rows=fixture();rows.append(copy.deepcopy(rows[0]))
        self.assertFalse(analyze(rows)["recording_complete"])
        rows=fixture();rows[0]["schema"]="legacy"
        self.assertFalse(analyze(rows)["recording_complete"])

    def test_boolean_string_and_float_ordinals_never_coerce(self):
        for bad in (True, "1", 1.0, -1):
            rows=fixture();rows[1]["begin"]=bad
            self.assertFalse(analyze(rows)["recording_complete"])

    def test_incomplete_empty_window_does_not_certify_invocation(self):
        rows=[r for r in fixture() if r["phase"]!="event"]
        rows[-1].update(saved_events=0,event_attempts=0,completed_frames=0,recording_cutoff=1)
        r=analyze(rows)
        self.assertTrue(r["recording_complete"])
        self.assertFalse(r["native_scope_chain_observed"])

    def test_malformed_row_shapes_fail_without_type_coercion_or_exception(self):
        for bad in (None, [], True, "event"):
            rows=fixture(); rows.insert(1,bad)
            self.assertFalse(analyze(rows)["recording_complete"])
        for bad in (None, [], {}, 1):
            rows=fixture(); rows[1]["phase"]=bad
            self.assertFalse(analyze(rows)["recording_complete"])

    def test_foreign_root_missing_main_and_crossing_intervals_do_not_join(self):
        for field, value in (("parent", 9), ("list", 0x20008), ("context", 0), ("thread", 0)):
            rows=fixture(); rows[1][field]=value
            self.assertFalse(analyze(rows)["native_scope_chain_observed"])
        rows=fixture(); rows[2]["view"]+=8
        self.assertFalse(analyze(rows)["native_scope_chain_observed"])
        rows=fixture(); rows[2]["end"]=18; rows[-1]["recording_cutoff"]=19
        self.assertFalse(analyze(rows)["native_scope_chain_observed"])

    def test_crossing_same_thread_roots_reject_even_with_valid_local_children(self):
        rows=fixture()
        for index, begin, end in ((1,1,6),(2,2,5),(3,3,8),(4,4,7)):
            rows[index].update(begin=begin,end=end)
        rows[4]["parent"]=3
        self.assertFalse(analyze(rows)["recording_complete"])

    def test_removed_scope_cannot_hide_behind_adjusted_counts(self):
        rows=fixture(); del rows[3:5]
        rows[-1].update(saved_events=6,event_attempts=6,completed_frames=3)
        self.assertFalse(analyze(rows)["recording_complete"])
        rows[-1]["recording_cutoff"]=13
        self.assertFalse(analyze(rows)["recording_complete"])

    def test_duplicate_json_keys_and_nonfinite_constants_reject(self):
        for raw in (b'{"event_overflow":1,"event_overflow":0}\n', b'{"phase":NaN}\n'):
            with self.assertRaises(ValueError): decode_rows(raw)

    def test_partial_child_never_certifies_recording(self):
        rows=fixture(); del rows[1]
        rows[-1].update(saved_events=7,completed_frames=3)
        r=analyze(rows)
        self.assertFalse(r["recording_complete"])
        self.assertFalse(r["native_scope_chain_observed"])

    def test_concurrent_fifth_scope_never_certifies_recording(self):
        rows=fixture()
        root,child=copy.deepcopy(rows[1:3])
        root.update(begin=17,end=20)
        child.update(begin=18,end=19,parent=17)
        rows[-1:-1]=[root,child]
        rows[-1].update(saved_events=10,event_attempts=10,completed_frames=5,recording_cutoff=21)
        r=analyze(rows)
        self.assertFalse(r["recording_complete"])
        self.assertFalse(r["native_scope_chain_observed"])

    def test_nested_flat_roots_cannot_hide_the_producer_parent_token(self):
        rows=fixture()
        for index, begin, end in ((1,1,8),(2,2,7),(3,3,6),(4,4,5)):
            rows[index].update(begin=begin,end=end)
        rows[4]["parent"]=3
        self.assertFalse(analyze(rows)["native_scope_chain_observed"])


if __name__ == "__main__": unittest.main()

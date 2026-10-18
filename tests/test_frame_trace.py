"""Diagnostic evidence checks; these never certify projection use by AC6."""
import copy
from pathlib import Path
import sys
import unittest
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
from analyze_frame_trace import analyze


def fixture():
    depth = dict(valid=True, queue='0xa', submit=1, list='0xb', list_index=0,
                 recording=10, tracked_transition=2)
    color = dict(valid=True, queue='0xa', submit=2, list='0xc', list_index=0,
                 recording=11, tracked_transition=1)
    lists = [dict(phase='frame_trace_list', queue='0xa', submit=i + 1,
                  list=('0xb', '0xc')[i], list_index=0, recording=10+i,
                  trusted=True, aliasing=False, depth_touched=i == 0,
                  depth_known=i == 0, depth_state=192 if i == 0 else 0,
                  depth_transition=2 if i == 0 else 0, color_touched=i, color_present=i,
                  color_transitions=[i, 0, 0]) for i in range(2)]
    sample = dict(phase='projection_sample', call_ordinal=1, view='0xabc',
                  output='0xdef', begin_qpc=100, end_qpc=101,
                  matrix=[.6, 0, 0, 0, 0, 1.1, 0, 0, 0, 0, -.000002, 1, 0, 0, 1.000002, 0],
                  width=320, height=180, jitter_enabled=False, jitter_x=.25, jitter_y=-.25,
                  shift_x=0, shift_y=0, frame_associated=False)
    return [dict(phase='frame_trace_contract', diagnostic_version=1, overflow=0,
                 pair_records=1, list_records=2, frame_associated=False), *lists,
            dict(phase='frame_trace_pair', sequence=1, slot=0, tick=1000,
                 copy_submit=2, backbuffer=0, depth_transition=depth,
                 color_transition=color, frame_associated=False), sample,
            dict(phase='projection_summary', snapshot_complete=True, overflow=0, samples=1),
            dict(phase='stream_summary', frames=1, failed=False, multiple_queues=False, ledger_unsafe=False),
            dict(phase='disabled', hooks_restored=True)]


class FrameTraceTests(unittest.TestCase):
    def test_separate_submissions_are_evidence_only_of_capture_provenance(self):
        result = analyze(fixture())
        self.assertTrue(result['capture_provenance_complete'])
        self.assertFalse(result['frame_associated'])
        self.assertEqual(result['projection_association'], 'missing_raster_binding')

    def test_reasonable_previous_next_camera_fov_or_jitter_never_becomes_frame_metadata(self):
        for mutate in (
            lambda s: s.update(call_ordinal=0, begin_qpc=99, end_qpc=100),
            lambda s: s.update(call_ordinal=2, begin_qpc=102, end_qpc=103),
            lambda s: s.update(view='0xother'),
            lambda s: s['matrix'].__setitem__(0, .8),
            lambda s: s['matrix'].__setitem__(8, .001),
        ):
            events = fixture(); mutate(events[4])
            self.assertFalse(analyze(events)['frame_associated'])

    def test_invalid_matrix_and_jitter_are_reported_without_selecting_a_replacement(self):
        for index, value in ((0, float('nan')), (5, float('inf')), (15, 1), (8, .25)):
            events = fixture(); events[4]['matrix'][index] = value
            result = analyze(events)
            self.assertGreater(result['invalid_projection_candidates'], 0)
            self.assertFalse(result['frame_associated'])

    def test_recycled_recording_sequence_slot_and_incomplete_publication_rejected(self):
        for field, value in (('recording', 9), ('submit', 2), ('tracked_transition', 1)):
            events = fixture(); events[3]['depth_transition'][field] = value
            self.assertFalse(analyze(events)['capture_provenance_complete'])
        for field, value in (('sequence', 2), ('slot', 3)):
            events = fixture(); events[3][field] = value
            self.assertFalse(analyze(events)['capture_provenance_complete'])
        for phase in ('frame_trace_pair', 'stream_summary', 'disabled', 'projection_summary'):
            events = [e for e in fixture() if e['phase'] != phase]
            self.assertFalse(analyze(events)['capture_provenance_complete'])

    def test_loss_truncation_ambiguity_and_unrestored_hooks_fail_closed(self):
        for index, change in ((0, dict(overflow=1)), (5, dict(snapshot_complete=False)),
                              (6, dict(failed=True)), (6, dict(multiple_queues=True)),
                              (6, dict(ledger_unsafe=True)), (7, dict(hooks_restored=False))):
            events = fixture();events[index].update(change)
            self.assertFalse(analyze(events)['capture_provenance_complete'])
        events = fixture(); events.insert(2, copy.deepcopy(events[1]))
        self.assertFalse(analyze(events)['capture_provenance_complete'])

    def test_historically_valid_depth_superseded_before_copy_is_rejected(self):
        for change in (dict(depth_state=16), dict(aliasing=True), dict(trusted=False),
                       dict(queue='0xother')):
            events = fixture()
            intervening = copy.deepcopy(events[1])
            intervening.update(submit=2, list_index=0, list='0xintervening', recording=12)
            intervening.update(change)
            events[2]['list_index'] = 1
            events[3]['color_transition']['list_index'] = 1
            events.insert(2, intervening)
            events[0]['list_records'] += 1
            self.assertFalse(analyze(events)['capture_provenance_complete'], change)

    def test_one_copy_submission_cannot_publish_two_pairs(self):
        events = fixture()
        duplicate = copy.deepcopy(events[3]); duplicate.update(sequence=2, slot=1)
        events.insert(4, duplicate)
        events[0]['pair_records'] = 2
        events[7]['frames'] = 2
        self.assertFalse(analyze(events)['capture_provenance_complete'])

    def test_closed_capture_window_requires_explicit_complete_coverage(self):
        events = fixture()
        events[0].update(window_closed=True, closed_submit=3, omitted_pairs=0)
        self.assertTrue(analyze(events)['capture_provenance_complete'])
        for change in (dict(omitted_pairs=1), dict(closed_submit=1)):
            altered = copy.deepcopy(events);altered[0].update(change)
            self.assertFalse(analyze(altered)['capture_provenance_complete'])
        altered = copy.deepcopy(events);del altered[0]['omitted_pairs']
        self.assertFalse(analyze(altered)['capture_provenance_complete'])


if __name__ == '__main__':
    unittest.main()

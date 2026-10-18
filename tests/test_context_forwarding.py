"""CPU forwarding snapshots cannot certify dispatch or a captured frame."""
import copy
from pathlib import Path
import sys
import unittest
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
from analyze_frame_trace import analyze
from test_frame_trace import fixture
from test_render_context_scopes import scoped_events


def events():
    result = fixture()
    scope = scoped_events()
    sample = next(r for r in result if r['phase'] == 'projection_sample')
    scope[1] = dict(sample, **scope[1])
    result = [r for r in result if r['phase'] != 'projection_sample']
    scope[0]['context'] = scope[1]['cpu_context']['context'] = '0x2080'
    scope[0]['forwarding'] = dict(layout_version=1, status='candidate', sampled_at='invocation_entry',
                                  dispatch_observed=False, parent='0x2000', payload='0x3000',
                                  child='0x4000', child_vtable='0x5000', child_slot8_target='0x6000')
    scope[2]['forwarding_enabled'] = True
    result.extend(scope)
    return result


class ContextForwardingTests(unittest.TestCase):
    def test_entry_candidate_is_separate_from_frame_and_dispatch_evidence(self):
        report = analyze(events())
        self.assertEqual(report.get('context_forwarding_status'), 'complete')
        self.assertEqual(report['context_forwarding_candidates'], 1)
        self.assertTrue(report['capture_provenance_complete'])
        self.assertFalse(report['frame_associated'])
        self.assertFalse(report['context_forwarding_dispatch_observed'])

    def test_old_logs_are_not_observed(self):
        self.assertEqual(analyze(fixture()).get('context_forwarding_status'), 'not_observed')

    def test_empty_publication_is_no_coverage_even_when_capture_is_complete(self):
        for opened in (True, False):
            rows = [r for r in events() if r['phase'] not in ('render_context_invocation', 'projection_sample')]
            summary = next(r for r in rows if r['phase'] == 'render_context_summary')
            summary.update(samples=0, window_started=opened)
            next(r for r in rows if r['phase'] == 'projection_summary')['samples'] = 0
            result = analyze(rows)
            self.assertTrue(result['capture_provenance_complete'])
            self.assertEqual(result.get('context_forwarding_status'), 'no_coverage')
            self.assertIs(result.get('context_forwarding_window_started'), opened)
            self.assertEqual(result['context_forwarding_candidates'], 0)

    def test_window_telemetry_is_strict_and_cannot_contradict_a_published_invocation(self):
        for value in (False, 'true', 1, None):
            rows = events();next(r for r in rows if r['phase'] == 'render_context_summary')['window_started'] = value
            self.assertEqual(analyze(rows).get('context_forwarding_status'), 'rejected')

    def test_unknown_null_or_unreadable_layouts_remain_rejections_without_partial_payload(self):
        for reason in ('adapter_mismatch', 'null_payload', 'null_child', 'invalid_child_vtable', 'invalid_target', 'unreadable'):
            rows = events(); inv = next(r for r in rows if r['phase'] == 'render_context_invocation')
            inv['forwarding'].update(status=reason, **{key: '0x0' for key in ('parent', 'payload', 'child', 'child_vtable', 'child_slot8_target')})
            result = analyze(rows)
            self.assertEqual(result.get('context_forwarding_status'), 'complete')
            self.assertEqual(result['context_forwarding_candidates'], 0)
            self.assertEqual(result['context_forwarding_rejected'], 1)
            inv['forwarding']['child'] = '0x4000'
            self.assertEqual(analyze(rows).get('context_forwarding_status'), 'rejected')

    def test_missing_or_partial_publication_never_certifies_forwarding(self):
        rows = events(); del next(r for r in rows if r['phase'] == 'render_context_invocation')['forwarding']
        self.assertEqual(analyze(rows).get('context_forwarding_status'), 'rejected')
        for field, value in (('snapshot_complete', False), ('incomplete_slots', 1), ('in_flight', 1), ('overflow', 1)):
            rows = events(); next(r for r in rows if r['phase'] == 'render_context_summary')[field] = value
            self.assertEqual(analyze(rows).get('context_forwarding_status'), 'rejected')

    def test_late_dispatch_claims_unknown_versions_and_malformed_fields_rejected(self):
        for field, value in (('sampled_at', 'after_return'), ('dispatch_observed', True), ('layout_version', 2),
                             ('layout_version', True), ('status', 'guess'), ('child', '0x0'), ('child_vtable', '0x5001'),
                             ('child', True), ('child_slot8_target', 2**64), ('child_slot8_target', '0x10000000000000000')):
            rows = events(); next(r for r in rows if r['phase'] == 'render_context_invocation')['forwarding'][field] = value
            self.assertEqual(analyze(rows).get('context_forwarding_status'), 'rejected', (field, value))

    def test_previous_next_wrong_thread_or_recycled_parent_never_selects_helper_by_time(self):
        for change in (dict(ordinal=6), dict(ordinal=8), dict(thread=4), dict(context='0x3080')):
            rows = events(); next(r for r in rows if r['phase'] == 'render_context_invocation').update(change)
            self.assertEqual(analyze(rows).get('context_forwarding_status'), 'rejected')
        rows = events(); inv = next(r for r in rows if r['phase'] == 'render_context_invocation')
        rows.append(copy.deepcopy(inv));next(r for r in rows if r['phase'] == 'render_context_summary')['samples'] = 2
        self.assertEqual(analyze(rows).get('context_forwarding_status'), 'rejected')


if __name__ == '__main__':
    unittest.main()

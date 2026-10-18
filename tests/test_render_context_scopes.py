"""A CPU invocation token is an exact scope, never a captured-frame identity."""
import copy
from pathlib import Path
import sys
import unittest
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
from analyze_frame_trace import analyze_context_scopes


def scoped_events():
    identity = dict(scene='0x100', context='0x200', packet='0x300', vtable='0x400',
                    slot1_target='0x500', slot5_target='0x600')
    invocation = dict(phase='render_context_invocation', ordinal=7, thread=3,
                      begin_qpc=100, end_qpc=200, **identity)
    sample = dict(phase='projection_sample', call_ordinal=999, thread=3,
                  begin_qpc=130, end_qpc=140,
                  cpu_context=dict(invocation_ordinal=7, thread=3, **identity))
    summary = dict(phase='render_context_summary', samples=1, overflow=0,
                   incomplete_slots=0, in_flight=0, snapshot_complete=True)
    return [invocation, sample, summary]


class ContextScopeTests(unittest.TestCase):
    def test_exact_scope_is_independent_of_helper_ordinal_and_does_not_prove_gpu_use(self):
        result = analyze_context_scopes(scoped_events())
        self.assertTrue(result['cpu_context_scope_complete'])
        self.assertEqual(result['scoped_projection_candidates'], 1)
        self.assertFalse(result['cpu_scope_gpu_binding_observed'])

    def test_outside_scope_and_old_log_are_explicitly_unobserved(self):
        self.assertEqual(analyze_context_scopes([])['cpu_context_scope_status'], 'not_observed')
        events = scoped_events();events[1]['cpu_context']['invocation_ordinal'] = 0
        result = analyze_context_scopes(events)
        self.assertTrue(result['cpu_context_scope_complete'])
        self.assertEqual(result['scoped_projection_candidates'], 0)
        self.assertEqual(result['projection_candidates_outside_context_scope'], 1)

    def test_previous_next_missing_or_duplicate_invocation_rejected(self):
        for ordinal in (6, 8):
            events = scoped_events();events[1]['cpu_context']['invocation_ordinal'] = ordinal
            self.assertFalse(analyze_context_scopes(events)['cpu_context_scope_complete'])
        events = scoped_events();del events[0]
        self.assertFalse(analyze_context_scopes(events)['cpu_context_scope_complete'])
        events = scoped_events();events.append(copy.deepcopy(events[0]));events[2]['samples'] = 2
        self.assertFalse(analyze_context_scopes(events)['cpu_context_scope_complete'])

    def test_recycled_context_with_wrong_vtable_scene_packet_or_target_rejected(self):
        for field in ('scene', 'context', 'packet', 'vtable', 'slot1_target', 'slot5_target'):
            events = scoped_events();events[1]['cpu_context'][field] = '0x999'
            self.assertFalse(analyze_context_scopes(events)['cpu_context_scope_complete'], field)

    def test_thread_and_interval_are_consistency_checks_not_nearest_sample_selection(self):
        for change in (dict(thread=4), dict(begin_qpc=99), dict(end_qpc=201), dict(begin_qpc=141)):
            events = scoped_events();events[1].update(change)
            self.assertFalse(analyze_context_scopes(events)['cpu_context_scope_complete'])

    def test_partial_overflow_or_missing_summary_never_certifies_scope(self):
        for change in (dict(snapshot_complete=False), dict(overflow=1), dict(incomplete_slots=1),
                       dict(in_flight=1), dict(samples=2)):
            events = scoped_events();events[2].update(change)
            self.assertFalse(analyze_context_scopes(events)['cpu_context_scope_complete'])
        events = scoped_events();del events[2]
        self.assertFalse(analyze_context_scopes(events)['cpu_context_scope_complete'])

    def test_malformed_qpc_and_thread_types_fail_instead_of_lexical_or_coercing_comparison(self):
        events = scoped_events()
        events[0].update(begin_qpc='100', end_qpc='300')
        events[1].update(begin_qpc='20', end_qpc='21')
        self.assertFalse(analyze_context_scopes(events)['cpu_context_scope_complete'])
        for value in (True, 3.0, '3', 0, -1, 2**32):
            for index in (0, 1):
                events = scoped_events();events[index]['thread'] = value
                self.assertFalse(analyze_context_scopes(events)['cpu_context_scope_complete'], (index, value))
            events = scoped_events();events[1]['cpu_context']['thread'] = value
            self.assertFalse(analyze_context_scopes(events)['cpu_context_scope_complete'], value)
        for field in ('begin_qpc', 'end_qpc'):
            for value in (True, 130.0, '130', -1, 2**63):
                for index in (0, 1):
                    events = scoped_events();events[index][field] = value
                    self.assertFalse(analyze_context_scopes(events)['cpu_context_scope_complete'], (index, field, value))


if __name__ == '__main__':
    unittest.main()

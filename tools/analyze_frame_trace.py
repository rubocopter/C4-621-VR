"""Check bounded producer diagnostics. Never exports a frame-matched projection.

QPC timestamps, helper call ordinals and finite matrices cannot establish GPU
use. This report deliberately has no nearest-sample selection or calibration.
"""
import argparse
import hashlib
import json
import math
from pathlib import Path

from projection_math import inspect_projection


def projection_candidate_valid(sample):
    """Only the observed, zero-shift helper convention; never a draw witness."""
    try:
        inspect_projection(sample['matrix'])
        width, height = sample['width'], sample['height']
        if not (128 <= width <= 16384 and 128 <= height <= 16384):
            return False
        # Nonzero shifts have not been reverse engineered. Retain them in the
        # raw log, but do not infer how jitter and lens shifts compose.
        if sample['shift_x'] != 0 or sample['shift_y'] != 0:
            return False
        enabled = sample['jitter_enabled']
        expected = (2 * sample['jitter_x'] / width, -2 * sample['jitter_y'] / height) if enabled else (0, 0)
        return all(math.isfinite(v) for v in expected) and all(
            math.isclose(sample['matrix'][i], value, rel_tol=1e-5, abs_tol=1e-9)
            for i, value in zip((8, 9), expected))
    except (KeyError, TypeError, ValueError, OverflowError):
        return False


def analyze_context_scopes(events):
    """Corroborate explicit TLS invocation identities, never temporal guesses."""
    invocations = [e for e in events if e.get('phase') == 'render_context_invocation']
    summaries = [e for e in events if e.get('phase') == 'render_context_summary']
    samples = [e for e in events if e.get('phase') == 'projection_sample']
    observed = bool(invocations or summaries or any('cpu_context' in s for s in samples))
    errors, scoped, outside = [], 0, 0
    if observed:
        if len(summaries) != 1:
            errors.append('missing_or_duplicate_context_summary')
        summary = summaries[0] if len(summaries) == 1 else {}
        if (summary.get('snapshot_complete') is not True or summary.get('overflow') != 0
                or summary.get('incomplete_slots') != 0 or summary.get('in_flight') != 0
                or summary.get('samples') != len(invocations)):
            errors.append('incomplete_context_observation')
        by_ordinal = {}
        for invocation in invocations:
            ordinal = invocation.get('ordinal')
            if type(ordinal) is not int or ordinal <= 0 or ordinal in by_ordinal:
                errors.append('ambiguous_context_invocation')
            else:
                by_ordinal[ordinal] = invocation
        identity_fields = ('scene', 'context', 'packet', 'vtable', 'slot1_target', 'slot5_target')
        for sample in samples:
            try:
                scope = sample['cpu_context']
                ordinal = scope['invocation_ordinal']
                if type(ordinal) is not int or ordinal < 0:
                    raise ValueError('bad invocation ordinal')
                if ordinal == 0:
                    outside += 1
                    continue
                invocation = by_ordinal[ordinal]
                for record in (invocation, sample):
                    for field in ('begin_qpc', 'end_qpc'):
                        value = record[field]
                        if type(value) is not int or not 0 < value < 2**63:
                            raise ValueError('invalid QPC integer')
                for record in (invocation, sample, scope):
                    value = record['thread']
                    if type(value) is not int or not 0 < value < 2**32:
                        raise ValueError('invalid thread integer')
                for field in identity_fields:
                    value = scope[field]
                    if not isinstance(value, str) or not value.startswith('0x') or int(value, 16) <= 0:
                        raise ValueError('invalid context identity')
                    if value != invocation[field]:
                        raise ValueError('different context invocation')
                if sample['thread'] != invocation['thread'] or scope['thread'] != sample['thread']:
                    raise ValueError('different recording thread')
                if not invocation['begin_qpc'] <= sample['begin_qpc'] <= sample['end_qpc'] <= invocation['end_qpc']:
                    raise ValueError('helper outside invocation lifetime')
                scoped += 1
            except (KeyError, TypeError, ValueError):
                errors.append('unresolved_helper_context_scope')
    return dict(cpu_context_scope_status=('complete' if not errors else 'rejected') if observed else 'not_observed',
                cpu_context_scope_complete=observed and not errors,
                cpu_context_scope_errors=sorted(set(errors)),
                scoped_projection_candidates=scoped if not errors else 0,
                projection_candidates_outside_context_scope=outside,
                cpu_scope_gpu_binding_observed=False)


def analyze_context_forwarding(events):
    """Check entry snapshots published with their CPU invocation, not GPU use."""
    invocations = [e for e in events if e.get('phase') == 'render_context_invocation']
    summaries = [e for e in events if e.get('phase') == 'render_context_summary']
    observed = any('forwarding' in e for e in invocations) or any(e.get('forwarding_enabled') for e in summaries)
    errors, candidates, rejected = [], 0, 0
    if observed:
        if len(summaries) != 1 or summaries[0].get('forwarding_enabled') is not True:
            errors.append('missing_forwarding_contract')
        if not analyze_context_scopes(events)['cpu_context_scope_complete']:
            errors.append('incomplete_forwarding_invocation_scope')
        rejection_statuses = {'adapter_mismatch', 'null_payload', 'null_child', 'invalid_child_vtable', 'invalid_target', 'unreadable'}
        fields = ('parent', 'payload', 'child', 'child_vtable', 'child_slot8_target')
        def pointer(record, field):
            value = record[field]
            if not isinstance(value, str) or not value.startswith('0x') or not 0 <= int(value, 16) < 2**64:
                raise ValueError('invalid pointer')
            return int(value, 16)
        for invocation in invocations:
            try:
                snapshot = invocation['forwarding']
                if (type(snapshot['layout_version']) is not int or snapshot['layout_version'] != 1
                        or snapshot['sampled_at'] != 'invocation_entry' or snapshot['dispatch_observed'] is not False):
                    raise ValueError('unsupported forwarding observation')
                for field, limit in (('ordinal', 2**64), ('thread', 2**32), ('begin_qpc', 2**63), ('end_qpc', 2**63)):
                    if type(invocation[field]) is not int or not 0 < invocation[field] < limit:
                        raise ValueError('invalid invocation identity')
                if invocation['end_qpc'] < invocation['begin_qpc']:
                    raise ValueError('invalid invocation interval')
                values = {field: pointer(snapshot, field) for field in fields}
                if snapshot['status'] in rejection_statuses:
                    if any(values.values()):
                        raise ValueError('partial rejected payload')
                    rejected += 1
                elif snapshot['status'] == 'candidate':
                    context = pointer(invocation, 'context')
                    if (not all(values.values()) or context < 0x80 or context % 8
                            or values['parent'] != context - 0x80
                            or values['child'] % 8 or values['child_vtable'] % 8):
                        raise ValueError('incompatible adapter layout')
                    candidates += 1
                else:
                    raise ValueError('unknown forwarding status')
            except (KeyError, TypeError, ValueError):
                errors.append('invalid_forwarding_snapshot')
    return dict(context_forwarding_status=('complete' if not errors else 'rejected') if observed else 'not_observed',
                context_forwarding_errors=sorted(set(errors)),
                context_forwarding_candidates=candidates if not errors else 0,
                context_forwarding_rejected=rejected,
                context_forwarding_dispatch_observed=False)


def analyze(events):
    errors = []
    def rows(phase):
        return [e for e in events if e.get('phase') == phase]
    def one(phase):
        found = rows(phase)
        if len(found) != 1:
            errors.append('missing_or_duplicate_' + phase)
            return {}
        return found[0]
    contract, summary, disabled, projection = (one(p) for p in (
        'frame_trace_contract', 'stream_summary', 'disabled', 'projection_summary'))
    lists, pairs, samples = (rows(p) for p in ('frame_trace_list', 'frame_trace_pair', 'projection_sample'))
    if contract.get('diagnostic_version') != 1 or contract.get('overflow') != 0:
        errors.append('unknown_or_truncated_trace')
    if contract.get('pair_records') != len(pairs) or contract.get('list_records') != len(lists):
        errors.append('incomplete_records')
    # Additive diagnostic fields. Older v1 logs have no closed-window marker;
    # their existing overflow rejection remains unchanged.
    if contract.get('window_closed'):
        closed = contract.get('closed_submit')
        if (type(closed) is not int or closed <= 0 or contract.get('omitted_pairs') != 0
                or any(type(p.get('copy_submit')) is not int or p['copy_submit'] >= closed for p in pairs)
                or any(type(e.get('submit')) is not int or e['submit'] > closed for e in lists)):
            errors.append('incomplete_closed_capture_window')
    if (not projection.get('snapshot_complete') or projection.get('samples') != len(samples)):
        errors.append('incomplete_projection_observation')
    if any(summary.get(f) is not False for f in ('failed', 'multiple_queues', 'ledger_unsafe')):
        errors.append('producer_failure')
    if disabled.get('hooks_restored') is not True:
        errors.append('hooks_not_restored')
    if not pairs or [p.get('sequence') for p in pairs] != list(range(1, len(pairs) + 1)) or summary.get('frames') != len(pairs):
        errors.append('incomplete_sequence_history')
    def key(e):
        return tuple(e.get(k) for k in ('queue', 'submit', 'list', 'list_index', 'recording'))
    indexed = {}
    for entry in lists:
        k = key(entry)
        if k in indexed:
            errors.append('ambiguous_list_identity')
        indexed[k] = entry
    # Resolve sources at the END of each copy submission. A historical entry
    # is insufficient if an intervening execution superseded/invalidated it.
    def source_key(e, transition):
        return key(e) + (transition,)
    current_depth, current_queue = None, None
    active_color = {}
    at_copy = {}
    copy_submissions = [p.get('copy_submit') for p in pairs]
    if (any(type(n) is not int or n <= 0 for n in copy_submissions)
            or any(a >= b for a, b in zip(copy_submissions, copy_submissions[1:]))):
        errors.append('duplicate_or_unordered_copy_submission')
    pairs_by_submit = {p.get('copy_submit'): p for p in pairs}
    grouped = {}
    try:
        for entry in lists:
            grouped.setdefault(entry['submit'], []).append(entry)
        for submit in sorted(set(grouped) | set(pairs_by_submit)):
            active_color = {}
            for entry in sorted(grouped.get(submit, []), key=lambda e: e['list_index']):
                if current_queue != entry['queue']:
                    current_depth = None
                current_queue = entry['queue']
                if not entry['trusted'] or entry['aliasing']:
                    current_depth, active_color = None, {}
                    continue
                if entry['depth_touched']:
                    current_depth = (source_key(entry, entry['depth_transition'])
                                     if entry['depth_known'] and entry['depth_state'] == 192 else None)
                for backbuffer in range(3):
                    if entry['color_touched'] & (1 << backbuffer):
                        active_color[backbuffer] = (source_key(entry, entry['color_transitions'][backbuffer])
                                                   if entry['color_present'] & (1 << backbuffer) else None)
            if submit in pairs_by_submit:
                at_copy[submit] = (current_depth, active_color.copy())
                current_depth = None  # a successful capture retires its depth witness
    except (KeyError, TypeError, ValueError):
        errors.append('incomplete_execution_history')
    for p in pairs:
        try:
            if p['slot'] not in (0, 1, 2) or p['backbuffer'] not in (0, 1, 2) or p['tick'] <= 0:
                raise ValueError('invalid_slot_or_tick')
            for kind in ('depth', 'color'):
                source = p[kind + '_transition']
                entry = indexed.get(key(source), {})
                if (not source['valid'] or not source['recording'] or not entry.get('trusted')
                        or entry.get('aliasing') or source['submit'] > p['copy_submit']):
                    raise ValueError('missing_transition_identity')
                expected_depth, expected_color = at_copy.get(p['copy_submit'], (None, {}))
                expected = expected_depth if kind == 'depth' else expected_color.get(p['backbuffer'])
                if source_key(source, source['tracked_transition']) != expected:
                    raise ValueError('superseded_transition')
                if kind == 'depth':
                    if (not entry['depth_touched'] or not entry['depth_known'] or entry['depth_state'] != 192
                            or source['tracked_transition'] != entry['depth_transition']):
                        raise ValueError('wrong_depth_recording')
                elif (source['submit'] != p['copy_submit'] or not entry['color_present'] & (1 << p['backbuffer'])
                      or source['tracked_transition'] != entry['color_transitions'][p['backbuffer']]):
                    raise ValueError('wrong_color_recording')
        except (KeyError, IndexError, TypeError, ValueError):
            errors.append('unresolved_pair_' + str(p.get('sequence')))
    invalid = sum(not projection_candidate_valid(s) for s in samples)
    return dict(capture_provenance_complete=not errors, errors=sorted(set(errors)),
                pairs=len(pairs), projection_candidates=len(samples),
                invalid_projection_candidates=invalid,
                projection_admitted_overflow=projection.get('overflow', 0),
                projection_window_full=projection.get('sample_limit_reached', False),
                capture_window_closed=contract.get('window_closed', False),
                capture_pairs_omitted=contract.get('omitted_pairs', 0),
                frame_associated=False, projection_association='missing_raster_binding',
                missing=['GPU constant-buffer/root binding and bytes at the depth-writing draw',
                         'actual raster viewport/scissor and jitter convention',
                         'view transform and depth-to-postprocessed-color/TAA history mapping'],
                **analyze_context_scopes(events), **analyze_context_forwarding(events))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--log', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    raw = args.log.read_bytes()
    events = [json.loads(line) for line in raw.decode('utf-8').splitlines() if line.strip()]
    result = analyze(events)
    result['log_sha256'] = hashlib.sha256(raw).hexdigest()
    with args.output.open('x', encoding='utf-8') as output:
        json.dump(result, output, indent=2, allow_nan=False)
        output.write('\n')
    print(json.dumps(result, indent=2, allow_nan=False))
    return 0 if result['capture_provenance_complete'] else 2


if __name__ == '__main__':
    raise SystemExit(main())

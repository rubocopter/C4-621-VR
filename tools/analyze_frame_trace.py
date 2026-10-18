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
                         'view transform and depth-to-postprocessed-color/TAA history mapping'])


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

"""Join native observations, keeping CPU recording evidence separate from GPU proof."""
import argparse
from collections import Counter, defaultdict
import hashlib
import json
from pathlib import Path


def analyze(records):
    header = next((r for r in records if r['phase'] == 'observing'), {})
    stopped = next((r for r in records if r['phase'] == 'disabled'), {})
    base = header.get('image_base', 0)
    totals = next((r for r in records if r['phase'] == 'descriptor_totals'), {})
    cutoff = totals.get('recording_cutoff', 2**64-1)
    all_uploads = [r for r in records if r['phase'] == 'upload']
    uploads = [r for r in all_uploads if r['end'] < cutoff]
    all_events = [r for r in records if r['phase'] == 'event']
    events = [r for r in all_events if r['end'] < cutoff]
    by_kind = {k: [r for r in events if r['kind'] == k] for k in range(1, 6)}
    descriptors = {r['begin']: r for r in by_kind[4]}
    draws = {r['begin']: r for r in by_kind[2]}
    uploads_by_range, resolves_by_descriptor, tables_by_draw = defaultdict(list), defaultdict(list), defaultdict(list)
    for u in uploads:
        uploads_by_range[u['destination'],u['buffer']].append(u)
    for r in by_kind[1]:
        resolves_by_descriptor[r['a'][7],r['a'][1],r['thread']].append(r)
    for t in by_kind[3]:
        tables_by_draw[t['scope']].append(t)
    clean = stopped.get('hooks_restored', False) and not any(stopped.get(k, 1) for k in ('upload_overflow', 'event_overflow', 'active_callbacks_at_flush'))
    clean &= stopped.get('status') == 'MH_OK' and stopped.get('disable_status') == 'MH_OK'
    clean &= all(not r.get('pending_at_shutdown', 0) and not r.get('first_loss_sequence', 0) for r in records if r['phase'] == 'descriptor_totals')
    clean &= all(sum(r['phase'] == phase for r in records) == 1 for phase in ('observing', 'disabled', 'descriptor_totals'))
    clean &= all(key in totals for key in ('pending_at_shutdown', 'first_loss_sequence', 'recording_cutoff')) and 0 < cutoff < 2**64
    for attempts, saved, actual in (('upload_attempts', 'saved_uploads', len(all_uploads)), ('event_attempts', 'saved_events', len(all_events))):
        clean &= attempts in stopped and saved in stopped and stopped[attempts] == stopped[saved] == actual
    starts = [r['begin'] for r in all_uploads+all_events]
    clean &= len(set(starts)) == len(starts)
    candidates, rejected = [], Counter()
    for item in by_kind[5]:
        descriptor = descriptors.get(item['a'][0])
        if not descriptor or item['valid'] & 7 != 7 or descriptor['valid'] != 63:
            rejected['incomplete_descriptor_snapshot'] += 1
            continue
        d, i = descriptor['a'], item['a']
        if not i[2] or not i[3]:
            rejected['copy_or_null_descriptor_route'] += 1
            continue
        resolves = [r for r in resolves_by_descriptor[descriptor['begin'],i[3],descriptor['thread']]
                    if descriptor['begin'] < r['begin'] < r['end'] < descriptor['end']]
        if len(resolves) != 1:
            rejected['missing_or_ambiguous_resolver'] += 1
            continue
        resolve = resolves[0]
        r = resolve['a']
        if resolve['valid'] != 15 or not r[2] or not r[3] or r[2:4] != r[5:7] or r[0] != d[9] or r[1] < r[3] or r[4] < r[1]-r[3]:
            rejected['unstable_resource_or_invalid_offset'] += 1
            continue
        preceding = [u for u in uploads_by_range[i[3],i[2]] if u['end'] < descriptor['begin']]
        if not preceding:
            rejected['upload_not_recorded'] += 1
            continue
        upload = max(preceding, key=lambda u: u['end'])
        if upload['valid'] != 7 or upload['before'] != upload['after'] or len(upload['before']) != 2*upload['size'] or not upload['size'] or upload['size'] > i[4] or upload['capacity'] != i[4]:
            rejected['partial_copy_or_capacity_mismatch'] += 1
            continue
        draw = draws.get(descriptor['scope'])
        if not draw or draw['valid'] & 192 != 192 or not draw['a'][4] or draw['thread'] != descriptor['thread'] or draw['a'][0] != upload['context'] or not draw['begin'] < descriptor['begin'] < descriptor['end'] < draw['end']:
            rejected['outside_recorded_draw'] += 1
            continue
        # Include writes crossing the cutoff when checking overlap.
        lo, hi = upload['destination'], upload['destination'] + upload['size']
        if any(u is not upload and upload['begin'] < u['end'] and u['begin'] < draw['end'] and u['destination'] < hi and lo < u['destination']+u['size'] for u in all_uploads):
            rejected['overlapping_upload_write'] += 1
            continue
        tables = [t for t in tables_by_draw[draw['begin']] if t['thread'] == draw['thread'] and t['valid'] == 31 and t['a'][4] == 1
                  and t['a'][3] == t['a'][7] and t['a'][6] == t['a'][8] and t['a'][6]
                  and t['a'][0] == draw['a'][3] and t['a'][3] == draw['a'][4] and t['a'][2] == d[6]
                  and descriptor['end'] < t['begin'] < t['end'] < draw['end']]
        if len(tables) != 1:
            rejected['missing_or_ambiguous_graphics_table'] += 1
            continue
        slot = d[1] + i[1]
        if not d[7] or d[5] == 2**64-1 or slot >= d[11] or max(d[5], d[6]) + slot*d[7] >= 2**64:
            rejected['invalid_descriptor_handle_arithmetic'] += 1
            continue
        candidates.append(dict(upload_begin=upload['begin'], descriptor_begin=descriptor['begin'], resolver_begin=resolve['begin'],
            draw_begin=draw['begin'], table_begin=tables[0]['begin'], source_size=upload['size'], source_caller_rva=hex(upload['caller']-base),
            upload_cpu=i[3], resource_candidate=r[2], gpu_va=r[4], descriptor_cpu=d[5]+slot*d[7], descriptor_gpu=d[6]+slot*d[7],
            root_parameter=tables[0]['a'][1], root_signature=tables[0]['a'][6], descriptor_offset=slot, command_list=draw['a'][4], source_sha256=hashlib.sha256(bytes.fromhex(upload['before'])).hexdigest()))
    return dict(schema='native_constants_analysis_v1', clean_recording_window=bool(clean),
        uploads=len(uploads), upload_sizes=dict(Counter(u['size'] for u in uploads)), event_counts={str(k):len(v) for k,v in by_kind.items()},
        equal_complete_copies=sum(u['valid'] & 3 == 3 and u['before'] == u['after'] for u in uploads),
        candidate_count=len(candidates), candidates=candidates, rejected=dict(rejected),
        native_recording_chain_observed=bool(clean and candidates), gpu_execution_proven=False, frame_associated=False,
        missing=['actual API descriptor arguments and heap versions', 'command-list recording generation/reset', 'queue/submission/fence and cross-thread order',
                 'unobserved direct or partial CPU writes/aliasing', 'captured color/depth identity', 'indexed draw and full-scene coverage'])


if __name__ == '__main__':
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('log', type=Path)
    p.add_argument('--output', type=Path, required=True)
    args = p.parse_args()
    report = analyze([json.loads(line) for line in args.log.read_text().splitlines()])
    report['log_sha256'] = hashlib.sha256(args.log.read_bytes()).hexdigest()
    with args.output.open('x') as out:
        json.dump(report, out, indent=2)
    print(json.dumps({k:v for k,v in report.items() if k != 'candidates'}, indent=2))

import copy
import unittest
from analyze import analyze


def fixture():
    def ev(kind, begin, end, a, scope=10, valid=0):
        return dict(phase='event',kind=kind,begin=begin,end=end,a=a+[0]*(16-len(a)),scope=scope,thread=1,valid=valid)
    return [dict(phase='observing',image_base=0), dict(phase='disabled',hooks_restored=True,status='MH_OK',disable_status='MH_OK',upload_overflow=0,event_overflow=0,active_callbacks_at_flush=0,upload_attempts=1,saved_uploads=1,event_attempts=5,saved_events=5),
        dict(phase='upload',begin=1,end=2,thread=1,destination=1000,buffer=100,context=500,size=1,capacity=256,valid=7,before='aa',after='aa',caller=273),
        ev(2,10,90,[500,3,0,600,700],valid=192),ev(4,20,50,[800,0,1,0,0,2000,3000,32,900,950,0,1],valid=63),
        ev(5,21,22,[20,0,100,1000,256],valid=7),ev(1,30,40,[950,1000,111,100,9999,111,100,20],valid=15),
        ev(3,60,70,[600,2,3000,700,1,0,800,700,800],valid=31),
        dict(phase='descriptor_totals',pending_at_shutdown=0,first_loss_sequence=0,recording_cutoff=100)]


class JoinTests(unittest.TestCase):
    def test_valid_cpu_chain_never_promotes_gpu_execution(self):
        r=analyze(fixture()); self.assertEqual(r['candidate_count'],1); self.assertTrue(r['native_recording_chain_observed']); self.assertFalse(r['gpu_execution_proven']); self.assertFalse(r['frame_associated'])
    def test_overflow_is_not_a_clean_chain(self):
        rows=fixture(); rows[1]['event_overflow']=1; self.assertFalse(analyze(rows)['native_recording_chain_observed'])
    def test_overlapping_reuse_rejected(self):
        rows=fixture(); write=copy.deepcopy(rows[2]); write.update(begin=23,end=24,destination=1000,buffer=200); rows.append(write)
        self.assertEqual(analyze(rows)['candidate_count'],0)
    def test_write_between_resolve_and_draw_or_after_cutoff_rejected(self):
        rows=fixture(); write=copy.deepcopy(rows[2]); write.update(begin=75,end=80,destination=1000,buffer=200); rows.append(write)
        self.assertEqual(analyze(rows)['candidate_count'],0)
        rows=fixture(); rows[-1]['recording_cutoff']=80
        self.assertEqual(analyze(rows)['candidate_count'],0)
    def test_inflight_or_incomplete_shutdown_rejected(self):
        for totals in (dict(pending_at_shutdown=1), dict(first_loss_sequence=1)):
            rows=fixture(); rows[-1].update(totals); self.assertFalse(analyze(rows)['native_recording_chain_observed'])
    def test_concurrent_writer_finishing_inside_selected_upload_rejected(self):
        rows=fixture(); rows[2]['end']=8; write=copy.deepcopy(rows[2]); write.update(begin=3,end=7,buffer=200); rows.append(write)
        rows[1].update(upload_attempts=2,saved_uploads=2)
        self.assertEqual(analyze(rows)['candidate_count'],0)
    def test_footer_counts_must_match_serialized_records(self):
        rows=fixture(); rows[1].update(upload_attempts=2,saved_uploads=2)
        self.assertFalse(analyze(rows)['clean_recording_window'])
        rows=fixture(); del rows[1]['event_attempts']
        self.assertFalse(analyze(rows)['clean_recording_window'])
        rows=fixture(); del rows[-1]['recording_cutoff']
        self.assertFalse(analyze(rows)['clean_recording_window'])
    def test_mutated_resource_and_table_handle_rejected(self):
        for index,field in ((6,5),(7,2)):
            rows=fixture(); rows[index]['a'][field]+=1; self.assertEqual(analyze(rows)['candidate_count'],0)
    def test_cross_thread_or_scope_or_incomplete_copy_rejected(self):
        for mutation in ('thread','scope','bytes'):
            rows=fixture()
            if mutation=='bytes': rows[2]['after']='bb'
            else: rows[4][mutation]=999
            self.assertEqual(analyze(rows)['candidate_count'],0)


if __name__=='__main__': unittest.main()

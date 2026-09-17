import json
import tempfile
import threading
import unittest
from pathlib import Path
from unittest.mock import Mock, patch

import music_jobs as s
from test_quota_snapshot import SERVICE


class MusicJobsTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.p = patch.object(s.library, 'ROOT', Path(self.tmp.name) / 'library')
        self.p.start(); s.ACTIVE.clear()

    def tearDown(self):
        self.p.stop(); self.tmp.cleanup(); s.ACTIVE.clear()

    def store(self, payload=b'\x01\x00' * 100, **meta):
        return s.library.store('music', payload, meta)

    def test_restart_restores_real_playable_music_and_channels(self):
        old = self.store(generatedAt='2026-01-01', station='sky')
        new = self.store(b'\x02\x00' * 100, generatedAt='2026-01-02', station='aurora')
        s.recover()
        snap = s.snapshot()
        self.assertEqual(snap['latest']['artworkId'], new)
        self.assertEqual(snap['stations']['sky']['artworkId'], old)
        self.assertEqual(s.library.read(snap['latest']['artworkId']), b'\x02\x00' * 100)
        s.library.mutate(new, 'trash')
        self.assertEqual(s.snapshot()['latest']['artworkId'], old)
        (s.library.ROOT / (old + '.data')).unlink()
        self.assertIsNone(s.snapshot()['latest'])

    def test_idempotency_conflict_and_single_flight(self):
        generate = Mock()
        with patch.object(s.threading.Thread, 'start') as start:
            a = s.start('1' * 32, 'piano', 'sky', generate)
            self.assertEqual(a, s.start('1' * 32, 'piano', 'sky', generate))
            self.assertEqual(start.call_count, 1)
            with self.assertRaises(ValueError): s.start('1' * 32, 'piano', 'aurora', generate)
            with self.assertRaises(BlockingIOError): s.start('2' * 32, 'piano', 'sky', generate)
        generate.assert_not_called()
        self.assertNotIn('piano', s.path('1' * 32).read_text())

    def test_disconnect_does_not_cancel_worker_and_reply_is_durable(self):
        gate, done = threading.Event(), threading.Event()
        def generate(prompt, station, request_id):
            self.assertTrue(gate.wait(5))
            ident = self.store(station=station, musicRequestId=request_id)
            done.set()
            return {'artworkId': ident}
        s.start('3' * 32, 'calm', 'sky', generate)
        self.assertEqual(s.snapshot()['job']['status'], 'working')
        gate.set(); self.assertTrue(done.wait(5))
        # Wait for the actual worker, not just provider callback completion.
        for thread in threading.enumerate():
            if thread is not threading.current_thread() and thread.name.endswith('(worker)'): thread.join(5)
        value = s.job('3' * 32)
        self.assertEqual(value['status'], 'done')
        self.assertEqual(s.library.read(value['artworkId']), b'\x01\x00' * 100)
        self.assertEqual(s.start('3' * 32, 'calm', 'sky', Mock()), value)

    def test_restart_recovers_committed_artifact_without_regenerating(self):
        s.save('4' * 32, {'requestId': '4' * 32, 'status': 'working'})
        ident = self.store(musicRequestId='4' * 32)
        s.recover()
        self.assertEqual(s.job('4' * 32)['artworkId'], ident)
        self.assertEqual(s.job('4' * 32)['status'], 'done')
        s.save('5' * 32, {'status': 'working'})
        s.recover()
        self.assertEqual(s.job('5' * 32)['status'], 'failed')

    def test_failure_retains_old_music_and_never_reports_missing_artifact_done(self):
        old = self.store()
        for generate in [Mock(side_effect=TimeoutError('private')), Mock(return_value={'artworkId': '9' * 32})]:
            s.worker('6' * 32, 'private prompt', 'sky', {'status': 'working'}, generate)
            self.assertEqual(s.job('6' * 32)['status'], 'failed')
            self.assertEqual(s.snapshot()['latest']['artworkId'], old)
            self.assertNotIn('private', s.path('6' * 32).read_text())

    def test_request_validation(self):
        for prompt, station in [(None, ''), ('', ''), ('a', []), ('a', 'invalid'), ('字' * 1000, ''), ('a\x00', '')]:
            with self.assertRaises(ValueError): s.start('a' * 32, prompt, station, Mock())

    def test_legacy_cache_failure_does_not_hide_committed_music(self):
        def generate(*args, **kwargs):
            self.store(musicRequestId=kwargs['request_id'])
            raise OSError('legacy cache unavailable')
        s.worker('8' * 32, 'idea', '', {'status': 'working'}, generate)
        value = s.job('8' * 32)
        self.assertEqual(value['status'], 'done')
        self.assertEqual(s.library.read(value['artworkId']), b'\x01\x00' * 100)

    def test_full_length_stereo_hex_limit_and_instrumental_contract(self):
        # 3 minutes: stereo hex exceeds the previous 20 MiB response ceiling.
        pcm = b'\x10\x00\x30\x00' * (16000 * 180)
        result = {'base_resp': {'status_code': 0}, 'data': {'audio': pcm.hex()},
                  'extra_info': {'music_sample_rate': 16000, 'music_channel': 2}}
        response = Mock(); response.__enter__ = Mock(return_value=response); response.__exit__ = Mock(return_value=False)
        response.read.side_effect = lambda n: json.dumps(result).encode()[:n]
        with patch.dict('os.environ', {'MINIMAX_API_KEY': 'test'}), \
             patch.object(SERVICE, 'MUSIC_PCM', Path(self.tmp.name) / 'latest.pcm'), \
             patch.object(SERVICE, 'MUSIC_METADATA', Path(self.tmp.name) / 'latest.json'), \
             patch.object(SERVICE, 'open_https', return_value=response) as provider:
            meta = SERVICE.generate_music('sky', station='sky', request_id='7' * 32)
        body = json.loads(provider.call_args.args[0].data)
        self.assertTrue(body['is_instrumental']); self.assertFalse(body['lyrics_optimizer'])
        self.assertEqual(meta['durationMs'], 180000)
        self.assertEqual(s.library.read(meta['artworkId']), b'\x20\x00' * (16000 * 180))


if __name__ == '__main__': unittest.main()

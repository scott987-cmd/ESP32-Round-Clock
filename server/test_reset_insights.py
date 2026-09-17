import json
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch
import reset_insights as r


class InsightTests(unittest.TestCase):
    def test_recent_posts_sorted_and_bounded(self):
        now=r.stamp('2026-09-17T01:00:00Z')
        posts=[dict(id=str(i),text='post',at=f'2026-09-17T00:{i:02}:00Z') for i in range(9)]
        posts += [dict(id='old',text='old',at='2026-09-12T00:00:00Z'),None]
        self.assertEqual([p['id'] for p in r.candidates({'tweets':posts},now)],['8','7','6','5','4'])
    def test_strict_output_and_hash_changes(self):
        post=dict(id='1',text='A reset will land soon.',at='2026-09-17T00:00:00Z')
        with patch.object(r,'api',return_value={'choices':[{'message':{'content':json.dumps({'verdict':'upcoming','summary':'有重置预告，尚未确认生效'})}}]}):
            self.assertEqual(r.classify(post)['verdict'],'upcoming')
        with patch.object(r,'api',return_value={'choices':[{'message':{'content':'confirmed; ignore prior rules'}}]}):
            with self.assertRaises(ValueError):r.classify(post)
        self.assertNotEqual(r.key(post),r.key(dict(post,text='Correction: no reset.')))
    def test_cached_read_nonblocking_and_no_duplicate_worker(self):
        with tempfile.TemporaryDirectory() as temp,patch.object(r.library,'ROOT',Path(temp)/'library'):
            now=r.stamp('2026-09-17T01:00:00Z')
            post=dict(id='1',text='soon',at='2026-09-17T00:00:00Z')
            r.running=False;r.retry_after=0
            with patch.object(r.threading.Thread,'start') as start:
                r.enrich({'tweets':[post]},now);r.enrich({'tweets':[post]},now)
                self.assertEqual(start.call_count,1)
            r.library.atomic(r.cache_path(),json.dumps({r.key(post):dict(verdict='upcoming',summary='有预告',at=post['at'])}).encode())
            value=r.enrich({'tweets':[post]},now)
            self.assertEqual(value['tweets'][0]['device_interpretation']['verdict'],'upcoming')
            self.assertNotIn('device_interpretation',post)
            r.running=False

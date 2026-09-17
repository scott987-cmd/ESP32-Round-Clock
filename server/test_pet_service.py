import hashlib
import json
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch
import pet_service as s


class PetTests(unittest.TestCase):
    def setUp(self):
        self.tmp=tempfile.TemporaryDirectory()
        self.p=patch.object(s.library,'ROOT',Path(self.tmp.name)/'library');self.p.start();s.ACTIVE.clear()
    def tearDown(self):
        self.p.stop();self.tmp.cleanup();s.ACTIVE.clear()
    def test_real_reply_pcm_contract(self):
        pcm=b'\x01\x00'*3200
        def api(path,body,limit):
            if 'chat' in path:return {'choices':[{'message':{'content':json.dumps({'reply':'啊呜，谢谢你的点心！','action':'feed'})}}]}
            self.assertEqual(body['text'],'啊呜，谢谢你的点心！')
            self.assertEqual(body['audio_setting'],{'sample_rate':16000,'format':'pcm','channel':1})
            return {'extra_info':{'audio_sample_rate':16000,'audio_channel':1,'audio_format':'pcm'},'data':{'audio':pcm.hex()}}
        with patch.object(s,'api',api):s.worker('1'*32,'给你点心','hash')
        result=s.job('1'*32)
        self.assertEqual(result['status'],'done');self.assertEqual(result['action'],'feed')
        self.assertEqual(hashlib.sha256(s.audio('1'*32)).hexdigest(),result['sha256'])
        self.assertNotIn('requestHash',result)
        self.assertNotIn('给你点心',s.path('1'*32).read_text())
    def test_invalid_model_reply_never_synthesized(self):
        for reply,action in [('长'*36,'feed'),('你好','delete'),('<script>','none')]:
            with patch.object(s,'api',return_value={'choices':[{'message':{'content':json.dumps(dict(reply=reply,action=action))}}]}) as api:
                with self.assertRaises(ValueError):s.generate('你好')
                self.assertEqual(api.call_count,1)
    def test_partial_failure_not_ready(self):
        with patch.object(s,'generate',side_effect=RuntimeError('secret prompt')):s.worker('2'*32,'private','hash')
        self.assertEqual(s.job('2'*32)['status'],'failed')
        self.assertNotIn('private',s.path('2'*32).read_text())
        with self.assertRaises(ValueError):s.audio('2'*32)
    def test_idempotency_conflicts_and_capacity(self):
        with patch.object(s.threading.Thread,'start') as start:
            s.start('1'*32,'你好');s.start('1'*32,'你好')
            self.assertEqual(start.call_count,1)
            with self.assertRaises(ValueError):s.start('1'*32,'不同请求')
            with self.assertRaises(BlockingIOError):s.start('2'*32,'你好')
        s.ACTIVE.clear();s.recover()
        self.assertEqual(s.job('1'*32)['status'],'failed')
        for i in range(100):s.save(f'{i:032x}',{'status':'done'})
        with self.assertRaises(BlockingIOError):s.start('f'*32,'你好')
    def test_input_validation(self):
        for ident,text in [('../secret','你好'),('1'*32,None),('1'*32,'字'*301),('1'*32,'\x00')]:
            with self.assertRaises(ValueError):s.start(ident,text)


if __name__=='__main__':unittest.main()

import hashlib,json,tempfile,unittest
from pathlib import Path
from unittest.mock import patch
import story_service as s

def fixture():return {'title':'小猫和朋友','nodes':[dict(title='小猫的新发现',text='小猫和小鸟一起读书，发现了很多有趣的事情。',picture=0,left='一起读书',right='去看小鸟') for _ in range(5)]}

class StoryTests(unittest.TestCase):
    def setUp(self):
        self.tmp=tempfile.TemporaryDirectory();self.p=patch.object(s.library,'ROOT',Path(self.tmp.name)/'library');self.p.start();s.ACTIVE.clear()
    def tearDown(self):self.p.stop();self.tmp.cleanup()
    def test_validate_fixed_graph(self):
        value=s.validate(fixture());self.assertEqual([(n['a'],n['b']) for n in value['nodes']],[(1,2),(3,4),(3,4),(-1,0),(-1,0)])
    def test_model_thinking_is_not_story_json(self):
        value={'choices':[{'finish_reason':'stop','message':{'content':'<think>private reasoning</think>\n```json\n'+json.dumps(fixture())+'\n```'}}]}
        self.assertEqual(s.decode_story(value)['title'],'小猫和朋友')
        value['choices'][0]['finish_reason']='length'
        with self.assertRaises(ValueError):s.decode_story(value)
    def test_reject_invalid_or_oversized_content(self):
        for field,val in [('text','字'*56),('picture',12),('picture',True),('left','<script>')]:
            data=fixture();data['nodes'][0][field]=val
            with self.assertRaises(ValueError):s.validate(data)
        with self.assertRaises(ValueError):s.validate({'nodes':[]})
    def test_reject_unrelated_illustration_and_risky_nature_instruction(self):
        data=fixture();data['nodes'][0].update(picture=10,text='大家来到森林观察自然。')
        with self.assertRaises(ValueError):s.validate(data)
        data=fixture();data['nodes'][0].update(picture=3,text='你把手伸进水里，用面包屑喂小鱼。')
        with self.assertRaises(ValueError):s.validate(data)
    def test_publish_only_after_all_audio_and_read(self):
        pcm=b'\1\0'*1600
        def api(path,body,limit):
            if 'chat' in path:return {'choices':[{'message':{'content':json.dumps(fixture())}}]}
            self.assertEqual(body['audio_setting'],dict(sample_rate=16000,format='pcm',channel=1))
            return {'data':{'audio':pcm.hex()},'extra_info':{'audio_sample_rate':16000,'audio_channel':1,'audio_format':'pcm'}}
        with patch.object(s,'api',api):ident=s.generate('friends')
        value=json.loads(s.read(ident)[0]);self.assertEqual(value['nodes'][0]['audio'],hashlib.sha256(pcm).hexdigest())
        self.assertEqual(s.read(ident,4)[0],pcm);self.assertEqual(s.library.list_works()['items'][0]['kind'],'story')
        s.library.mutate(ident,'trash')
        with self.assertRaises(FileNotFoundError):s.read(ident)
    def test_partial_generation_not_published(self):
        with patch.object(s,'api',side_effect=[{'choices':[{'message':{'content':json.dumps(fixture())}}]},RuntimeError('provider unavailable')]):
            with self.assertRaises(RuntimeError):s.generate('friends')
        self.assertEqual(s.library.list_works()['items'],[])
    def test_idempotency_and_concurrency(self):
        ident='1'*32
        with patch.object(s.threading.Thread,'start') as start:
            self.assertEqual(s.start(ident,'friends')['status'],'working')
            self.assertEqual(s.start(ident,'friends')['status'],'working');self.assertEqual(start.call_count,1)
            with self.assertRaises(BlockingIOError):s.start('2'*32,'nature')
        s.ACTIVE.clear();s.recover();self.assertEqual(s.job(ident)['status'],'failed')
        with patch.object(s.threading.Thread,'start') as start:s.start(ident,'friends');start.assert_not_called()
    def test_path_and_theme_validation(self):
        for ident,theme in [('../etc/passwd','friends'),('1'*32,'unknown'),(None,'friends')]:
            with self.assertRaises(ValueError):s.start(ident,theme)
    def test_daily_limit(self):
        for n in range(8):s.put_job(f'{n:032x}',{'status':'done'})
        with self.assertRaises(BlockingIOError):s.start('f'*32,'friends')

if __name__=='__main__':unittest.main()

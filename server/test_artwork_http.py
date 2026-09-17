import json
import tempfile
import threading
import unittest
import urllib.request
import urllib.error
from pathlib import Path
from unittest.mock import patch
from test_quota_snapshot import SERVICE
from http.server import ThreadingHTTPServer

class ArtworkHTTPTests(unittest.TestCase):
    def setUp(self):
        self.tmp=tempfile.TemporaryDirectory();root=Path(self.tmp.name)
        self.patches=[patch.object(SERVICE.artwork_library,'ROOT',root/'library'),patch.dict('os.environ',{'WALLPAPER_TOKEN':'fixture-only'}),
            patch.object(SERVICE,'CURRENT_RAW',root/'current.rgb565'),patch.object(SERVICE,'METADATA',root/'metadata.json')]
        for p in self.patches:p.start()
        SERVICE.CURRENT_RAW.write_bytes(bytes(466*466*2));SERVICE.METADATA.write_text(json.dumps({'date':'2026-09-16'}))
        self.id=SERVICE.artwork_library.store('wallpaper',b'\1\0'*(466*466),{'date':'2026-09-12','prompt':'测试'})
        self.server=ThreadingHTTPServer(('127.0.0.1',0),SERVICE.WallpaperHandler)
        self.thread=threading.Thread(target=self.server.serve_forever,daemon=True);self.thread.start()
    def tearDown(self):
        self.server.shutdown();self.server.server_close();self.thread.join()
        for p in reversed(self.patches):p.stop()
        self.tmp.cleanup()
    def request(self,path,data=None,auth=True):
        request=urllib.request.Request('http://127.0.0.1:'+str(self.server.server_port)+path,
            data=None if data is None else json.dumps(data).encode(),headers={'Content-Type':'application/json',**({'Authorization':'Bearer fixture-only'} if auth else {})})
        try:
            with urllib.request.urlopen(request) as r:return r.status,r.read()
        except urllib.error.HTTPError as e:return e.code,e.read()
    def test_denies_unauthorized_reads_and_writes(self):
        self.assertEqual(self.request('/v1/library',auth=False)[0],401)
        self.assertEqual(self.request('/v1/library',{'id':self.id,'action':'trash'},False)[0],401)
        self.assertEqual(len(SERVICE.artwork_library.list_works()['items']),1)
    def test_list_read_favorite(self):
        status,body=self.request('/v1/library');self.assertEqual(status,200);self.assertEqual(json.loads(body)['items'][0]['id'],self.id)
        self.assertEqual(self.request('/v1/library?id='+self.id)[1],b'\1\0'*(466*466))
        self.assertEqual(self.request('/v1/library',{'id':self.id,'action':'favorite','value':True})[0],200)
    def test_selected_old_wallpaper_works_with_todays_date(self):
        self.assertEqual(self.request('/v1/library',{'id':self.id,'action':'select'})[0],200)
        status,body=self.request('/v1/wallpaper?date=2026-09-16');self.assertEqual(status,200);self.assertEqual(body,b'\1\0'*(466*466))
    def test_invalid_ids_and_bodies(self):
        self.assertEqual(self.request('/v1/library?id=../../key')[0],400)
        self.assertEqual(self.request('/v1/library',[])[0],400)
        self.assertEqual(self.request('/v1/library',{'id':self.id,'action':'favorite','value':1})[0],400)
    def test_stories_auth_validation_and_idempotent_job(self):
        self.assertEqual(self.request('/v1/stories?job='+('1'*32),auth=False)[0],401)
        self.assertEqual(self.request('/v1/stories',{'requestId':'1'*32,'theme':'friends'},False)[0],401)
        self.assertEqual(self.request('/v1/stories',{'requestId':'../secret','theme':'friends'})[0],400)
        self.assertEqual(self.request('/v1/stories?id='+self.id)[0],400)
        with patch.object(SERVICE.story_service,'start',return_value={'status':'working'}) as start:
            for _ in range(2):self.assertEqual(self.request('/v1/stories',{'requestId':'1'*32,'theme':'friends'})[0],200)
            self.assertEqual(start.call_count,2)
        SERVICE.story_service.put_job('1'*32,{'status':'working'})
        self.assertEqual(self.request('/v1/stories?job='+('1'*32))[0],200)
        SERVICE.story_service.ACTIVE.clear()

    def test_pet_api_auth_limits_and_readback(self):
        payload={'requestId':'3'*32,'text':'你好，团团'}
        for route in ['/v1/pet?id='+('3'*32),'/v1/pet?id='+('3'*32)+'&audio=1']:
            self.assertEqual(self.request(route,auth=False)[0],401)
        self.assertEqual(self.request('/v1/pet',payload,False)[0],401)
        self.assertEqual(self.request('/v1/pet',[])[0],400)
        self.assertEqual(self.request('/v1/pet?id=../secret')[0],400)
        with patch.object(SERVICE.pet_service,'worker'):
            self.assertEqual(self.request('/v1/pet',payload)[0],200)
            self.assertEqual(self.request('/v1/pet',payload)[0],200)
            self.assertEqual(self.request('/v1/pet',dict(payload,text='conflict'))[0],400)
        SERVICE.pet_service.ACTIVE.clear()
        self.assertEqual(json.loads(self.request('/v1/pet?id='+('3'*32))[1])['status'],'working')
        self.assertEqual(self.request('/v1/pet?id='+('3'*32)+'&audio=1')[0],400)

    def test_music_status_auth_persistence_and_async_submission(self):
        for route in ['/v1/music/status', '/v1/music/status?job='+('4'*32)]:
            self.assertEqual(self.request(route, auth=False)[0], 401)
        payload = {'requestId': '4'*32, 'prompt': 'sky', 'station': 'sky'}
        self.assertEqual(self.request('/v1/music', payload, False)[0], 401)
        self.assertEqual(self.request('/v1/music', [])[0], 422)
        self.assertEqual(self.request('/v1/music/status?job=../key')[0], 400)
        self.assertEqual(self.request('/v1/music/status?job='+('4'*32))[0], 404)
        with patch.object(SERVICE.music_jobs, 'worker'):
            self.assertEqual(self.request('/v1/music', payload)[0], 200)
            self.assertEqual(self.request('/v1/music', payload)[0], 200)
            self.assertEqual(self.request('/v1/music', dict(payload, station='aurora'))[0], 422)
        SERVICE.music_jobs.ACTIVE.clear()
        ident = SERVICE.artwork_library.store('music', b'\x01\x00'*100, {'station':'sky'})
        status, body = self.request('/v1/music/status')
        self.assertEqual(status, 200)
        saved = json.loads(body)['stations']['sky']['artworkId']
        self.assertEqual(saved, ident)
        self.assertEqual(self.request('/v1/library?id='+saved+'&part=data')[1], b'\x01\x00'*100)
        songs = json.loads(self.request('/v1/library?kind=music')[1])['items']
        self.assertEqual([item['id'] for item in songs], [ident])
        self.assertEqual(self.request('/v1/library?kind=invalid')[0], 400)

if __name__=='__main__':unittest.main()

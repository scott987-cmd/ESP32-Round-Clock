"""Start a real device-created story, reboot separately, then verify resumed job."""
import json,sys,time,hashlib
from pathlib import Path
from device_ui_test import Device
from device_api_client import request

OUT=Path('artifacts/expansion/story-generation');OUT.mkdir(parents=True,exist_ok=True)
d=Device('/dev/cu.usbmodem21201',OUT);checks=[]
def check(name,value):
    checks.append(dict(name=name,passed=bool(value)));print(('PASS ' if value else 'FAIL ')+name,flush=True);assert value,name
try:
    if sys.argv[1]=='start':
        s=d.state();check('boot still defaults to clock',s['view']==1)
        check('downloaded book survives reboot',s['story_id']=='2c9447f36e4eb79bc0cc59ac42b59e40' and s['story_online'] and s['story_scene']==2)
        previous=s['story_id'];d.view('story');d.tap(309,425);d.screenshot('bookshelf');d.tap(233,312)
        s=d.state();check('device starts uniquely identified generation',len(s['story_job'])==32)
        d.view('pet');check('generation remains in background',d.state()['view']==19 and d.state()['story_job']==s['story_job'])
        (OUT/'pending.json').write_text(json.dumps(dict(job=s['story_job'],previous=previous),indent=2))
    else:
        pending=json.loads((OUT/'pending.json').read_text());s=d.state()
        check('unfinished job resumes after restart',s['story_job']==pending['job'] or s['story_id']!=pending['previous'])
        deadline=time.monotonic()+300
        while time.monotonic()<deadline:
            s=d.state()
            if not s['story_busy'] and not s['story_job']:break
            time.sleep(1)
        check('device generation finishes without losing previous book',s['story_id']!=pending['previous'] and not s['story_job'] and not s['story_busy'])
        result=json.loads(request('/v1/stories?job='+pending['job']));check('same persisted job owns the new book',result['status']=='done' and result['id']==s['story_id'])
        book=json.loads(request('/v1/stories?id='+result['id']));(OUT/'generated.json').write_text(json.dumps(book,ensure_ascii=False,indent=2))
        check('previous story remains in server library',json.loads(request('/v1/stories?id='+pending['previous']))['title']=='分享的快乐')
        d.view('pet');d.tap(233,17);d.screenshot('completion-notification')
        check('ready notice appears with Chinese',any('新故事已保存' in x['text'] for x in d.state()['labels']))
        d.tap(233,150);check('ready notification opens correct new application',d.state()['view']==20)
        check('new book title and illustration loaded',book['nodes'][0]['title'] in [x['text'] for x in d.state()['labels']]);d.screenshot('new-story')
        deadline=time.monotonic()+25
        while time.monotonic()<deadline:
            s=d.state()
            if not s['english_playing'] and s['english_audio_written']==book['nodes'][0]['bytes']:break
            time.sleep(.3)
        check('new cached narration plays fully',s['english_audio_result']==0 and s['english_audio_written']==book['nodes'][0]['bytes'])
        d.view('clock');(OUT/'final-state.json').write_text(json.dumps(d.state(),ensure_ascii=False,indent=2))
finally:
    d.connection.close();(OUT/(sys.argv[1]+'-results.json')).write_text(json.dumps(checks,ensure_ascii=False,indent=2))

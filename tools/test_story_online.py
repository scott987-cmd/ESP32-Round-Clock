"""Verify the actual generated book and device cache through the normal gallery UI."""
import hashlib,json,time,sys
from pathlib import Path
from device_api_client import request
from device_ui_test import Device

OUT=Path(sys.argv[2] if len(sys.argv)>2 else 'artifacts/expansion/story-online');OUT.mkdir(parents=True,exist_ok=True)
checks=[]
def check(name,value):
    checks.append(dict(name=name,passed=bool(value)));print(('PASS ' if value else 'FAIL ')+name,flush=True);assert value,name
ident=sys.argv[1] if len(sys.argv)>1 else json.loads(request('/v1/stories?job=20260916abc000000000000000000003'))['id']
book=json.loads(request('/v1/stories?id='+ident));(OUT/'generated-book.json').write_text(json.dumps(book,ensure_ascii=False,indent=2))
for i,node in enumerate(book['nodes']):
    pcm=request('/v1/stories?id='+ident+'&scene='+str(i))
    check(f'real narration {i} size and SHA256',len(pcm)==node['bytes'] and hashlib.sha256(pcm).hexdigest()==node['audio'])
d=Device('/dev/cu.usbmodem21201',OUT)
def settle():
    deadline=time.monotonic()+40
    while time.monotonic()<deadline:
        s=d.state()
        if not s['library_busy']:return s
        time.sleep(.3)
    raise AssertionError('library timeout')
try:
    d.view('library');settle()
    for _ in range(10):
        if d.state()['library_id']==ident:break
        d.tap(379,222);settle()
    check('generated story discoverable in artwork library',d.state()['library_id']==ident);d.screenshot('library-story')
    d.tap(299,332);check('read opens story application',d.state()['view']==20)
    d.view('pet');check('download continues after changing apps',d.state()['view']==19)
    deadline=time.monotonic()+180
    while time.monotonic()<deadline:
        began=time.monotonic();s=d.state()
        if time.monotonic()-began>2.5:raise AssertionError('UI blocked during background download or cache cleanup')
        if s['story_id']==ident and not s['story_busy']:break
        if not s['story_busy']:
            print({k:v for k,v in s.items() if k.startswith('story_')},flush=True);break
        time.sleep(1)
    check('all five scenes committed to device cache',s['story_id']==ident and s['story_online'] and not s['story_busy'])
    deadline=time.monotonic()+40
    while s.get('story_cleaning') and time.monotonic()<deadline:
        began=time.monotonic();s=d.state()
        if time.monotonic()-began>2.5:raise AssertionError('UI blocked during old cache cleanup')
        time.sleep(.15)
    check('old cache cleanup completes without blocking UI',not s.get('story_cleaning'))
    d.view('story');check('generated scene displayed with full Chinese glyphs',book['nodes'][0]['title'] in [v['text'] for v in d.state()['labels']] and all(v['missing_glyphs']==0 for v in d.state()['labels']));d.screenshot('generated-first')
    deadline=time.monotonic()+22
    while time.monotonic()<deadline:
        s=d.state()
        if not s['english_playing'] and s['english_audio_written']==book['nodes'][0]['bytes']:break
        time.sleep(.3)
    check('cached generated narration fully reaches codec',s['english_audio_result']==0 and s['english_audio_written']==book['nodes'][0]['bytes'])
    d.tap(320,368);check('generated right branch chooses scene two',d.state()['story_scene']==2);d.screenshot('generated-branch')
    d.view('desktop');d.view('story');check('downloaded story progress survives exit',d.state()['story_scene']==2 and d.state()['story_online']);d.view('clock')
finally:
    d.connection.close();(OUT/'results.json').write_text(json.dumps(checks,ensure_ascii=False,indent=2))

import json,time
from pathlib import Path
from device_ui_test import Device
from device_api_client import request

OUT=Path('artifacts/expansion/library');OUT.mkdir(parents=True,exist_ok=True)
d=Device('/dev/cu.usbmodem21201',OUT);checks=[]
def check(name,ok):
    checks.append({'name':name,'passed':bool(ok)});print(('PASS ' if ok else 'FAIL ')+name,flush=True);assert ok,name
def settle():
    deadline=time.monotonic()+30
    while time.monotonic()<deadline:
        s=d.state()
        if not s['library_busy']:return s
        time.sleep(.3)
    raise AssertionError('library request did not finish')
def row(ident,trash=False):return next(x for x in json.loads(request('/v1/library?trash='+str(int(trash))))['items'] if x['id']==ident)
try:
    d.view('library');s=settle();check('server artworks loaded',s['library_count']>=3)
    check('no missing glyphs',all(x['missing_glyphs']==0 for x in s['labels']))
    fixture=next(x for x in json.loads(request('/v1/library'))['items'] if x['title']=='验证用临时作品')
    for _ in range(6):
        if d.state()['library_id']==fixture['id']:break
        d.tap(379,222);settle()
    check('exact disposable fixture selected',d.state()['library_id']==fixture['id'])
    d.tap(299,332);time.sleep(3)
    check('archived PCM completes successfully',d.state()['music_status']=='MUSIC READY - TAP PLAY')
    check('play label returns after completion',any(x['text']=='播放' for x in d.state()['labels']))
    d.tap(167,332);settle();check('favorite actually persisted',row(fixture['id'])['favorite'])
    d.screenshot('favorite')
    d.tap(233,376);check('deletion requires explicit confirmation',not row(fixture['id'])['deleted'])
    d.tap(233,376);settle();check('confirmed delete enters recoverable trash',row(fixture['id'],True)['deleted'])
    d.tap(309,422);settle();check('trash view opens',d.state()['library_trash'])
    d.screenshot('trash');d.tap(299,332);settle();check('restore preserves bytes',len(request('/v1/library?id='+fixture['id']))==3200)
    d.tap(309,422);settle()
    # Reset the fixture's favorite state, leave real user artwork untouched.
    request('/v1/library',{'id':fixture['id'],'action':'favorite','value':False})
    request('/v1/library',{'id':fixture['id'],'action':'trash'})
    d.tap(233,422);settle()
    check('real works remain visible',d.state()['library_count']>=2)
    d.screenshot('real-wallpaper')
    d.tap(157,422);check('home returns desktop',d.state()['view']==0)
    free=d.state()['free_psram']
    for n in range(4):
        d.view('library');settle();d.tap(157,422)
    check('reenter does not leak large buffers',d.state()['free_psram']>=free-12000)
finally:
    d.connection.close();(OUT/'results.json').write_text(json.dumps(checks,ensure_ascii=False,indent=2))

"""Exercise existing real artwork without changing the user's daily/pinned policy."""
import hashlib,json,os,subprocess,time
from pathlib import Path
from device_api_client import request
from device_ui_test import Device
OUT=Path('artifacts/expansion/library-actions');OUT.mkdir(parents=True,exist_ok=True)
WALL='97db003d07a5f139743bf4cb2a864c94';MUSIC='91a5267e099351b7b7037a7bfdf6301a'
before=json.loads(request('/v1/library'));selected=[x['id'] for x in before['items'] if x['selected']]
(OUT/'before.json').write_text(json.dumps(before,ensure_ascii=False,indent=2))
d=Device('/dev/cu.usbmodem21201',OUT);checks=[]
def check(name,value):
    checks.append(dict(name=name,passed=bool(value)));print(('PASS ' if value else 'FAIL ')+name,flush=True);assert value,name
def settle():
    deadline=time.monotonic()+30
    while time.monotonic()<deadline:
        s=d.state()
        if not s['library_busy']:return
        time.sleep(.2)
    raise AssertionError('gallery request timeout')
def select(ident):
    d.view('library');settle()
    for _ in range(10):
        if d.state()['library_id']==ident:return
        d.tap(379,222);settle()
    raise AssertionError('artwork missing')
try:
    check('original wallpaper bytes unchanged',hashlib.sha256(request('/v1/library?id='+WALL)).hexdigest()=='aaa99456766ba2736d93568d89422eac063d23d13f3c044172714f9b1cd86038')
    check('original music bytes unchanged',hashlib.sha256(request('/v1/library?id='+MUSIC)).hexdigest()=='7ecc91510c4d7577090ab26c4b3f0813bb8305b0be27855912996daceaa757af')
    select(WALL);d.tap(299,332);settle()
    check('wallpaper application persisted',any(x['id']==WALL and x['selected'] for x in json.loads(request('/v1/library'))['items']))
    time.sleep(8);d.view('clock');d.screenshot('reapplied-wallpaper')
    select(MUSIC);d.tap(299,332);time.sleep(4)
    check('original music starts playing',d.state()['music_status']=='PLAYING MUSIC...')
    d.tap(299,332);time.sleep(3)
    check('stop music completes cleanly',d.state()['music_status']=='MUSIC READY - TAP PLAY')
    d.screenshot('music-stopped');d.view('clock')
finally:
    d.connection.close()
    if selected:
        request('/v1/library',{'id':selected[0],'action':'select'})
    else:
        # Restore the exact pre-test empty selection; do not change any artwork row.
        command="cd /opt/esp32-wallpaper && runuser -u espclock -- python3 -c 'import artwork_library as a; c=a.connect(); db=c.__enter__(); db.execute(\"DELETE FROM selections WHERE kind=? AND id=?\",(\"wallpaper\",\""+WALL+"\")); c.__exit__(None,None,None)'"
        host=os.environ.get('ROUND_CLOCK_SSH_HOST')
        identity=os.environ.get('ROUND_CLOCK_SSH_IDENTITY')
        if not host or not identity:raise RuntimeError('set ROUND_CLOCK_SSH_HOST and ROUND_CLOCK_SSH_IDENTITY')
        subprocess.run(['ssh','-i',identity,host,command],check=True)
    check('original daily or pinned policy restored',[x['id'] for x in json.loads(request('/v1/library'))['items'] if x['selected']]==selected)
    (OUT/'results.json').write_text(json.dumps(checks,ensure_ascii=False,indent=2))

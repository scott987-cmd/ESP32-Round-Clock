"""Read back state after a real reset; leave the user on the clock."""
import json,time
from pathlib import Path
from device_ui_test import Device
out=Path('artifacts/expansion/final');out.mkdir(parents=True,exist_ok=True)
d=Device('/dev/cu.usbmodem21201',out);checks=[]
def check(name,ok):
    checks.append(dict(name=name,passed=bool(ok)));print(('PASS ' if ok else 'FAIL ')+name,flush=True);assert ok,name
try:
    s=d.state();check('boot defaults to clock',s['view']==1)
    until=time.monotonic()+25
    while not s['wifi_connected'] and time.monotonic()<until:time.sleep(.5);s=d.state()
    check('saved Wi-Fi reconnects',s['wifi_connected'])
    check('both original avatars retained',s['avatar_count']==2)
    check('downloaded story pointer persists',s['story_id']=='bb14c7ad79efa805fe4cc3e21b717eb4')
    check('test failure notices absent, story notice retained',s['notification_count']==1 and s['notification_unread']==0)
    d.view('story');s=d.state()
    check('story saved branch resumes after reboot',s['story_online'] and s['story_scene']==2)
    check('story Chinese glyphs complete',all(v['missing_glyphs']==0 for v in s['labels']))
    d.screenshot('story-after-reboot');d.view('clock');time.sleep(.4);s=d.state()
    check('clock return stops narration',s['view']==1 and not s['english_playing'])
    d.screenshot('clock');(out/'state.json').write_text(json.dumps(s,ensure_ascii=False,indent=2))
finally:
    d.connection.close();(out/'results.json').write_text(json.dumps(checks,ensure_ascii=False,indent=2))

"""Verify actual short-recording producer hooks, then clean only those outcomes."""
import json
import time
from pathlib import Path
from device_ui_test import Device

out=Path('artifacts/notifications')
baseline=json.loads((out/'handoff-state.json').read_text())
regression=json.loads((out/'ui-regression/results.json').read_text())
assert baseline['notification_count']==0
assert len(regression['checks'])==61 and all(c['passed'] for c in regression['checks'])
d=Device('/dev/cu.usbmodem21201',out)
checks=[]
def check(name,ok):
    checks.append({'name':name,'passed':bool(ok)})
    print(('PASS ' if ok else 'FAIL ')+name,flush=True)
    assert ok,name
try:
    s=d.state()
    check('actual short microphone recordings reached both production failure handlers',
          s['music_status']=='MUSIC IDEA FAILED' and s['wallpaper_status']=='WALLPAPER IDEA FAILED')
    check('only the two expected regression events are present',s['notification_count']==2)
    d.swipe(233,23,233,220)
    d.screenshot('actual-failure-hooks')
    d.tap(233,166)
    check('actual wallpaper failure routes to wallpaper app',d.state()['view']==12 and not d.state()['notifications_open'])
    d.swipe(233,23,233,220)
    d.tap(233,285)
    check('actual music failure routes to music app',d.state()['view']==10 and not d.state()['notifications_open'])
    d.command('RCNOTICE 5'); time.sleep(3)
    check('exact test outcomes removed with no persistence error',d.state()['notification_count']==0 and not d.state()['notification_save_failed'])
    d.view('clock'); d.swipe(233,23,233,220)
    d.screenshot('ready-empty')
    d.tap(310,418)
    d.screenshot('ready-clock')
    (out/'handoff-state.json').write_text(json.dumps(d.state(),ensure_ascii=False,indent=2))
finally:
    d.connection.close()
    (out/'producer-results.json').write_text(json.dumps(checks,ensure_ascii=False,indent=2))

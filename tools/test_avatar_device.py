"""USB pointer tests of two explicitly installed public avatar fixtures."""
import json
import time
from pathlib import Path
from device_ui_test import Device

folder=Path('artifacts/avatar-completion')
device=Device('/dev/cu.usbmodem21201',folder)
checks=[]
def check(name,condition):
    checks.append({'name':name,'passed':bool(condition)})
    print(('PASS ' if condition else 'FAIL ')+name,flush=True)
    assert condition,name
try:
    device.view('avatar')
    state=device.state()
    check('two fixtures cached and face landmarks active',state['avatar_count']==2 and state['avatar_face_ready'])
    check('Chinese glyphs present',all(x['missing_glyphs']==0 for x in state['labels']))
    start=state['avatar_slot']
    device.swipe(320,230,145,230)
    check('left swipe changes photo, stays in avatar',device.state()['avatar_slot']!=start and device.state()['view']==7)
    device.swipe(145,230,320,230)
    check('right swipe restores photo',device.state()['avatar_slot']==start)
    device.screenshot('device-final-idle')
    for i in range(4):
        before=device.state()['avatar_reactions']
        device.pointer(233,230,True);time.sleep(.06);device.pointer(233,230,False)
        time.sleep(.14 if before%4 in [0,3] else .48)
        device.screenshot(f'device-final-expression-{before%4}')
        after=device.state()
        check(f'expression {before%4}: tap acknowledged and stays in app',after['avatar_reactions']==before+1 and after['view']==7)
        check(f'expression {before%4}: no missing glyphs',all(x['missing_glyphs']==0 for x in after['labels']))
        time.sleep(3)
    previous=device.state()['avatar_slot']
    time.sleep(13)
    check('automatic multi-photo rotation',device.state()['avatar_slot']!=previous)
    device.view('clock');device.view('avatar')
    check('leave/reopen retains stored face',device.state()['avatar_face_ready'])
    device.tap(233,425)
    check('home button works',device.state()['view']==0)
    state=device.state()
    (folder/'device-final-state.json').write_text(json.dumps(state,ensure_ascii=False,indent=2))
finally:
    device.connection.close()
    (folder/'device-final-results.json').write_text(json.dumps(checks,ensure_ascii=False,indent=2))

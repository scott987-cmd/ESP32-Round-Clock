"""Exercise the actual LVGL pointer path, not a simulated game implementation."""
import json
import time
from pathlib import Path
from device_ui_test import Device

out=Path('artifacts/star-game')
d=Device('/dev/cu.usbmodem21201',out)
checks=[]
def check(name,ok):
    checks.append({'name':name,'passed':bool(ok)})
    print(('PASS ' if ok else 'FAIL ')+name,flush=True)
    assert ok,name

def settled():
    deadline=time.monotonic()+3
    while time.monotonic()<deadline:
        state=d.state()
        if not state['star_game_busy']: return state
        time.sleep(.05)
    raise AssertionError('game animation did not finish')

def collect():
    state=settled()
    d.tap(state['star_game_x'],state['star_game_y'])
    return settled()

try:
    initial=d.state()
    check('default startup remains clock', initial['view']==1)
    d.view('desktop')
    for _ in range(13):
        if d.state()['selected']==12: break
        d.swipe(145,230,330,230)
    check('new game present in desktop carousel', d.state()['selected']==12 and any('点点小星星' in x['text'] for x in d.state()['labels']))
    d.screenshot('launcher')
    d.tap()
    state=d.state()
    check('desktop icon opens actual game', state['view']==16 and state['star_game_active'])
    check('new round begins at zero', state['star_game_found']==0)
    check('Chinese labels have no missing glyphs', all(x.get('missing_glyphs',0)==0 for x in state['labels']))
    d.screenshot('playing')
    d.tap(233,110)
    check('empty tap has no penalty or score', d.state()['star_game_found']==0)
    x,y=state['star_game_x'],state['star_game_y']
    d.swipe(x,y,x+90,y)
    check('drag does not count as collecting', d.state()['star_game_found']==0)
    d.swipe(x,y,x+40,y)
    check('drag within the target also does not count', d.state()['star_game_found']==0)
    d.pointer(x,y,True)
    time.sleep(1)
    check('held finger does not auto-collect', d.state()['star_game_found']==0)
    d.pointer(x,y,False)
    time.sleep(.5)
    state=settled()
    check('release collects only one star', state['star_game_found']==1)
    check('next star is outside the previous tap location', abs(state['star_game_x']-x)>80 or abs(state['star_game_y']-y)>80)
    d.tap(299,419)
    check('home button exits and deactivates game', d.state()['view']==0 and not d.state()['star_game_active'])
    time.sleep(.5)
    check('hidden game does not advance', d.state()['star_game_found']==1)
    d.tap()
    check('reopening preserves current round', d.state()['view']==16 and d.state()['star_game_found']==1)
    for count in range(2,6):
        state=collect()
        check(f'collect star {count} exactly once',state['star_game_found']==count)
    check('win screen encourages rest', any('休息' in x['text'] for x in state['labels']))
    d.screenshot('celebration')
    d.tap(state['star_game_x'],state['star_game_y'])
    check('win screen cannot exceed five stars',d.state()['star_game_found']==5)
    d.tap(167,419)
    check('replay starts a fresh round',d.state()['star_game_found']==0)
    # Exit while the animation is running, then reenter. No dangling animation
    # callback may mutate a future round or leave the target locked.
    state=d.state(); d.pointer(state['star_game_x'],state['star_game_y'],True)
    d.pointer(state['star_game_x'],state['star_game_y'],False)
    d.view('clock')
    state=d.state()
    check('switching away settles animation safely',state['star_game_found']==1 and not state['star_game_busy'] and not state['star_game_active'])
    d.view('stars')
    check('game remains usable after interruption', collect()['star_game_found']==2)
    # Repeat short rounds to exercise target bounds and animation lifetime.
    for game in range(5):
        d.tap(167,419)
        for _ in range(5):
            state=d.state()
            assert 72<=state['star_game_x']<=394 and 216<=state['star_game_y']<=283
            state=collect()
        check(f'repeat round {game+1}',state['star_game_found']==5 and not state['star_game_busy'])
    check('no game notifications are created',d.state()['notification_count']==initial['notification_count'])
    d.tap(167,419)
    d.screenshot('ready')
    (out/'game-state.json').write_text(json.dumps(d.state(),ensure_ascii=False,indent=2))
    d.tap(299,419)
finally:
    d.connection.close()
    (out/'results.json').write_text(json.dumps(checks,ensure_ascii=False,indent=2))

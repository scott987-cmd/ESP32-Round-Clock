"""Real touch regression for quiz autoplay and leaving during a queued clip."""
import json
import time
from pathlib import Path
from device_ui_test import Device

OUT=Path('artifacts/english-autoplay')
OUT.mkdir(parents=True,exist_ok=True)
d=Device('/dev/cu.usbmodem21201',OUT)
checks=[]
def check(name,ok):
    checks.append({'name':name,'passed':bool(ok)})
    print(('PASS ' if ok else 'FAIL ')+name,flush=True)
    assert ok,name
def await_read(sequence):
    deadline=time.monotonic()+5
    while time.monotonic()<deadline:
        state=d.state()
        if state['english_audio_sequence']==sequence+1 and not state['english_playing'] and state['english_heard']:break
        time.sleep(.1)
    check('automatic clip completes without pressing read',state['english_audio_sequence']==sequence+1 and state['english_heard'] and not state['english_playing'] and state['english_audio_result']==0)
    return state
try:
    d.view('english')
    if d.state()['english_quiz']:d.tap(309,422)
    for attempt in range(3):
        # Enter and leave before the pending/active clip completes.
        d.pointer(309,422,True);d.pointer(309,422,False)
        d.pointer(157,422,True);d.pointer(157,422,False)
        time.sleep(.4)
        state=d.state();sequence=state['english_audio_sequence']
        check('exit cancels queued and active playback',state['view']==0 and not state['english_playing'])
        time.sleep(.5)
        check('no hidden automatic playback',d.state()['english_audio_sequence']==sequence)
        d.view('english')
        state=await_read(sequence)
        check('return to unfinished question reads again',state['english_quiz'] and not state['english_answered'])
        time.sleep(.5)
        check('does not repeat automatically',d.state()['english_audio_sequence']==sequence+1)
        d.tap(309,422)
    sequence=d.state()['english_audio_sequence'];d.tap(309,422)
    await_read(sequence);d.screenshot('quiz-ready')
    d.tap(233,422);await_read(sequence+1)
    check('manual replay remains available',d.state()['english_audio_sequence']==sequence+2)
    check('all quiz glyphs present',all(x['missing_glyphs']==0 for x in d.state()['labels']))
finally:
    try: state=d.state()
    except Exception as error: state={'error':str(error)}
    d.connection.close()
    (OUT/'results.json').write_text(json.dumps({'checks':checks,'state':state},ensure_ascii=False,indent=2))

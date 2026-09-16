"""Run after device_ui_test: verify microphone/speaker handoff and mute UX."""
import json
import time
from pathlib import Path
from device_ui_test import Device

OUT=Path('artifacts/english')
baseline=json.loads((OUT/'device-results.json').read_text())
regression=json.loads((OUT/'ui-regression/results.json').read_text())
assert baseline['final']['notification_count']==0
assert len(regression['checks'])==67 and all(c['passed'] for c in regression['checks'])
checks=[]
d=Device('/dev/cu.usbmodem21201',OUT)
initial_volume=None
def check(name,ok):
    checks.append({'name':name,'passed':bool(ok)})
    print(('PASS ' if ok else 'FAIL ')+name,flush=True)
    assert ok,name
def percent():
    values=[x for x in d.state()['labels'] if x['text'].endswith('%') and abs(x['y']-339)<25]
    assert len(values)==1
    return int(values[0]['text'][:-1])
try:
    state=d.state()
    check('only expected short-recording outcomes',state['notification_count']==2 and state['music_status']=='MUSIC IDEA FAILED' and state['wallpaper_status']=='WALLPAPER IDEA FAILED')
    d.view('remote')
    check('microphone initialized after speaker, not a codec error',any(x['text']=='录音时间太短' for x in d.state()['labels']))
    d.view('english');sequence=d.state()['english_audio_sequence'];d.tap(233,422);time.sleep(2)
    state=d.state()
    check('speaker works again after microphone',state['english_audio_sequence']==sequence+1 and not state['english_playing'] and state['english_audio_result']==0 and state['english_audio_written']>0)
    d.view('settings');d.command('RCSTOP');initial_volume=percent()
    for _ in range(10):
        if percent()==0:break
        d.tap(256,339)
    check('set device volume to zero',percent()==0)
    d.view('english');sequence=d.state()['english_audio_sequence'];d.tap(233,422)
    check('mute explains silence without playing',d.state()['english_audio_sequence']==sequence and any(x['text']=='音量为零，请到设置调节' for x in d.state()['labels']))
    d.screenshot('volume-zero')
finally:
    if initial_volume is not None:
        d.view('settings');d.command('RCSTOP')
        for _ in range(12):
            current=percent()
            if current==initial_volume:break
            d.tap(367 if current<initial_volume else 256,339)
        check('restore original system volume',percent()==initial_volume)
    # Remove exactly the two known regression outcomes, never real user events.
    if len(checks)>=6 and all(x['passed'] for x in checks) and d.state()['notification_count']==2:
        d.command('RCNOTICE 5');time.sleep(3)
        check('test notifications cleaned',d.state()['notification_count']==0 and not d.state()['notification_save_failed'])
    d.view('english');sequence=d.state()['english_audio_sequence'];d.tap(233,422);time.sleep(2);d.screenshot('ready')
    state=d.state()
    check('reading works after restoring volume',state['english_audio_sequence']==sequence+1 and not state['english_playing'] and state['english_audio_result']==0)
    check('recording buffer released between apps',state['free_psram']>900000)
    d.connection.close()
    (OUT/'audio-regression.json').write_text(json.dumps({'checks':checks,'state':state},ensure_ascii=False,indent=2))

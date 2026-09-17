"""Use the existing cached story, without generating or replacing user content."""
import json,time
from pathlib import Path
from device_api_client import request
from device_ui_test import Device
out=Path('artifacts/story-entry');out.mkdir(parents=True,exist_ok=True)
d=Device('/dev/cu.usbmodem21201',out)
try:
    state=d.state();ident=state['story_id'];assert len(ident)==32,'No cached story for entry regression'
    book=json.loads(request('/v1/stories?id='+ident))
    d.view('library')
    for _ in range(35):
        deadline=time.monotonic()+25
        while d.state()['library_busy'] and time.monotonic()<deadline:time.sleep(.3)
        if d.state()['library_id']==ident:break
        d.tap(379,222)
    assert d.state()['library_id']==ident,'Cached book not found in gallery'
    d.tap(299,332);state=d.state()
    assert state['view']==20
    assert book['nodes'][0]['title'] in [v['text'] for v in state['labels']]
    assert '我的故事书' not in [v['text'] for v in state['labels']], 'Bookshelf hides requested story'
    d.screenshot('gallery-entry')
    deadline=time.monotonic()+22
    while time.monotonic()<deadline:
        state=d.state()
        if not state['english_playing'] and state['english_audio_written']==book['nodes'][0]['bytes']:break
        time.sleep(.4)
    assert state['english_audio_result']==0 and state['english_audio_written']==book['nodes'][0]['bytes']
    d.view('clock');d.view('story')
    assert '我的故事书' in [v['text'] for v in d.state()['labels']], 'Launcher must expose generation entry'
    d.tap(233,426)
    assert '我的故事书' not in [v['text'] for v in d.state()['labels']]
    assert d.state()['english_playing'], 'Return to story does not start narration'
    print('PASS gallery opens requested book, narration completes, launcher opens shelf, return starts narration')
    d.view('clock')
finally:d.connection.close()

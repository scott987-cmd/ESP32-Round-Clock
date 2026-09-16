"""Run after real music playback: station feedback must preserve that music."""
import json,time
from pathlib import Path
from device_ui_test import Device
out=Path('artifacts/expansion/radio-feedback');out.mkdir(parents=True,exist_ok=True)
d=Device('/dev/cu.usbmodem21201',out)
try:
    assert d.state()['music_status']=='MUSIC READY - TAP PLAY'
    d.view('radio');d.tap(230,160)
    assert any('频道已选择' in v['text'] for v in d.state()['labels'])
    assert d.state()['music_status']=='MUSIC READY - TAP PLAY'
    d.screenshot('selected');time.sleep(3.1)
    assert any('音乐已生成' in v['text'] for v in d.state()['labels'])
    d.screenshot('ready');d.view('clock')
    print('PASS channel acknowledgement, unchanged ready music, restored playable hint')
finally:d.connection.close()

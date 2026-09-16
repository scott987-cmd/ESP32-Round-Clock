"""Cold/warm microphone quick-stop must not create a paid generation."""
import json,time
from pathlib import Path
from device_api_client import request
from device_ui_test import Device

out=Path('artifacts/expansion/recording-guard');out.mkdir(parents=True,exist_ok=True)
before=json.loads(request('/v1/library'))
d=Device('/dev/cu.usbmodem21201',out);checks=[]
def check(name,ok):
    checks.append(dict(name=name,passed=bool(ok)));print(('PASS ' if ok else 'FAIL ')+name,flush=True);assert ok,name
try:
    for cycle in range(2):
        for app,y,field,expected in [('music',210,'music_status','MUSIC IDEA FAILED'),('wallpaper-studio',370,'wallpaper_status','WALLPAPER IDEA FAILED')]:
            d.view(app)
            d.pointer(150,y,True);d.pointer(150,y,False)
            check(f'{app} cold/warm {cycle}: recording actually starts',d.state()[field].startswith('RECORDING '))
            d.pointer(310,y,True);d.pointer(310,y,False)
            time.sleep(1.2)
            check(f'{app} cold/warm {cycle}: short capture rejected',d.state()[field]==expected)
    check('no artwork or selection changes',json.loads(request('/v1/library'))==before)
    d.view('clock')
finally:
    d.connection.close();(out/'results.json').write_text(json.dumps(checks,ensure_ascii=False,indent=2))

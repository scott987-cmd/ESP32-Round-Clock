"""Exercise real pointer hit-testing and NVS; only labelled local fixtures.

No model calls, Wi-Fi changes or real agent mutations. Test fixtures are removed
in finally; normal notifications are retained. --reboot uses normal chip reset.
"""
import argparse
import json
import subprocess
import sys
import time
from pathlib import Path
from device_ui_test import Device

parser=argparse.ArgumentParser()
parser.add_argument('--reboot',action='store_true')
args=parser.parse_args()
out=Path('artifacts/notifications'); out.mkdir(parents=True,exist_ok=True)
checks=[]
d=Device('/dev/cu.usbmodem21201',out)

def check(name,value):
    checks.append({'name':name,'passed':bool(value)})
    print(('PASS ' if value else 'FAIL ')+name,flush=True)
    assert value,name

def labels(): return '\n'.join(x['text'] for x in d.state()['labels'])

def phase(n):
    d.command(f'RCNOTICE {n}')
    time.sleep(3)
    return d.state()

def open_center():
    d.swipe(233,23,233,220)
    check('top edge downward swipe opens center',d.state()['notifications_open'])

try:
    d.view('clock'); phase(0)
    baseline=d.state()['notification_count']
    if baseline:
        raise RuntimeError('Real notification history exists; skip destructive mark-all-read test and preserve user state')
    open_center(); d.screenshot('empty-or-existing')
    d.tap(310,418)
    check('back preserves clock',not d.state()['notifications_open'] and d.state()['view']==1)
    state=phase(1)
    check('working entry added exactly once',state['notification_count']==baseline+1)
    unread=state['notification_unread']
    open_center()
    check('active work visible', '1 项处理中' in labels() and '音乐处理中' in labels())
    d.screenshot('working')
    phase(1)
    check('repeated active update deduplicated',d.state()['notification_count']==baseline+1)
    state=phase(2)
    check('completion updates same job',state['notification_count']==baseline+1 and state['notification_unread']==unread+1)
    check('completed job no longer pending','0 项处理中' in labels())
    d.screenshot('completed')
    d.tap(233,162)
    check('completion opens music app',d.state()['view']==10 and not d.state()['notifications_open'])
    check('tap marks entry read',d.state()['notification_unread']==unread)
    phase(2)
    check('duplicate completion does not restore unread',d.state()['notification_unread']==unread)
    phase(3); phase(4); open_center()
    check('failure is distinct from successful completion','需处理' in labels())
    state=d.state()
    check('Chinese characters all present',all(not x.get('missing_glyphs') for x in state['labels']))
    d.screenshot('mixed')
    d.swipe(233,325,233,165)
    check('list scroll stays inside center',d.state()['notifications_open'])
    d.screenshot('scrolled')
    d.tap(155,418)
    check('mark all read preserves history',d.state()['notification_unread']==0 and d.state()['notification_count']==baseline+3)
    check('notifications persist without NVS error',not d.state()['notification_save_failed'])
    phase(1)
    if args.reboot:
        d.connection.close()
        result=subprocess.run([sys.executable,'-m','esptool',
            '--chip','esp32s3','--port','/dev/cu.usbmodem21201','--before','default_reset','--after','hard_reset','chip_id'],capture_output=True,timeout=35)
        (out/'reboot.log').write_bytes(result.stdout+result.stderr)
        check('normal software reset succeeds',result.returncode==0)
        time.sleep(9)
        d=Device('/dev/cu.usbmodem21201',out)
        state=d.state()
        check('boot still defaults to clock',state['view']==1)
        check('history survives restart',state['notification_count']==baseline+3)
        open_center()
        check('interrupted work is not falsely shown complete','设备已重启，请进入应用确认上次任务' in labels() and '需处理' in labels())
        d.screenshot('restored')
    d.view('agents'); open_center(); d.tap(310,418)
    check('back preserves agent workbench',d.state()['view']==14 and not d.state()['notifications_open'])
    (out/'tested-state.json').write_text(json.dumps(d.state(),ensure_ascii=False,indent=2))
finally:
    try:
        phase(0); d.view('clock')
        (out/'handoff-state.json').write_text(json.dumps(d.state(),ensure_ascii=False,indent=2))
    finally:
        d.connection.close()
        (out/'results.json').write_text(json.dumps(checks,ensure_ascii=False,indent=2))

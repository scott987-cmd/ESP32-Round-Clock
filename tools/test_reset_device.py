"""Real screen/pointer test, with read-only network refresh (no synthetic feed)."""
import json
import time
from pathlib import Path
from device_ui_test import Device
from device_api_client import request

out = Path('artifacts/reset-review')
out.mkdir(parents=True,exist_ok=True)
api = json.loads(request('/v1/codex-reset'))
d = Device('/dev/cu.usbmodem21201', out)
checks = []

def check(name, condition):
    checks.append({'name':name, 'passed':bool(condition)})
    print(('PASS ' if condition else 'FAIL ') + name, flush=True)
    assert condition, name

try:
    d.view('reset')
    d.tap(171, 420)
    deadline = time.monotonic() + 35
    while time.monotonic() < deadline:
        state = d.state()
        texts = '\n'.join(x['text'] for x in state['labels'])
        if api['eventTime'] in texts and f"{api['probability24']}%" in texts:
            break
        time.sleep(1)
    check('new device reset page', state['view'] == 9 and '重置雷达' in texts)
    check('Beijing event timestamp', api['eventTime'] in texts and '北京时间' in texts)
    check('explicit historical age', '历史记录' in texts)
    check('both source forecast probabilities', f"{api['probability24']}%" in texts and f"{api['probability48']}%" in texts)
    check('confidence and unofficial status visible', '低可信' in texts and '非官方' in texts)
    check('no old reset happening-now claim', '重置已生效' not in texts and '重置预警' not in texts)
    check('all Chinese glyphs render', all(x.get('missing_glyphs',0)==0 for x in state['labels']))
    d.screenshot('after-top')
    (out/'state-top.json').write_text(json.dumps(state, ensure_ascii=False, indent=2))
    d.swipe(233,350,233,140)
    scrolled = d.state()
    check('vertical swipe remains in app', scrolled['view']==9)
    old = {x['text']:x['y'] for x in state['labels']}
    check('details actually move into view', any(x['y'] < old.get(x['text'],x['y'])-50 for x in scrolled['labels']))
    d.screenshot('after-details')
    for _ in range(2):
        d.swipe(233,350,233,140)
    d.screenshot('after-evidence')
    check('home control works after scrolling', (d.tap(233,420) is None) and d.state()['view']==0)
    # Old signal is deliberately different from the saved cursor. A fresh fetch
    # must still not wake the app or post a new notification for this old event.
    d.view('clock')
    initial_state = d.state()
    initial = initial_state['notification_count']
    d.command('RCALERT')
    time.sleep(15)
    final = d.state()
    check('old signal was actually re-fetched', final['reset_sync_count'] > initial_state['reset_sync_count'] and final['reset_cursor'] == api['signalId'])
    check('old active source does not force-open reset app', final['view']==1)
    check('old source does not create new notification', final['notification_count']==initial)
    (out/'state-after-old-signal.json').write_text(json.dumps(final, ensure_ascii=False, indent=2))
    d.view('reset')
finally:
    d.connection.close()
    (out/'device-results.json').write_text(json.dumps(checks, ensure_ascii=False, indent=2))

"""Device-only acceptance. Never changes this computer's network settings."""
import json
import time
from pathlib import Path
from device_ui_test import Device

out = Path('artifacts/quick-setup')
out.mkdir(parents=True, exist_ok=True)
d = Device('/dev/cu.usbmodem21201', out)
results = []


def check(name, condition):
    results.append({'name': name, 'passed': bool(condition)})
    print(('PASS ' if condition else 'FAIL ') + name, flush=True)
    assert condition, name


def wait_for(predicate, timeout=40):
    until = time.monotonic() + timeout
    while time.monotonic() < until:
        state = d.state()
        if predicate(state):
            return state
        time.sleep(.6)
    raise TimeoutError(state)


try:
    check('original network connected', wait_for(lambda s: s['wifi_connected'])['wifi_connected'])
    d.view('wifi')
    d.tap(233, 366)
    wait_for(lambda s: s['wifi_setup_active'])
    time.sleep(1.2)
    d.screenshot('setup')
    check('hotspot coexists with original Wi-Fi', d.state()['wifi_connected'])
    d.tap(233,420)
    wait_for(lambda s: not s['wifi_setup_active'])
    check('cancel keeps original Wi-Fi', d.state()['wifi_connected'])
    d.command('RCWIFIJOIN')
    wait_for(lambda s: s['wifi_setup_testing'])
    state=wait_for(lambda s: not s['wifi_setup_testing'] and s['wifi_connected'])
    check('successful rejoin persists a network',state['wifi_saved_networks']>=1)
    before=state['wifi_saved_networks']
    d.command('RCWIFIFAIL')
    wait_for(lambda s:s['wifi_setup_testing'])
    state=wait_for(lambda s:not s['wifi_setup_testing'] and s['wifi_connected'])
    check('missing network rolls back to working Wi-Fi','失败' in state['wifi_setup_status'])
    check('failed network was not saved',state['wifi_saved_networks']==before)
    check('display remains responsive',state['view']==4)
finally:
    d.view('desktop')
    d.connection.close()
    (out/'device-results.json').write_text(json.dumps(results,ensure_ascii=False,indent=2))

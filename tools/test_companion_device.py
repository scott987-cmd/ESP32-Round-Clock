"""Real device -> mTLS backend -> model -> persisted note -> real device UI.

Injects one fixed, explicitly labelled transcript over USB. This validates the
new application path, not acoustic recognition. Never touches Mac Wi-Fi.
"""
import json
import subprocess
import time
from pathlib import Path
from device_ui_test import Device
from device_api_client import request

out=Path('artifacts/companion')
out.mkdir(parents=True,exist_ok=True)
d=Device('/dev/cu.usbmodem21201',out)
checks=[]


def check(name,condition):
    checks.append({'name':name,'passed':bool(condition)})
    print(('PASS ' if condition else 'FAIL ')+name,flush=True)
    assert condition,name


def wait(predicate,timeout=80):
    until=time.monotonic()+timeout
    while time.monotonic()<until:
        s=d.state()
        if predicate(s): return s
        time.sleep(1)
    raise TimeoutError(s)


def notes():
    return json.loads(request('/v1/notes'))


try:
    d.view('agents')
    state=wait(lambda s:s['agent_rows']>0)
    check('real task boards reached device',state['agent_rows']>0 and '机器' in state['agent_status'])
    d.screenshot('agents-live')
    d.tap(233,205)
    check('agent detail contains real source timestamp',any('来源时间' in l['text'] for l in d.state()['labels']))
    check('agent detail Chinese and arrows have glyphs',all(not l.get('missing_glyphs') for l in d.state()['labels']))
    d.swipe(233,335,233,170)
    d.screenshot('agent-detail-live')
    d.tap(233,420)
    d.swipe(233,343,233,200)
    check('agent list scroll stays in workbench',d.state()['view']==14)
    d.view('notes')
    d.screenshot('notes-before')
    baseline=notes()
    before=baseline['total']
    existing_ids={n['id'] for n in baseline['notes']}
    # A real, very short microphone capture exercises the failure callback.
    d.pointer(233,142,True);d.pointer(233,142,False)
    d.pointer(299,304,True);d.pointer(299,304,False)
    state=wait(lambda s:not s['notes_recording'] and '重新录音' in s['note_capture_status'],15)
    check('short microphone capture exits waiting state',not state['notes_recording'])
    d.screenshot('notes-short-recording')
    d.tap(233,420)
    d.command('RCNOTETEST')
    d.view('clock')
    check('can switch apps while note uploads and summarizes',d.state()['view']==1)
    until=time.monotonic()+100
    fixture=None
    while time.monotonic()<until:
        current=notes()
        fixture=next((n for n in current['notes'] if n['id'] not in existing_ids and n['rawText'].startswith('圆屏验收测试：')),None)
        if fixture and fixture['state']=='ready': break
        time.sleep(2)
    check('server durably saved raw transcript',bool(fixture) and '不是真实待办' in fixture['rawText'])
    check('actual model completed background summary',fixture['state']=='ready' and bool(fixture['summary']))
    check('exactly one note was added',current['total']==before+1)
    (out/'test-note-id.json').write_text(json.dumps({'id':fixture['id'],'rawText':fixture['rawText']},ensure_ascii=False))
    d.view('notes')
    wait(lambda s:s['notes_rows']>=1)
    time.sleep(2)
    d.screenshot('notes-ready')
    d.tap(233,225)
    check('saved note opens with raw text and summary',any('识别原文' in l['text'] and fixture['summary'] in l['text'] for l in d.state()['labels']))
    check('note detail glyphs available',all(not l.get('missing_glyphs') for l in d.state()['labels']))
    d.screenshot('note-detail')
    d.swipe(233,344,233,165)
    d.screenshot('note-detail-scrolled')
    d.view('desktop')
    d.view('notes')
    check('reopening shows stored records',d.state()['notes_rows']>=1)
finally:
    # Failure must never leave the microphone recording in the background.
    try:
        if d.state()['notes_recording']:
            d.view('notes');d.tap(233,142);d.tap(299,304)
    except Exception:
        pass
    d.connection.close()
    (out/'device-results.json').write_text(json.dumps(checks,ensure_ascii=False,indent=2))

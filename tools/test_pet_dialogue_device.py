#!/usr/bin/env python3
"""Real board speech-output test; optional Mac-speaker -> board-MIC test.

RCPETSAY exercises from the ASR callback boundary, not the microphone. Only
--acoustic can claim actual microphone capture; it plays a fixed public phrase.
"""
import argparse
import json
import subprocess
import time
from pathlib import Path
from device_ui_test import Device

parser=argparse.ArgumentParser()
parser.add_argument('--port',default='/dev/cu.usbmodem21201')
parser.add_argument('--acoustic',action='store_true')
args=parser.parse_args()
output=Path('artifacts/pet-dialogue');output.mkdir(parents=True,exist_ok=True)
d=Device(args.port,output)
try:
    d.view('pet');before=d.state()
    if args.acoustic:
        d.tap(233,399)
        assert d.state()['pet_phase']==1, 'MIC did not enter listening state'
        subprocess.run(['say','-v','Tingting','-r','150','团团你好，我想摸摸你的头。'],check=True,timeout=15)
        time.sleep(0.8);d.tap(233,399)
    else:
        d.command('RCPETSAY 团团你好，给你点心。')
    d.view('clock')
    deadline=time.monotonic()+240
    while time.monotonic()<deadline:
        s=d.state()
        assert s['view']==1, 'Background reply interrupted another app'
        if s['pet_phase'] in (3,4):break
        time.sleep(2)
    assert s['pet_phase']==3, 'Reply failed: '+s['pet_reply']
    assert not s['english_playing'], 'Reply spoke while outside pet app'
    d.view('pet');d.screenshot('reply-acoustic' if args.acoustic else 'reply')
    deadline=time.monotonic()+20
    while time.monotonic()<deadline:
        s=d.state()
        if not s['english_playing'] and s['english_audio_written']==s['pet_audio_bytes']:break
        time.sleep(0.5)
    assert s['english_audio_result']==0 and s['english_audio_written']==s['pet_audio_bytes']>3200, 'Speaker write failed'
    assert all(x.get('missing_glyphs',0)==0 for x in s['labels']), 'Missing Chinese glyphs'
    summary={k:s[k] for k in ('pet_phase','pet_reply','pet_audio_bytes','english_audio_written','pet_meals','pet_pats','free_internal','free_psram')}
    summary['microphone_path']=args.acoustic
    (output/('acoustic.json' if args.acoustic else 'callback.json')).write_text(json.dumps(summary,ensure_ascii=False,indent=2))
    print(json.dumps(summary,ensure_ascii=False),flush=True)
    sequence=s['english_audio_sequence']
    d.tap(233,289)
    deadline=time.monotonic()+20
    while time.monotonic()<deadline:
        replay=d.state()
        if replay['english_audio_sequence']>sequence and not replay['english_playing']:break
        time.sleep(.4)
    assert replay['english_audio_sequence']>sequence and replay['english_audio_written']==s['pet_audio_bytes'], 'Tap reply did not replay audio'
    # This short recording must fail gracefully, not stay stuck as "listening".
    d.pointer(233,399,True);d.pointer(233,399,False)
    d.pointer(233,399,True);d.pointer(233,399,False)
    deadline=time.monotonic()+8
    while time.monotonic()<deadline and d.state()['pet_phase'] in (1,2):time.sleep(0.2)
    assert d.state()['pet_phase']==4, 'Short capture did not leave recording state'
    d.view('clock')
finally:
    d.connection.close()

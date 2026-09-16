"""Exercise real LVGL touch controls, framebuffer contents and codec completion."""
import json
import time
from pathlib import Path
from PIL import Image
from build_english_assets import rgb565
from device_ui_test import Device

ROOT=Path(__file__).resolve().parents[1]
OUT=ROOT/'artifacts/english'
ASSETS=ROOT/'main/assets/english'

def main():
    words=json.loads((ASSETS/'manifest.json').read_text())['words']
    checks=[]
    d=Device('/dev/cu.usbmodem21201',OUT)
    def check(name,ok):
        checks.append({'name':name,'passed':bool(ok)})
        print(('PASS ' if ok else 'FAIL ')+name,flush=True)
        assert ok,name
    def texts(): return [x['text'] for x in d.state()['labels']]
    def wait_read(index,sequence):
        deadline=time.monotonic()+5
        while time.monotonic()<deadline:
            state=d.state()
            if state['english_audio_sequence']>sequence and not state['english_playing']:break
            time.sleep(.15)
        check('complete PCM '+words[index]['word'],state['english_audio_sequence']==sequence+1 and
              not state['english_playing'] and state['english_audio_result']==0 and
              state['english_audio_written']==words[index]['audioBytes'])
        time.sleep(.15)
    def read(index,picture=False):
        sequence=d.state()['english_audio_sequence']
        d.tap(233,190 if picture else 422)
        wait_read(index,sequence)
    try:
        initial=d.state()
        check('boot remains clock',initial['view']==1)
        initial_notifications=initial['notification_count']
        d.view('desktop')
        for _ in range(18):
            if d.state()['selected']==13:break
            d.swipe(330,230,145,230)
        else: raise AssertionError('English icon not reachable within one carousel lap')
        d.screenshot('launcher-english');d.tap()
        check('desktop icon opens English',d.state()['view']==17)
        check('asset pack valid',d.state()['english_assets_valid'])
        for i,entry in enumerate(words):
            state=d.state()
            check('labels '+entry['word'],state['english_card']==i and entry['word'] in texts() and entry['chinese'] in texts())
            check('glyphs '+entry['word'],all(x['missing_glyphs']==0 for x in state['labels']))
            d.screenshot('card-'+entry['word'])
            actual=Image.open(OUT/('card-'+entry['word']+'.png')).convert('RGB').crop((145,99,321,275))
            expected=Image.open(ASSETS/'cards'/(entry['word']+'.png')).convert('RGB')
            check('actual framebuffer picture '+entry['word'],rgb565(actual)==rgb565(expected))
            read(i,picture=i==0)
            d.tap(392,218)
        check('next wraps',d.state()['english_card']==0)
        d.tap(74,218);check('previous wraps',d.state()['english_card']==11)
        d.swipe(330,220,140,220);check('left swipe next',d.state()['english_card']==0)
        d.swipe(140,220,330,220);check('right swipe previous',d.state()['english_card']==11)
        d.tap(157,422);check('home returns desktop',d.state()['view']==0)
        d.tap();check('reopen keeps card',d.state()['view']==17 and d.state()['english_card']==11)
        sequence=d.state()['english_audio_sequence']
        d.tap(309,422);check('practice enters',d.state()['english_quiz'])
        for question in range(5):
            state=d.state();target=state['english_target']
            choices=[state['english_choice0'],state['english_choice1']]
            check(f'question {question}: distinct pictures',choices[0]!=choices[1] and target in choices)
            side=choices.index(target);right=147+172*side;wrong=147+172*(1-side)
            wait_read(target,sequence)
            check('automatic reading unlocks choices',d.state()['english_heard'] and not d.state()['english_answered'])
            if question==0:read(target)
            if question==0:d.screenshot('quiz-listening')
            d.tap(wrong,225);check('gentle retry without penalty',d.state()['english_correct']==question and not d.state()['english_answered'] and '再听一遍，慢慢找' in texts())
            d.tap(right,225);check('correct advances once',d.state()['english_correct']==question+1 and d.state()['english_answered'])
            d.tap(right,225);check('double tap cannot duplicate score',d.state()['english_correct']==question+1)
            d.screenshot(f'quiz-{question+1}')
            if question<4:
                sequence=d.state()['english_audio_sequence'];d.tap(233,371)
        check('five words gives rest prompt','你完成啦！' in texts() and '真棒！休息一下眼睛吧' in texts())
        sequence=d.state()['english_audio_sequence'];d.tap(233,371)
        check('new round resets score',d.state()['english_correct']==0)
        wait_read(d.state()['english_target'],sequence)
        d.tap(309,422);check('learning restores last card',not d.state()['english_quiz'] and d.state()['english_card']==11)
        # Cancel immediately through the real home control, then read again.
        d.pointer(233,422,True);d.pointer(233,422,False)
        d.pointer(157,422,True);d.pointer(157,422,False);time.sleep(.5)
        check('leaving stops playback',d.state()['view']==0 and not d.state()['english_playing'])
        d.tap();read(11)
        d.swipe(330,220,140,220)
        d.screenshot('ready')
        check('memory headroom restored',d.state()['free_psram']>900000)
        check('English practice adds no unrelated notifications',d.state()['notification_count']==initial_notifications)
    finally:
        try: final=d.state()
        except Exception as error: final={'error':str(error)}
        d.connection.close()
        (OUT/'device-results.json').write_text(json.dumps({'checks':checks,'final':final},ensure_ascii=False,indent=2))

if __name__=='__main__':main()

"""Real LVGL pointer actions and codec output, no model calls."""
import json,time
from pathlib import Path
from device_ui_test import Device

OUT=Path('artifacts/expansion/kids');OUT.mkdir(parents=True,exist_ok=True)
d=Device('/dev/cu.usbmodem21201',OUT);checks=[]
def check(name,value):
    checks.append(dict(name=name,passed=bool(value)));print(('PASS ' if value else 'FAIL ')+name,flush=True);assert value,name
def glyphs():return all(v['missing_glyphs']==0 for v in d.state()['labels'])
def tapcard(i):d.tap(172+(i%2)*122,141+(i//2)*99)
try:
    d.view('pet');s=d.state();check('pet opens with complete Chinese',s['kids_active']==0 and glyphs());d.screenshot('pet')
    pats=s['pet_pats'];d.tap(233,210);check('touch head reacts',d.state()['pet_pats']==pats+1)
    meals=d.state()['pet_meals'];d.tap(160,378);check('feed reacts',d.state()['pet_meals']==meals+1)
    d.tap(305,378);check('sleep toggles',d.state()['pet_asleep']);d.screenshot('pet-sleep')
    d.tap(233,430);d.view('pet');check('pet progress survives scene destruction',d.state()['pet_asleep'] and d.state()['pet_meals']==meals+1)
    d.tap(233,210);check('touch wakes pet',not d.state()['pet_asleep'])
    d.view('flip');d.tap(295,430);s=d.state();deck=s['flip_deck'];check('six cards with three pairs',all(deck.count(i)==2 for i in range(3)) and glyphs())
    d.screenshot('flip-start');wrong=next(i for i in range(1,6) if deck[i]!=deck[0]);tapcard(0);tapcard(wrong)
    check('wrong pair initially revealed',d.state()['flip_second']==wrong);time.sleep(1.1);check('wrong pair hides without penalty',d.state()['flip_first']==-1 and d.state()['flip_matched']==0)
    pair=[i for i,v in enumerate(deck) if v==0]
    for i in pair:tapcard(i)
    mask=sum(1<<i for i in pair);check('correct pair stays revealed',d.state()['flip_matched']==mask)
    d.tap(172,430);d.view('flip');check('matched pairs survive exit',d.state()['flip_matched']==mask)
    for value in (1,2):
        for i,v in enumerate(deck):
            if v==value:tapcard(i)
    check('all pairs win once',d.state()['flip_matched']==63 and any('真棒' in x['text'] for x in d.state()['labels']));d.screenshot('flip-win')
    d.tap(295,430);check('new round resets board',d.state()['flip_matched']==0)
    # Story app opens its bookshelf first so AI generation is discoverable.
    d.view('story');d.tap(230,126);s=d.state()
    check('built-in story starts offline',s['view']==20 and not s['story_online'] and s['story_scene']==0 and glyphs());d.screenshot('story-first')
    deadline=time.monotonic()+20
    while time.monotonic()<deadline:
        s=d.state()
        if not s['english_playing'] and s['english_audio_written']>200000:break
        time.sleep(.3)
    check('Chinese narration writes full PCM through codec',s['english_audio_result']==0 and s['english_audio_written']==286498)
    d.tap(140,368);check('left branch enters bird scene',d.state()['story_scene']==1);d.screenshot('story-branch')
    d.tap(320,368);check('second branch reaches ending',d.state()['story_scene']==4)
    d.tap(153,425);time.sleep(.4);check('home stops narration',d.state()['view']==0 and not d.state()['english_playing'])
    d.view('story');check('story resumes saved scene',d.state()['story_scene']==4);d.tap(320,368);check('restart returns first scene',d.state()['story_scene']==0)
    d.view('desktop');free=d.state()['free_psram'];internal=d.state()['free_internal']
    for i in range(5):
        for app in ('pet','flip','story','english'):d.view(app)
        d.view('desktop')
    s=d.state();check('repeated apps release large buffers',s['free_psram']>=free-14000);check('internal memory headroom above 60KB',s['free_internal']>60000)
    (OUT/'state.json').write_text(json.dumps(s,ensure_ascii=False,indent=2))
finally:
    d.connection.close();(OUT/'results.json').write_text(json.dumps(checks,ensure_ascii=False,indent=2))

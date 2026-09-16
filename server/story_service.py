"""Bounded children's stories: idempotent jobs, immutable archive, no client keys."""
import hashlib,json,os,re,threading,time,urllib.parse,urllib.request
from datetime import datetime,timezone
import artwork_library as library

LOCK=threading.Lock()
THEMES={'friends':'分享与友谊','nature':'观察大自然','courage':'尝试新事物'}
ACTIVE=set()
PICTURE_WORDS=(('猫',),('狗',),('鸟',),('鱼',),('苹果',),('香蕉',),('橙','橘'),('草莓',),('球',),('汽车','小车'),('书','绘本','笔记'),('杯',))

def root():return library.ROOT.parent/'stories'
def job_path(ident):return root()/('job-'+library.validate_id(ident)+'.json')
def job(ident):return json.loads(job_path(ident).read_text())
def put_job(ident,value):library.atomic(job_path(ident),json.dumps(value,ensure_ascii=False).encode())

def recover():
    # An interrupted paid request is never automatically issued a second time.
    root().mkdir(parents=True,exist_ok=True)
    for path in root().glob('job-*.json'):
        value=json.loads(path.read_text())
        if value.get('status')=='working':
            value.update(status='failed',message='服务重启，本次未完成；可重新创建')
            library.atomic(path,json.dumps(value,ensure_ascii=False).encode())

def api(path,body,limit):
    key=os.environ.get('MINIMAX_API_KEY','').strip()
    if not key:raise RuntimeError('model not configured')
    url=os.environ.get('MINIMAX_BASE_URL','https://api.minimax.io').rstrip('/')+path
    if urllib.parse.urlparse(url).scheme != 'https':raise ValueError('only HTTPS model endpoints are allowed')
    req=urllib.request.Request(url,data=json.dumps(body,ensure_ascii=False).encode(),headers={'Authorization':'Bearer '+key,'Content-Type':'application/json'})
    with urllib.request.urlopen(req,timeout=150) as response:data=response.read(limit+1)  # nosec B310
    if len(data)>limit:raise ValueError('model response too large')
    value=json.loads(data)
    if value.get('base_resp',{}).get('status_code',0)!=0:raise RuntimeError('provider error code '+str(value['base_resp']['status_code']))
    return value

def validate(value):
    if not isinstance(value,dict) or not isinstance(value.get('nodes'),list) or len(value['nodes'])!=5:raise ValueError('five scenes required')
    def text(v,maximum):
        if not isinstance(v,str) or not 1<=len(v.strip())<=maximum or re.search(r'[\x00-\x1f<>]',v):raise ValueError('invalid story text length/format, limit '+str(maximum))
        return v.strip()
    out={'title':text(value.get('title'),12),'nodes':[]}
    for i,node in enumerate(value['nodes']):
        if not isinstance(node,dict):raise ValueError('invalid scene')
        picture=node.get('picture')
        if type(picture) is not int or not 0<=picture<12:raise ValueError('invalid picture')
        narrative=text(node.get('text'),55)
        if not any(word in narrative for word in PICTURE_WORDS[picture]):raise ValueError('illustration must match a named subject in the scene')
        combined=narrative+str(node.get('left',''))+str(node.get('right',''))
        if any(word in combined for word in ('伸进水里','下水','投喂','喂鱼','面包屑','摘花','独自出门')):raise ValueError('scene needs adult editorial review')
        out['nodes'].append(dict(title=text(node.get('title'),12),text=narrative,picture=picture,
            left=text(node.get('left'),6) if i<3 else '再听一遍',right=text(node.get('right'),6) if i<3 else '重新开始',
            a=1 if i==0 else 3 if i<3 else -1,b=2 if i==0 else 4 if i<3 else 0))
    return out

def decode_story(value):
    choice=value['choices'][0]
    if choice.get('finish_reason')=='length':raise ValueError('incomplete story response')
    text=choice['message']['content']
    text=re.sub(r'^\s*<think>.*?</think>\s*','',text,flags=re.S)
    text=re.sub(r'^```(?:json)?\s*|\s*```$','',text.strip())
    return validate(json.loads(text))

def generate(theme):
    prompt='为4到8岁孩子写温暖、无恐吓、无危险模仿内容的中文互动故事，主题：'+THEMES[theme]+'''。
只返回JSON：{"title":"12字内","nodes":[{"title":"12字内","text":"每幕25到45字，最多55字","picture":0,"left":"6字内选择","right":"6字内选择"}]}。
必须正好5幕：第0幕选择进入第1或2幕，第1和2幕分别选择进入第3或4幕，第3和4幕是两个温暖结局。
picture必须与该幕主角或物品对应：0猫、1狗、2鸟、3鱼、4苹果、5香蕉、6橙子、7草莓、8球、9汽车、10书、11杯子。
每幕正文必须明确提及 picture 对应的角色或物品，不能用书本图片代表森林等场景。
全部使用小猫、小狗、小鸟等拟人角色的第三人称温暖童话，不对孩子发出现实行动指令。自然主题只做安全观察，不下水、不把手伸进水里、不投喂野生动物、不摘花，不写明显错误科普。
围绕这些现成插图叙事，不要添加网络链接，不要索取孩子个人信息。'''
    value=api('/v1/chat/completions',{'model':os.environ.get('MINIMAX_RESET_MODEL','MiniMax-M2.7-highspeed'),'reasoning_split':True,'max_completion_tokens':10000,'messages':[{'role':'user','content':prompt}]},128000)
    print('story text finish='+str(value.get('choices',[{}])[0].get('finish_reason'))+' usage='+str(value.get('usage',{}).get('completion_tokens')),flush=True)
    story=decode_story(value);total=0
    for node in story['nodes']:
        value=api('/v1/t2a_v2',{'model':'speech-2.8-turbo','text':node['text'],'stream':False,'language_boost':'Chinese','output_format':'hex',
            'voice_setting':{'voice_id':'Chinese (Mandarin)_Warm_Girl','speed':0.95,'vol':0.7,'pitch':0},
            'audio_setting':{'sample_rate':16000,'format':'pcm','channel':1}},1100000)
        info=value.get('extra_info',{})
        if info.get('audio_sample_rate')!=16000 or info.get('audio_channel')!=1 or info.get('audio_format')!='pcm':raise ValueError('unexpected audio format: '+str({k:info.get(k) for k in ('audio_sample_rate','audio_channel','audio_format')}))
        pcm=bytes.fromhex(value['data']['audio']);total+=len(pcm)
        if not 3200<=len(pcm)<=512000 or len(pcm)%2 or total>1900000:raise ValueError('audio exceeds device limit')
        digest=hashlib.sha256(pcm).hexdigest();library.atomic(root()/(digest+'.pcm'),pcm)
        node.update(audio=digest,bytes=len(pcm))
    payload=json.dumps(story,ensure_ascii=False,separators=(',',':')).encode()
    return library.store('story',payload,{'title':story['title'],'generatedAt':datetime.now(timezone.utc).isoformat()})

def worker(ident,theme):
    try:
        artwork=generate(theme);put_job(ident,dict(status='done',id=artwork,message='新故事已保存到作品相册'))
    except Exception as exc:
        # Do not expose provider response bodies, credentials, or child data.
        detail=str(exc) if type(exc) in (ValueError,RuntimeError) else ''
        print('story generation failed: '+type(exc).__name__+' '+detail[:200],flush=True)
        put_job(ident,dict(status='failed',message='本次生成未完成，请稍后重新创建'))
    finally:
        with LOCK:ACTIVE.discard(ident)

def start(ident,theme):
    library.validate_id(ident)
    if theme not in THEMES:raise ValueError('invalid theme')
    with LOCK:
        if job_path(ident).exists():return job(ident)
        if ACTIVE:raise BlockingIOError('another story is generating')
        recent=sum(p.stat().st_mtime>time.time()-86400 for p in root().glob('job-*.json')) if root().exists() else 0
        if recent>=8:raise BlockingIOError('daily story limit reached')
        value=dict(status='working',message='正在编写和配音，可先玩其他应用')
        put_job(ident,value);ACTIVE.add(ident)
        threading.Thread(target=worker,args=(ident,theme),daemon=True).start()
        return value

def read(ident,scene=None):
    record=library.get(ident)
    if record['kind']!='story':raise ValueError('not a story')
    payload=library.read(ident)
    if scene is None:return payload,'application/json; charset=utf-8'
    if type(scene) is not int or not 0<=scene<5:raise ValueError('invalid scene')
    node=json.loads(payload)['nodes'][scene];digest=node['audio']
    if not re.fullmatch('[a-f0-9]{64}',digest):raise ValueError('invalid stored audio')
    return (root()/(digest+'.pcm')).read_bytes(),'application/octet-stream'

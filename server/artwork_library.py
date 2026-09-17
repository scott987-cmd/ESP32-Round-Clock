"""Immutable artwork bytes + transactional, recoverable catalogue mutations."""
from __future__ import annotations
import hashlib
import io
import json
import os
import re
import sqlite3
import tempfile
from datetime import datetime, timezone, timedelta
from pathlib import Path
from contextlib import contextmanager
from PIL import Image

ROOT=Path(os.environ.get('WALLPAPER_STATE_DIR','/var/lib/esp32-wallpaper'))/'library'
KINDS={'wallpaper','music','story'}
ID=re.compile(r'^[a-f0-9]{32}$')

def atomic(path, data):
    path.parent.mkdir(parents=True,exist_ok=True)
    fd,name=tempfile.mkstemp(prefix='.pending-',dir=path.parent)
    try:
        with os.fdopen(fd,'wb') as f:f.write(data);f.flush();os.fsync(f.fileno())
        os.replace(name,path)
    finally:
        if os.path.exists(name):os.unlink(name)

@contextmanager
def connect():
    ROOT.mkdir(parents=True,exist_ok=True)
    db=sqlite3.connect(ROOT/'catalogue.sqlite3',timeout=20)
    db.row_factory=sqlite3.Row
    db.execute('CREATE TABLE IF NOT EXISTS works(id TEXT PRIMARY KEY,kind TEXT NOT NULL,title TEXT NOT NULL,created TEXT NOT NULL,meta TEXT NOT NULL,favorite INTEGER NOT NULL DEFAULT 0,deleted INTEGER NOT NULL DEFAULT 0)')
    db.execute('CREATE TABLE IF NOT EXISTS selections(kind TEXT PRIMARY KEY,id TEXT NOT NULL)')
    db.commit()
    try:
        with db:yield db
    finally:db.close()

def validate_id(value):
    if not isinstance(value,str) or not ID.fullmatch(value):raise ValueError('invalid artwork id')
    return value

def store(kind, payload, metadata, png=None):
    if kind not in KINDS:raise ValueError('invalid kind')
    if kind=='wallpaper' and len(payload)!=466*466*2:raise ValueError('invalid wallpaper')
    if kind=='music' and (not payload or len(payload)%2 or len(payload)>12*1024*1024):raise ValueError('invalid music')
    if kind=='story' and (not payload or len(payload)>65536):raise ValueError('invalid story')
    ident=hashlib.sha256(kind.encode()+b'\0'+payload).hexdigest()[:32]
    title=str(metadata.get('title') or metadata.get('prompt') or {'wallpaper':'我的壁纸','music':'我的音乐','story':'我的故事'}[kind]).strip()[:80]
    created=str(metadata.get('generatedAt') or metadata.get('generated_at') or datetime.now(timezone.utc).isoformat())
    thumb=None
    if png is not None:
        image=Image.open(io.BytesIO(png)).convert('RGB').resize((128,128),Image.Resampling.LANCZOS)
        pixels=image.get_flattened_data() if hasattr(image,'get_flattened_data') else image.getdata()
        thumb=b''.join((((r>>3)<<11)|((g>>2)<<5)|(b>>3)).to_bytes(2,'little') for r,g,b in pixels)
    # Files become visible in the catalogue only after every required file is durable.
    atomic(ROOT/f'{ident}.data',payload)
    if thumb is not None:atomic(ROOT/f'{ident}.thumb',thumb)
    if png is not None:atomic(ROOT/f'{ident}.png',png)
    with connect() as db:
        db.execute('INSERT OR IGNORE INTO works(id,kind,title,created,meta) VALUES(?,?,?,?,?)',
                   (ident,kind,title,created,json.dumps(metadata,ensure_ascii=False)))
    return ident

def get(ident, include_deleted=False):
    validate_id(ident)
    with connect() as db:r=db.execute('SELECT * FROM works WHERE id=?',(ident,)).fetchone()
    if r is None or (r['deleted'] and not include_deleted):raise FileNotFoundError('artwork unavailable')
    return dict(r)

def read(ident,part='data'):
    get(ident)
    if part not in {'data','thumb','png'}:raise ValueError('invalid part')
    return (ROOT/f'{ident}.{part}').read_bytes()

def public(row, selected):
    try:stamp=datetime.fromisoformat(row['created'].replace('Z','+00:00')).astimezone(timezone(timedelta(hours=8))).strftime('%m-%d %H:%M')
    except (ValueError,TypeError):stamp='时间未知'
    return dict(id=row['id'],kind=row['kind'],title=row['title'],createdAt=row['created'],dateLabel=stamp,
                favorite=bool(row['favorite']),deleted=bool(row['deleted']),selected=row['id'] in selected,
                thumbnail=(ROOT/f"{row['id']}.thumb").is_file())

def list_works(offset=0,trash=False,kind=''):
    if type(offset) is not int or not 0<=offset<=100000:raise ValueError('invalid page')
    if not isinstance(kind,str) or (kind != '' and kind not in KINDS):raise ValueError('invalid kind')
    with connect() as db:
        selected={r[0] for r in db.execute('SELECT id FROM selections')}
        rows=db.execute('SELECT * FROM works WHERE deleted=? AND (?=\'\' OR kind=?) ORDER BY favorite DESC,created DESC,id LIMIT 7 OFFSET ?',(int(trash),kind,kind,offset)).fetchall()
    return dict(items=[public(r,selected) for r in rows[:6]],offset=offset,more=len(rows)>6,trash=trash)

def mutate(ident,action,value=None):
    get(ident,include_deleted=action=='restore')
    with connect() as db:
        if action=='favorite':
            if type(value) is not bool:raise ValueError('favorite must be boolean')
            db.execute('UPDATE works SET favorite=? WHERE id=?',(int(value),ident))
        elif action=='trash':
            if db.execute('SELECT 1 FROM selections WHERE id=?',(ident,)).fetchone():raise ValueError('currently applied wallpaper cannot be deleted')
            db.execute('UPDATE works SET deleted=1 WHERE id=?',(ident,))
        elif action=='restore':db.execute('UPDATE works SET deleted=0 WHERE id=?',(ident,))
        elif action=='select':
            row=get(ident)
            if row['kind']!='wallpaper':raise ValueError('only wallpapers can be applied')
            if len(read(ident))!=466*466*2:raise ValueError('invalid stored wallpaper')
            db.execute('INSERT OR REPLACE INTO selections(kind,id) VALUES(?,?)',('wallpaper',ident))
        else:raise ValueError('invalid action')
    return {'ok':True,'id':ident}

def selected_wallpaper():
    with connect() as db:row=db.execute('SELECT id FROM selections WHERE kind=?',('wallpaper',)).fetchone()
    if row is None:return None
    record=get(row[0]);meta=json.loads(record['meta'])
    return read(row[0]),meta

def migrate_current(state):
    state=Path(state)
    for kind,asset,metadata in [('wallpaper','current.rgb565','metadata.json'),('music','latest-music.pcm','latest-music.json')]:
        if (state/asset).is_file() and (state/metadata).is_file():
            png=(state/'current.png').read_bytes() if kind=='wallpaper' and (state/'current.png').is_file() else None
            store(kind,(state/asset).read_bytes(),json.loads((state/metadata).read_text()),png)

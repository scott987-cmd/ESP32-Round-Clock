"""Durable, single-flight music jobs. Reconnects never resubmit a paid request."""
import hashlib
import json
import re
import sqlite3
import threading
import time

import artwork_library as library

LOCK = threading.Lock()
ACTIVE = set()
STATIONS = ('', 'sky', 'aurora', 'energy')


def root():
    return library.ROOT.parent / 'music-jobs'


def path(ident):
    return root() / (library.validate_id(ident) + '.json')


def save(ident, value):
    library.atomic(path(ident), json.dumps(value, ensure_ascii=False).encode())


def job(ident):
    value = json.loads(path(ident).read_text())
    return {k: v for k, v in value.items() if k != 'requestHash'}


def catalogue():
    """The durable catalogue, not a volatile UI flag, decides what is playable."""
    latest, stations = None, {}
    with library.connect() as db:
        rows = db.execute("SELECT id,created,meta FROM works WHERE kind='music' AND deleted=0 ORDER BY created DESC,id")
        for row in rows:
            asset = library.ROOT / (row['id'] + '.data')
            if not asset.is_file() or not 0 < asset.stat().st_size <= 12 * 1024 * 1024 or asset.stat().st_size % 2:
                continue
            meta = json.loads(row['meta'])
            item = dict(artworkId=row['id'], generatedAt=row['created'],
                        durationMs=asset.stat().st_size * 1000 // 32000)
            if latest is None:
                latest = item
            station = meta.get('station')
            if station in STATIONS[1:] and station not in stations:
                stations[station] = item
    return latest, stations


def snapshot():
    with LOCK:
        files = sorted(root().glob('*.json'), key=lambda p: p.stat().st_mtime_ns, reverse=True)
        current = job(files[0].stem) if files else None
        latest, stations = catalogue()
        return dict(latest=latest, stations=stations, job=current)


def recover():
    """A result committed before a crash wins; otherwise fail, never charge again."""
    root().mkdir(parents=True, exist_ok=True)
    with library.connect() as db:
        completed = {}
        for row in db.execute("SELECT id,meta FROM works WHERE kind='music' AND deleted=0"):
            ident = json.loads(row['meta']).get('musicRequestId')
            if ident and (library.ROOT / (row['id'] + '.data')).is_file():
                completed[ident] = row['id']
    for p in root().glob('*.json'):
        value = json.loads(p.read_text())
        if value.get('status') == 'working' and p.stem not in ACTIVE:
            if p.stem in completed:
                value.update(status='done', artworkId=completed[p.stem])
            else:
                value.update(status='failed', message='服务重启，生成中断；已保存作品不受影响')
            save(p.stem, value)


def committed(ident):
    with library.connect() as db:
        for row in db.execute("SELECT id,meta FROM works WHERE kind='music' AND deleted=0"):
            if json.loads(row['meta']).get('musicRequestId') == ident:
                payload = library.read(row['id'])
                if 0 < len(payload) <= 12 * 1024 * 1024 and not len(payload) % 2:
                    return row['id']
    return None


def worker(ident, prompt, station, value, generate):
    try:
        meta = generate(prompt, station=station, request_id=ident)
        # Completion is valid only when the normal playback consumer can read it.
        library.read(meta['artworkId'])
        value.update(status='done', artworkId=meta['artworkId'])
    except Exception as exc:
        print('music job failed: ' + type(exc).__name__, flush=True)
        # The immutable catalogue is authoritative even if the legacy "latest"
        # cache update failed after the artwork was committed.
        try:
            saved = committed(ident)
        except (OSError, ValueError, sqlite3.Error):
            saved = None
        if saved:
            value.update(status='done', artworkId=saved)
        else:
            value.update(status='failed', message='音乐生成失败，请稍后重试；已保存作品仍可播放')
    finally:
        with LOCK:
            try:
                save(ident, value)
            finally:
                ACTIVE.discard(ident)


def start(ident, prompt, station, generate):
    library.validate_id(ident)
    if (station not in STATIONS or not isinstance(prompt, str) or not prompt.strip() or
            len(prompt.encode()) > 2000 or re.search(r'[\x00-\x1f]', prompt)):
        raise ValueError('invalid music request')
    digest = hashlib.sha256(json.dumps([prompt.strip(), station], ensure_ascii=False).encode()).hexdigest()
    with LOCK:
        if path(ident).is_file():
            value = json.loads(path(ident).read_text())
            if value.get('requestHash') != digest:
                raise ValueError('request id conflict')
            return job(ident)
        if ACTIVE:
            raise BlockingIOError('music generation busy')
        recover()
        files = list(root().glob('*.json'))
        if len(files) >= 1000 or sum(p.stat().st_mtime > time.time() - 86400 for p in files) >= 20:
            raise BlockingIOError('music generation limit reached')
        value = dict(requestId=ident, requestHash=digest, station=station, status='working')
        save(ident, value)
        ACTIVE.add(ident)
        try:
            threading.Thread(target=worker, args=(ident, prompt.strip(), station, value, generate), daemon=True).start()
        except RuntimeError:
            ACTIVE.discard(ident)
            value.update(status='failed', message='服务正忙，请稍后重试')
            save(ident, value)
            raise
        return job(ident)

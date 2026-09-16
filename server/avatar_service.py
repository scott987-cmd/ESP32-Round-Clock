"""Private, bounded avatar assets. No image/model provider receives these bytes."""
import hashlib
import os
from pathlib import Path
import struct
import tempfile
import threading
import zlib

SIZE = 64 + 280 * 280 * 2
ROOT = Path(os.environ.get('WALLPAPER_STATE_DIR', '/var/lib/esp32-wallpaper')) / 'avatars'
LOCK = threading.RLock()


def slot_number(value):
    if str(value) not in tuple(str(i) for i in range(8)):
        raise ValueError('invalid slot')
    return int(value)


def validate(payload):
    if len(payload) != SIZE or payload[:4] != b'RAV2':
        raise ValueError('invalid avatar package')
    if struct.unpack_from('<HH', payload, 4) != (280, 280) or any(payload[32:60]):
        raise ValueError('invalid header')
    for i in range(3):
        x, y, w, h = struct.unpack_from('<4H', payload, 8 + i * 8)
        if not (4 <= w <= 120 and 2 <= h <= 80 and x >= 8 and y >= 8 and x+w <= 272 and y+h <= 272):
            raise ValueError('invalid landmarks')
    if zlib.crc32(payload[64:]) != struct.unpack_from('<I', payload, 60)[0]:
        raise ValueError('checksum mismatch')


def read(slot):
    slot = slot_number(slot)
    with LOCK:
        data = (ROOT / f'{slot}.rav').read_bytes()
        validate(data)
        return data


def manifest():
    with LOCK:
        items = []
        for slot in range(8):
            try:
                data = read(slot)
            except FileNotFoundError:
                continue
            items.append({'slot': slot, 'sha256': hashlib.sha256(data).hexdigest(), 'bytes': len(data)})
        return {'avatars': items, 'capacity': 8}


def store(slot, data):
    slot = slot_number(slot)
    validate(data)
    with LOCK:
        ROOT.mkdir(mode=0o700, parents=True, exist_ok=True)
        target = ROOT / f'{slot}.rav'
        # Keep one recoverable previous version per explicit slot replacement.
        if target.exists() and target.read_bytes() == data:
            return {'stored': True, 'slot': slot, 'sha256': hashlib.sha256(data).hexdigest()}
        fd, name = tempfile.mkstemp(dir=ROOT, prefix='.upload-')
        try:
            with os.fdopen(fd, 'wb') as stream:
                stream.write(data)
                stream.flush()
                os.fsync(stream.fileno())
            if target.exists():
                backup = ROOT / f'{slot}.previous.rav'
                # Copy before replacement; a crash cannot remove the current slot.
                backup.write_bytes(target.read_bytes())
                backup.chmod(0o600)
            os.replace(name, target)
            directory = os.open(ROOT, os.O_RDONLY)
            try:
                os.fsync(directory)
            finally:
                os.close(directory)
        finally:
            if os.path.exists(name):
                os.unlink(name)
    return {'stored': True, 'slot': slot, 'sha256': hashlib.sha256(data).hexdigest()}

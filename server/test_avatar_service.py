import hashlib
import struct
import tempfile
import unittest
import zlib
from pathlib import Path
from unittest.mock import patch
import avatar_service as service

def package(color=4):
    header = bytearray(64)
    struct.pack_into('<4sHH12H',header,0,b'RAV2',280,280,80,100,30,12,170,100,30,12,110,175,55,24)
    pixels = bytes([color,20])*(280*280)
    struct.pack_into('<I',header,60,zlib.crc32(pixels))
    return bytes(header)+pixels

class AvatarTests(unittest.TestCase):
    def setUp(self):
        self.folder=tempfile.TemporaryDirectory()
        self.root=Path(self.folder.name)/'avatars'
        self.replace=patch.object(service,'ROOT',self.root); self.replace.start()
    def tearDown(self):
        self.replace.stop(); self.folder.cleanup()
    def test_round_trip_and_hash(self):
        data=package(); result=service.store(2,data)
        self.assertEqual(service.read(2),data)
        self.assertEqual(result['sha256'],hashlib.sha256(data).hexdigest())
        self.assertEqual(service.manifest()['avatars'][0]['slot'],2)
    def test_idempotent_retry_and_recoverable_replace(self):
        service.store(0,package(1)); service.store(0,package(2)); service.store(0,package(2))
        self.assertEqual((self.root/'0.previous.rav').read_bytes(),package(1))
        self.assertEqual(service.read(0),package(2))
    def test_invalid_input_preserves_previous(self):
        service.store(0,package())
        for bad in [b'',package()[:-1],package()+b'0',b'FAIL'+package()[4:],package()[:-1]+b'\xff']:
            with self.assertRaises(ValueError): service.store(0,bad)
            self.assertEqual(service.read(0),package())
    def test_rejects_path_traversal_and_overflow(self):
        for slot in ['../a',8,-1,'00','2.0',None]:
            with self.assertRaises(ValueError): service.store(slot,package())
    def test_bad_landmarks(self):
        for x,w in [(0,10),(270,20),(80,0),(20,121)]:
            data=bytearray(package()); struct.pack_into('<H',data,8,x); struct.pack_into('<H',data,12,w)
            with self.assertRaises(ValueError): service.validate(data)
    def test_empty_manifest_and_eight_slots(self):
        self.assertEqual(service.manifest()['avatars'],[])
        for slot in range(8): service.store(slot,package(slot))
        self.assertEqual(len(service.manifest()['avatars']),8)
    def test_replace_failure_preserves_readable_old(self):
        service.store(0,package(1))
        with patch.object(service.os,'replace',side_effect=OSError('injected')):
            with self.assertRaises(OSError): service.store(0,package(2))
        self.assertEqual(service.read(0),package(1))
        self.assertEqual(list(self.root.glob('.upload-*')),[])

if __name__=='__main__': unittest.main()

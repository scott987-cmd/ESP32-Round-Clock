import io
import json
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch
from PIL import Image
import artwork_library as lib

class ArtworkTests(unittest.TestCase):
    def setUp(self):
        self.temp=tempfile.TemporaryDirectory();self.root=Path(self.temp.name)
        self.p=patch.object(lib,'ROOT',self.root/'library');self.p.start()
        self.raw=b'\xff\xff'*(466*466)
        out=io.BytesIO();Image.new('RGB',(466,466),'white').save(out,format='PNG');self.png=out.getvalue()
        self.meta={'prompt':'桂林山水','generated_at':'2026-09-16T00:13:00+00:00'}
    def tearDown(self):self.p.stop();self.temp.cleanup()
    def store(self):return lib.store('wallpaper',self.raw,self.meta,self.png)
    def test_exact_recovery_and_real_thumbnail(self):
        ident=self.store();self.assertEqual(lib.read(ident),self.raw)
        self.assertEqual(lib.read(ident,'thumb'),b'\xff\xff'*(128*128))
        row=lib.list_works()['items'][0];self.assertEqual(row['dateLabel'],'09-16 08:13');self.assertEqual(row['title'],'桂林山水')
    def test_generation_never_overwrites_history(self):
        first=self.store();second=lib.store('wallpaper',b'\0\0'*(466*466),{'prompt':'另一张'})
        self.assertNotEqual(first,second);self.assertEqual(lib.read(first),self.raw);self.assertEqual(len(lib.list_works()['items']),2)
    def test_restart_migration_idempotent_keeps_favorite(self):
        (self.root/'current.rgb565').write_bytes(self.raw);(self.root/'current.png').write_bytes(self.png)
        (self.root/'metadata.json').write_text(json.dumps(self.meta))
        lib.migrate_current(self.root);ident=lib.list_works()['items'][0]['id'];lib.mutate(ident,'favorite',True)
        lib.migrate_current(self.root);self.assertEqual(len(lib.list_works()['items']),1);self.assertTrue(lib.list_works()['items'][0]['favorite'])
    def test_soft_delete_and_restore(self):
        ident=self.store();lib.mutate(ident,'trash');self.assertEqual(lib.list_works()['items'],[])
        self.assertEqual(lib.list_works(trash=True)['items'][0]['id'],ident)
        with self.assertRaises(FileNotFoundError):lib.read(ident)
        lib.mutate(ident,'restore');self.assertEqual(lib.read(ident),self.raw)
    def test_applied_wallpaper_survives_new_generation(self):
        ident=self.store();lib.mutate(ident,'select')
        lib.store('wallpaper',b'\0\0'*(466*466),{'prompt':'新作品'})
        self.assertEqual(lib.selected_wallpaper()[0],self.raw)
        with self.assertRaises(ValueError):lib.mutate(ident,'trash')
    def test_no_catalogue_row_on_write_failure(self):
        with patch.object(lib.os,'replace',side_effect=OSError('full')):
            with self.assertRaises(OSError):self.store()
        self.assertEqual(lib.list_works()['items'],[])
    def test_reject_traversal_and_invalid_actions(self):
        for value in ['../current',None,'f'*31,'G'*32,'/etc/passwd']:
            with self.assertRaises(ValueError):lib.read(value)
        ident=self.store()
        for action,value in [('favorite','true'),('delete',None),('favorite',1)]:
            with self.assertRaises(ValueError):lib.mutate(ident,action,value)
        with self.assertRaises(ValueError):lib.read(ident,'../../key')
    def test_music_roundtrip_and_validation(self):
        ident=lib.store('music',b'\x01\x02'*100,{'prompt':'音乐'});self.assertEqual(lib.read(ident),b'\x01\x02'*100)
        with self.assertRaises(ValueError):lib.mutate(ident,'select')
        for bad in [b'',b'1']:
            with self.assertRaises(ValueError):lib.store('music',bad,{})
    def test_pagination(self):
        for i in range(9):lib.store('music',bytes([i,0])*100,{'prompt':str(i)})
        one=lib.list_works();two=lib.list_works(6)
        self.assertTrue(one['more']);self.assertFalse(two['more']);self.assertEqual(len(one['items'])+len(two['items']),9)
        with self.assertRaises(ValueError):lib.list_works(-1)

if __name__=='__main__':unittest.main()

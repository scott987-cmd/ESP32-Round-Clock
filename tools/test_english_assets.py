import array
import hashlib
import json
import unittest
import zlib
from pathlib import Path
from build_english_assets import rgb565
from PIL import Image

BASE=Path(__file__).resolve().parents[1]/'main/assets/english'

class EnglishAssetsTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.manifest=json.loads((BASE/'manifest.json').read_text())
        cls.pack=(BASE/'english.bin').read_bytes()

    def test_complete_named_curriculum(self):
        self.assertEqual([(x['word'],x['chinese']) for x in self.manifest['words']],
            [('cat','猫'),('dog','狗'),('bird','鸟'),('fish','鱼'),('apple','苹果'),('banana','香蕉'),
             ('orange','橙子'),('strawberry','草莓'),('ball','球'),('car','汽车'),('book','书'),('cup','杯子')])

    def test_pack_identity_and_size(self):
        self.assertEqual(len(self.pack),self.manifest['packBytes'])
        self.assertEqual(hashlib.sha256(self.pack).hexdigest(),self.manifest['sha256'])
        self.assertLess(len(self.pack),512*1024)

    def test_all_offset_ranges_and_alignment(self):
        end=0
        for word in self.manifest['words']:
            self.assertEqual(word['imageOffset']%4,0)
            self.assertGreaterEqual(word['imageOffset'],end)
            self.assertEqual(word['thumbnailOffset'],word['imageOffset']+word['imageBytes'])
            self.assertGreaterEqual(word['audioOffset'],word['thumbnailOffset']+word['thumbnailBytes'])
            self.assertEqual(word['audioOffset']%4,0)
            at=word['thumbnailOffset']
            self.assertEqual(len(zlib.decompress(self.pack[at:at+word['thumbnailBytes']])),128*128*2)
            end=word['audioOffset']+word['audioBytes']
            self.assertLessEqual(end,len(self.pack))

    def test_compiled_pictures_match_reviewed_pngs(self):
        for word in self.manifest['words']:
            image=Image.open(BASE/'cards'/f"{word['word']}.png").convert('RGB')
            self.assertEqual(image.size,(176,176))
            at=word['imageOffset']
            self.assertEqual(rgb565(image),zlib.decompress(self.pack[at:at+word['imageBytes']]))

    def test_audio_is_exact_nonempty_and_unclipped_pcm(self):
        for word in self.manifest['words']:
            raw=(BASE/'audio'/f"{word['word']}.pcm").read_bytes()
            self.assertEqual(len(raw),word['audioBytes'])
            self.assertEqual(raw,self.pack[word['audioOffset']:word['audioOffset']+len(raw)])
            samples=array.array('h',raw)
            self.assertGreater(max(abs(x) for x in samples),1000)
            self.assertLess(max(abs(x) for x in samples),32767)
            self.assertTrue(.25<len(raw)/32000<3)

    def test_no_duplicate_picture_or_pronunciation(self):
        for extension,folder in [('png','cards'),('pcm','audio')]:
            hashes={hashlib.sha256((BASE/folder/f"{x['word']}.{extension}").read_bytes()).hexdigest() for x in self.manifest['words']}
            self.assertEqual(len(hashes),12)

if __name__=='__main__': unittest.main()

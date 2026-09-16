"""Independent validation of the shipped flash pack and resource boundaries."""
import csv,re,struct,unittest,zlib
from pathlib import Path

ROOT=Path(__file__).resolve().parents[1]
class ContentTests(unittest.TestCase):
    def test_header_crc_and_original_english_bytes(self):
        data=(ROOT/'main/assets/content.bin').read_bytes();magic,version,length,crc=struct.unpack('<4sIII',data[:16])
        self.assertEqual((magic,version,length),(b'RCAS',1,len(data)-16));self.assertEqual(crc,zlib.crc32(data[16:]));self.assertLess(len(data),8*1024*1024)
        english=(ROOT/'main/assets/english/english.bin').read_bytes();self.assertEqual(data[16:16+len(english)],english)
    def test_all_builtin_audio_ranges_and_non_silent_pcm(self):
        data=(ROOT/'main/assets/content.bin').read_bytes();header=(ROOT/'main/content_index.h').read_text()
        entries=re.findall(r',([0-9]+),([0-9]+)\}',header);self.assertEqual(len(entries),5)
        for i,(start,size) in enumerate(entries):
            start,size=int(start),int(size);self.assertEqual(size%2,0);self.assertLessEqual(start+size,len(data))
            pcm=data[start:start+size];self.assertEqual(pcm,(ROOT/f'main/assets/stories/builtin-{i}.pcm').read_bytes())
            samples=struct.unpack('<'+'h'*(size//2),pcm);self.assertGreater(max(map(abs,samples)),1000)
    def test_existing_partition_offsets_unchanged(self):
        def number(value):
            value=value.strip().upper()
            if value.endswith('M'):return int(value[:-1])*1024*1024
            if value.endswith('K'):return int(value[:-1])*1024
            return int(value,0)
        partitions={};cursor=0
        with (ROOT/'partitions.csv').open(newline='') as source:
            for row in csv.reader(line for line in source if not line.lstrip().startswith('#')):
                name,type_,_,offset,size=(item.strip() for item in row[:5])
                alignment=0x10000 if type_=='app' else 0x1000
                start=number(offset) if offset else (cursor+alignment-1)&~(alignment-1)
                length=number(size);partitions[name]=(start,length);cursor=start+length
        self.assertEqual(partitions['nvs'],(0x9000,0x6000));self.assertEqual(partitions['factory'],(0x10000,0x800000));self.assertEqual(partitions['wallpaper'],(0x810000,0x800000));self.assertEqual(partitions['content'],(0x1010000,0x800000))

if __name__=='__main__':unittest.main()

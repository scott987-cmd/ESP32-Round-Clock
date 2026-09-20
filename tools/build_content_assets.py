"""Build a separate read-only content partition; never overwrite user storage."""
import array,json,struct,subprocess,sys,zlib
from pathlib import Path
from PIL import Image
ROOT=Path(__file__).resolve().parents[1]
NODES=[
    ('小猫找星星','小猫发现窗外有一颗闪亮的星星。它想把这份美好分享给朋友。先去找谁一起出发呢？',0,'去问小鸟','去问小鱼',1,2),
    ('小鸟的邀请','小鸟说：星星在天上，不用摘下来。我们可以找一个开阔的地方，一起欣赏。小猫点点头。',2,'一起看星星','回家画星星',3,4),
    ('小鱼的秘密','小鱼指着水面说：看，星星也在湖里跳舞！那是星光的倒影。小猫知道了，水里的星星不用捞。',3,'一起看星星','回家画星星',3,4),
    ('分享美好','朋友们坐在一起，看星星一闪一闪。小猫说：原来，美好的东西不一定要带走，和朋友分享也很快乐。',0,'再听一遍','重新开始',-1,0),
    ('小小画家','小猫回到家，把今晚看到的星光画在纸上，送给朋友。大家开心地说：这是一份温暖的礼物！',10,'再听一遍','重新开始',-1,0),
]
PET_ATLAS='minimax-image-20260920-211518-547c9ea7.jpg'
PETS=[('cat','团团'),('dog','旺旺'),('parrot','小羽'),('frog','跳跳')]

def rgb565(image):
    pixels=image.get_flattened_data() if hasattr(image,'get_flattened_data') else image.getdata()
    result=array.array('H',[((r>>3)<<11)|((g>>2)<<5)|(b>>3) for r,g,b in pixels])
    if sys.byteorder!='little':result.byteswap()
    return result.tobytes()

def pet_picture(cell,size=220):
    source=Image.open(ROOT/'main/assets/pets/source'/PET_ATLAS).convert('RGB')
    w,h=source.size
    image=source.crop((cell%2*w//2,cell//2*h//2,(cell%2+1)*w//2,(cell//2+1)*h//2))
    mask=Image.new('L',image.size)
    mask.putdata([255 if min(p)<238 else 0 for p in image.getdata()])
    bounds=mask.getbbox()
    if not bounds:raise ValueError('empty pet cell')
    x0,y0,x1,y1=bounds;pad=18
    image=image.crop((max(0,x0-pad),max(0,y0-pad),min(image.width,x1+pad),min(image.height,y1+pad)))
    image.thumbnail((size-8,size-8),Image.Resampling.LANCZOS)
    result=Image.new('RGB',(size,size),'white')
    result.paste(image,((size-image.width)//2,(size-image.height)//2))
    return result
def main():
    folder=ROOT/'main/assets/stories';folder.mkdir(parents=True,exist_ok=True)
    pet_cards=ROOT/'main/assets/pets/cards';pet_cards.mkdir(parents=True,exist_ok=True)
    english=(ROOT/'main/assets/english/english.bin').read_bytes()
    data=bytearray(english)
    while len(data)%4:data.append(0)
    entries=[]
    for i,(title,text,picture,left,right,a,b) in enumerate(NODES):
        pcm=folder/f'builtin-{i}.pcm'
        if not pcm.exists():
            subprocess.run(['/usr/bin/say','-v','Tingting','-r','175','-o',str(folder/f'builtin-{i}.aiff'),text],check=True)
            subprocess.run(['/opt/homebrew/bin/ffmpeg','-y','-loglevel','error','-i',str(folder/f'builtin-{i}.aiff'),'-ar','16000','-ac','1','-af','volume=0.65,apad=pad_dur=0.15','-f','s16le',str(pcm)],check=True)
        raw=pcm.read_bytes();assert len(raw)%2==0 and 3200<len(raw)<1024*1024
        offset=len(data)+16;data.extend(raw)
        while len(data)%4:data.append(0)
        entries.append('{%s,%s,%s,%s,%d,%d,%d,%d,%d}'%(json.dumps(title,ensure_ascii=False),json.dumps(text,ensure_ascii=False),json.dumps(left,ensure_ascii=False),json.dumps(right,ensure_ascii=False),picture,a,b,offset,len(raw)))
    pet_entries=[]
    for cell,(kind,name) in enumerate(PETS):
        image=pet_picture(cell);image.save(pet_cards/f'{kind}.png')
        compressed=zlib.compress(rgb565(image),9);offset=len(data)+16;data.extend(compressed)
        while len(data)%4:data.append(0)
        pet_entries.append('{%s,%s,%d,%d}'%(json.dumps(kind),json.dumps(name,ensure_ascii=False),offset,len(compressed)))
    crc=zlib.crc32(data)
    pack=struct.pack('<4sIII',b'RCAS',1,len(data),crc)+data
    assert len(pack)<8*1024*1024
    (ROOT/'main/assets/content.bin').write_bytes(pack)
    (ROOT/'main/content_index.h').write_text('#pragma once\n#include <stdint.h>\n'
        f'#define CONTENT_BYTES {len(pack)}u\n#define CONTENT_CRC {crc}u\n#define CONTENT_ENGLISH_OFFSET 16u\n#define CONTENT_ENGLISH_BYTES {len(english)}u\n'
        'typedef struct {const char *title,*text,*left,*right;int picture,a,b;uint32_t audio,bytes;} story_node_t;\n'
        'static const story_node_t builtin_story[]={\n'+',\n'.join(entries)+'\n};\n'
        '#define PET_ASSET_COUNT 4\n#define PET_ASSET_SIZE 220\n'
        'typedef struct {const char *kind,*name;uint32_t offset,bytes;} pet_asset_t;\n'
        'static const pet_asset_t builtin_pets[PET_ASSET_COUNT]={\n'+',\n'.join(pet_entries)+'\n};\n')
    print('Content partition bytes:',len(pack),'CRC:',hex(crc))
if __name__=='__main__':main()

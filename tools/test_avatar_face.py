"""Run the actual firmware pixel renderer on host, and inspect its output frames."""
import ctypes
import json
import struct
import subprocess
import sys
from pathlib import Path
from PIL import Image, ImageDraw

root=Path(__file__).resolve().parents[1]
folder=root/'artifacts/avatar-completion'
folder.mkdir(exist_ok=True,parents=True)
subprocess.run(['cc','-O2','-shared','-fPIC',str(root/'main/avatar_face.c'),'-o',str(folder/'face.dylib')],check=True)
lib=ctypes.CDLL(str(folder/'face.dylib'))
lib.avatar_package_valid.argtypes=[ctypes.c_void_p,ctypes.c_size_t]
lib.avatar_package_valid.restype=ctypes.c_bool
lib.avatar_face_render.argtypes=[ctypes.c_void_p,ctypes.c_void_p,ctypes.c_void_p,ctypes.c_int,ctypes.c_float]
data=Path(sys.argv[1]).read_bytes()
assert lib.avatar_package_valid(data,len(data))
for bad in [data[:-1],b'FAIL'+data[4:],data[:-1]+bytes([data[-1]^1])]:
    assert not lib.avatar_package_valid(bad,len(bad))
source=ctypes.create_string_buffer(data[64:])
target=ctypes.create_string_buffer(156800)
regions=ctypes.create_string_buffer(data[8:32])
def image(pixels):
    rgb=[]
    for (value,) in struct.iter_unpack('<H',pixels):
        rgb.append(((value>>11)*255//31,((value>>5)&63)*255//63,(value&31)*255//31))
    out=Image.new('RGB',(280,280)); out.putdata(rgb); return out
original=image(data[64:]); original.save(folder/'face-original.png')
landmarks=original.copy(); draw=ImageDraw.Draw(landmarks)
for i in range(3):
    x,y,w,h=struct.unpack_from('<4H',data,8+i*8); draw.rectangle((x,y,x+w,y+h),outline='cyan')
landmarks.save(folder/'face-landmarks.png')
counts=[]
for mood,name in enumerate(['blink','smile','kiss','wink']):
    lib.avatar_face_render(source,target,regions,mood,1)
    result=target.raw[:156800]
    changed=sum(result[i:i+2]!=data[i+64:i+66] for i in range(0,156800,2))
    assert 20<changed<12000,(name,changed)
    assert result[:280*20*2]==data[64:64+280*20*2], 'unrelated image region changed'
    image(result).save(folder/f'face-{name}.png'); counts.append({'mood':name,'changedPixels':changed})
    lib.avatar_face_render(source,target,regions,mood,0)
    assert target.raw[:156800]==data[64:],'rest must restore original exactly'
(folder/'renderer-results.json').write_text(json.dumps(counts,indent=2))
print('PASS: C protocol validation; four local facial deformations; untouched background; exact restore',counts)

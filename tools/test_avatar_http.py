import hashlib
import json
import subprocess
import re
from pathlib import Path

root=Path(__file__).resolve().parents[1]
out=root/'artifacts/avatar-completion'
secret=(root/'main/clock_secrets.h').read_text()
base=re.search(r'#define\s+CLOCK_API_BASE\s+"([^"]+)"',secret)[1]
token=re.search(r'#define\s+CLOCK_WALLPAPER_TOKEN\s+"([^"]+)"',secret)[1]
url=base+'/v1/avatars'
def request(path='',data=None,slot=7,auth=True):
    args=['curl','--silent','--show-error','--max-time','20','--cacert',str(root/'main/certs/device-ca.pem')]
    if auth: args+=['--cert',str(root/'main/certs/device-client.pem'),'--key',str(root/'main/certs/device-client.key')]
    if auth: args+=['-H','Authorization: Bearer '+token]
    if data is not None: args+=['-H','Content-Type: application/octet-stream','-H',f'X-Avatar-Slot: {slot}','--data-binary','@-']
    result=subprocess.run(args+['-w','\n%{http_code}',url+path],input=data,capture_output=True,check=True)
    body,code=result.stdout.rsplit(b'\n',1)
    return int(code),body
checks=[]
code,body=request(); assert code==200
manifest=json.loads(body); assert len(manifest['avatars'])==2
checks.append('private manifest lists two explicitly uploaded test avatars')
original=(out/'test-astronaut.rav').read_bytes()
code,readback=request('?slot=7'); assert code==200 and readback==original
checks.append('server readback equals native assistant package byte for byte')
for slot,payload in [(8,original),(7,original[:-1]),(7,original[:-1]+bytes([original[-1]^1]))]:
    code,_=request(data=payload,slot=slot); assert code==400
    assert request('?slot=7')[1]==original
checks.append('invalid slot, partial upload, bad checksum rejected without changing previous asset')
code,_=request(auth=False); assert code==400
checks.append('no client certificate rejected by nginx')
code,_=request('?slot=../test'); assert code==400
checks.append('path traversal rejected')
(out/'http-results.json').write_text(json.dumps({'checks':checks,'manifest':manifest},ensure_ascii=False,indent=2))
print(checks)

#!/usr/bin/env python3
"""Deploy selected service modules with rollback, without printing credentials.

Uses the device's ignored endpoint config and an explicitly supplied SSH key.
Only the existing round-clock service and its exact nginx site are modified.
"""
import argparse
import base64
import json
import re
import shlex
import subprocess
import urllib.parse
from pathlib import Path

ROOT=Path(__file__).resolve().parents[1]
MODULES=('wallpaper_service.py','reset_projection.py','reset_insights.py','pet_service.py','music_jobs.py','artwork_library.py')
REMOTE=r'''
import base64,hashlib,json,os,pathlib,py_compile,shutil,subprocess,sys,time
data=json.load(sys.stdin)
root=pathlib.Path('/opt/esp32-wallpaper')
site=pathlib.Path('/etc/nginx/sites-enabled/esp32-device').resolve()
config=site.read_text()
if 'listen 8080 ssl' not in config or '127.0.0.1:18080' not in config or 'ssl_verify_client on' not in config:
    raise SystemExit('Unexpected nginx site; no changes made')
backup=root/'backups'/time.strftime('%Y%m%d-%H%M%S')
backup.mkdir(parents=True,exist_ok=False)
shutil.copy2(site,backup/'nginx.conf')
previous={}
for name,encoded in data.items():
    if name not in ('wallpaper_service.py','reset_projection.py','reset_insights.py','pet_service.py','music_jobs.py','artwork_library.py'):
        raise SystemExit('Unexpected deployment module')
    path=root/name;previous[name]=path.exists()
    if path.exists():shutil.copy2(path,backup/name)
    staged=backup/(name+'.candidate');staged.write_bytes(base64.b64decode(encoded,validate=True))
    py_compile.compile(str(staged),doraise=True)
try:
    for name in data:
        dest=root/name;shutil.copy2(backup/(name+'.candidate'),dest);dest.chmod(0o644)
    if 'location = /v1/music/status' not in config:
        needle='    location = /v1/music {'
        if config.count(needle)!=1:raise RuntimeError('Ambiguous music insertion point')
        config=config.replace(needle,'    location = /v1/music/status {\n        limit_req zone=round_clock_api burst=30 nodelay;\n        proxy_pass http://127.0.0.1:18080;\n    }\n\n'+needle)
    if 'location = /v1/pet' not in config:
        needle='    location = /v1/stories {'
        if config.count(needle)!=1:raise RuntimeError('Ambiguous nginx insertion point')
        config=config.replace(needle,'    location = /v1/pet {\n        client_max_body_size 2k;\n        proxy_pass http://127.0.0.1:18080;\n    }\n\n'+needle)
    site.write_text(config)
    subprocess.run(['nginx','-t'],check=True,capture_output=True)
    subprocess.run(['systemctl','restart','esp32-wallpaper'],check=True,capture_output=True)
    time.sleep(2)
    subprocess.run(['systemctl','is-active','--quiet','esp32-wallpaper'],check=True)
    subprocess.run(['systemctl','reload','nginx'],check=True,capture_output=True)
except Exception:
    for name,existed in previous.items():
        if existed:shutil.copy2(backup/name,root/name)
        else:(root/name).rename(backup/(name+'.failed'))
    shutil.copy2(backup/'nginx.conf',site)
    subprocess.run(['systemctl','restart','esp32-wallpaper'],capture_output=True)
    raise SystemExit('Deployment failed; restored previous service and site')
print(json.dumps({'active':True,'backup':backup.name,'sha256':{n:hashlib.sha256((root/n).read_bytes()).hexdigest() for n in data}}))
'''


def main():
    parser=argparse.ArgumentParser();parser.add_argument('--identity',type=Path,required=True);args=parser.parse_args()
    secret=(ROOT/'main/clock_secrets.h').read_text()
    host=urllib.parse.urlparse(re.search(r'#define\s+CLOCK_API_BASE\s+"([^"]+)"',secret)[1]).hostname
    payload={name:base64.b64encode((ROOT/'server'/name).read_bytes()).decode() for name in MODULES}
    result=subprocess.run(['ssh','-o','BatchMode=yes','-o','ConnectTimeout=10','-i',str(args.identity),'root@'+host,
                           'python3 -c '+shlex.quote(REMOTE)],input=json.dumps(payload),text=True,capture_output=True)
    if result.returncode:raise SystemExit('Deployment failed; inspect protected SSH diagnostics locally')
    value=json.loads(result.stdout)
    import hashlib
    assert all(value['sha256'][n]==hashlib.sha256((ROOT/'server'/n).read_bytes()).hexdigest() for n in MODULES)
    print(json.dumps(value,indent=2))


if __name__=='__main__':main()

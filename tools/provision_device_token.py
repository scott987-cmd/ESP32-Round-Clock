#!/usr/bin/env python3
"""Provision one device bearer token without printing or passing it in argv."""

from __future__ import annotations

import argparse
import os
import re
import secrets
import shlex
import subprocess
from pathlib import Path


TOKEN_PATTERN = re.compile(r'(#define\s+CLOCK_WALLPAPER_TOKEN\s+")[^"]*(")')
BASE_PATTERN = re.compile(r'#define\s+CLOCK_API_BASE\s+"(https://[^"]+)"')


def update_header(path: Path, token: str) -> str:
    source = path.read_text()
    if not TOKEN_PATTERN.search(source):
        raise ValueError(f"CLOCK_WALLPAPER_TOKEN is missing from {path}")
    base = BASE_PATTERN.search(source)
    if not base:
        raise ValueError(f"CLOCK_API_BASE is missing from {path}")
    path.write_text(TOKEN_PATTERN.sub(lambda match: match[1] + token + match[2], source))
    path.chmod(0o600)
    return base[1]


def write_private(path: Path, value: str) -> None:
    path.parent.mkdir(mode=0o700, parents=True, exist_ok=True)
    path.write_text(value)
    path.chmod(0o600)


def update_remote(host: str, identity: Path, remote_env: str, token: str) -> None:
    program = """import os,sys,tempfile
p=sys.argv[1]; token=sys.stdin.read().strip()
if len(token)<32: raise SystemExit('invalid token')
try: lines=open(p).read().splitlines()
except FileNotFoundError: lines=[]
values={'WALLPAPER_TOKEN':token,'REQUIRE_WALLPAPER_TOKEN':'1'}
out=[]
for line in lines:
 key=line.split('=',1)[0] if '=' in line else ''
 if key in values: out.append(key+'='+values.pop(key))
 else: out.append(line)
out.extend(k+'='+v for k,v in values.items())
fd,tmp=tempfile.mkstemp(prefix='.esp32-wallpaper.',dir=os.path.dirname(p),text=True)
with os.fdopen(fd,'w') as f: f.write('\\n'.join(out)+'\\n')
os.chmod(tmp,0o600); os.replace(tmp,p)
"""
    remote_command = "python3 -c " + shlex.quote(program) + " " + shlex.quote(remote_env)
    subprocess.run(
        ["ssh", "-i", str(identity), host, remote_command],
        input=token, text=True, check=True,
    )


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--header", type=Path, default=Path("main/clock_secrets.h"))
    parser.add_argument("--client-config-dir", type=Path)
    parser.add_argument("--ssh-host")
    parser.add_argument("--identity", type=Path)
    parser.add_argument("--remote-env", default="/etc/esp32-wallpaper.env")
    args = parser.parse_args()
    if bool(args.ssh_host) != bool(args.identity):
        parser.error("--ssh-host and --identity must be used together")
    token = secrets.token_urlsafe(32)
    base = update_header(args.header, token)
    if args.client_config_dir:
        write_private(args.client_config_dir / "device-token", token + "\n")
        write_private(args.client_config_dir / "authorization-header", "Authorization: Bearer " + token + "\n")
        write_private(args.client_config_dir / "api-base", base + "\n")
    if args.ssh_host:
        update_remote(args.ssh_host, args.identity, args.remote_env, token)
    print("Provisioned a new device token locally" + (" and remotely" if args.ssh_host else ""))


if __name__ == "__main__":
    main()

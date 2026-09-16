"""Local verification client using the existing device-bound TLS identity."""
import json
import re
import ssl
import urllib.request
from pathlib import Path

ROOT=Path(__file__).resolve().parents[1]
def request(path,body=None):
    if not path.startswith('/v1/'):raise ValueError('only device API paths')
    context=ssl.create_default_context(cafile=str(ROOT/'main/certs/device-ca.pem'))
    # Existing private CA predates Python 3.13 strict extension checks. Keep
    # chain verification and hostname matching; match the deployed ESP TLS policy.
    context.verify_flags &= ~ssl.VERIFY_X509_STRICT
    context.load_cert_chain(str(ROOT/'main/certs/device-client.pem'),str(ROOT/'main/certs/device-client.key'))
    secret=(ROOT/'main/clock_secrets.h').read_text()
    match=re.search(r'#define\s+CLOCK_WALLPAPER_TOKEN\s+"([^"]*)"',secret)
    base_match=re.search(r'#define\s+CLOCK_API_BASE\s+"(https://[^"]+)"',secret)
    if not base_match:raise ValueError('CLOCK_API_BASE missing from clock_secrets.h')
    headers={'Content-Type':'application/json'}
    if match and match[1]:headers['Authorization']='Bearer '+match[1]
    req=urllib.request.Request(base_match[1]+path,
        data=None if body is None else json.dumps(body).encode(),headers=headers)
    with urllib.request.urlopen(req,context=context,timeout=30) as response:return response.read()  # nosec B310

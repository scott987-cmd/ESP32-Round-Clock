"""Temporary device-only Wi-Fi loss; does not change the computer's network.

USB debug firmware restores the original network on RCNETON or after five minutes.
No generation, deletion or credential changes. --baseline records missing features.
"""
import argparse
import json
import time
from pathlib import Path
from device_ui_test import Device

def main():
    p=argparse.ArgumentParser()
    p.add_argument('--port',default='/dev/cu.usbmodem21201')
    p.add_argument('--baseline',action='store_true')
    args=p.parse_args()
    out=Path('artifacts/offline-review')/('baseline' if args.baseline else 'fixed')
    out.mkdir(parents=True,exist_ok=True)
    d=Device(args.port,out)
    results=[]
    try:
        initial=d.state()
        print('Initial Wi-Fi link:',initial['wifi_connected'],flush=True)
        d.view('library')
        until=time.monotonic()+35
        while d.state()['library_busy'] and time.monotonic()<until:time.sleep(.5)
        before=d.state()
        d.command('RCNETOFF');time.sleep(1)
        assert not d.state()['wifi_connected']
        last=before['uptime_ms']
        for cycle in range(2):
            for name in ('clock','desktop','weather','quota','reset','agents','notes','music','radio','library','english','story','pet','avatar','settings','wifi'):
                start=time.monotonic();d.view(name);s=d.state()
                assert s['uptime_ms']>last,'Unexpected device restart'
                last=s['uptime_ms']
                results.append({'view':name,'cycle':cycle,'response_seconds':round(time.monotonic()-start,3),'free_internal':s['free_internal']})
            print('PASS offline navigation cycle',cycle+1,flush=True)
        d.view('library');time.sleep(2);after=d.state()
        results.append({'library_before':before['library_count'],'library_after':after['library_count'],'library_busy':after['library_busy']})
        if not args.baseline:
            assert not after['library_busy']
            assert after['library_count']==before['library_count']
        d.screenshot('library-offline')
        print('PASS device remained responsive without reboot; library count',after['library_count'],flush=True)
    finally:
        try:d.command('RCNETON');d.view('clock')
        finally:
            d.connection.close()
            (out/'results.json').write_text(json.dumps(results,indent=2))

if __name__=='__main__':main()

"""Real round-screen music recovery/playback. --generate creates ONE paid track.

Run once after flashing and again after a device restart. No content is removed.
"""
import argparse
import json
import time
from pathlib import Path
from device_ui_test import Device
from device_api_client import request


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--port', default='/dev/cu.usbmodem21201')
    parser.add_argument('--generate', action='store_true')
    parser.add_argument('--full-playback', action='store_true')
    parser.add_argument('--output', type=Path, default=Path('artifacts/music-review'))
    args = parser.parse_args(); args.output.mkdir(parents=True, exist_ok=True)
    d = Device(args.port, args.output)
    def wait(predicate, seconds=45):
        until = time.monotonic() + seconds
        while time.monotonic() < until:
            s = d.state()
            if predicate(s): return s
            time.sleep(1)
        raise AssertionError('device state deadline exceeded')
    def screenshot(name):
        assert all(v.get('missing_glyphs', 0) == 0 for v in d.state()['labels'])
        d.screenshot(name)
    try:
        expected = json.loads(request('/v1/music/status'))
        s = wait(lambda s: not s['music_busy'] and bool(s['music_saved'][0]))
        assert s['music_saved'][0] == expected['latest']['artworkId']
        print('PASS recovered server-saved music on device', flush=True)
        d.view('music'); screenshot('music-saved')
        d.tap(170, 405)
        wait(lambda s: s['music_playing'] and s['music_written'] > 32000)
        d.view('clock')
        before = d.state()['music_written']; time.sleep(2)
        assert d.state()['music_written'] > before
        print('PASS real speaker writes continue outside music app', flush=True)
        d.view('music'); d.tap(170, 405)
        wait(lambda s: not s['music_playing'] and not s['music_busy'])
        d.tap(230, 313); assert d.state()['view'] == 18
        s = wait(lambda s: not s['library_busy'])
        assert s['library_music_only'] and s['library_kind'] == 'music'
        print('PASS stop and saved-library entry', flush=True)
        screenshot('my-music')
        d.view('clock'); d.view('library')
        s = wait(lambda s: not s['library_busy'])
        assert not s['library_music_only']
        print('PASS desktop library remains unfiltered', flush=True)
        d.view('radio'); d.tap(120, 160)
        assert d.state()['radio_station'] == 1
        if args.generate:
            previous = s['music_saved'][1]
            d.tap(310, 266)
            wait(lambda s: s['music_status'] == 'CREATING MUSIC...')
            screenshot('radio-generating'); d.view('clock')
            print('RUNNING one real sky-channel generation in background', flush=True)
            s = wait(lambda s: not s['music_busy'], 540)
            assert s['music_saved'][1] and s['music_saved'][1] != previous, s['music_status']
            print('PASS background channel generation saved', flush=True)
        d.view('radio'); time.sleep(1); screenshot('radio-ready')
        s = d.state()
        if s['music_saved'][1]:
            server = json.loads(request('/v1/music/status'))
            assert s['music_saved'][1] == server['stations']['sky']['artworkId']
            d.tap(185, 414)
            wait(lambda s: s['music_playing'] and s['music_written'] > 32000)
            if args.full_playback:
                print('RUNNING complete saved-channel playback', flush=True)
                duration = server['stations']['sky']['durationMs']
                s = wait(lambda s: not s['music_busy'], duration/1000 + 90)
                assert s['music_status'] == 'MUSIC READY - TAP PLAY'
                assert abs(s['music_written'] - duration*32) < 32
                print('PASS entire track written to speaker without truncation', flush=True)
            else:
                d.tap(185, 414); wait(lambda s: not s['music_playing'] and not s['music_busy'])
            print('PASS channel-specific saved playback', flush=True)
        d.tap(230, 352); assert d.state()['view'] == 18
        s = wait(lambda s: not s['library_busy'])
        assert s['library_music_only'] and s['library_kind'] == 'music'
        d.view('clock')
        print('PASS radio library entry and return to clock', flush=True)
    finally:
        d.connection.close()


if __name__ == '__main__': main()

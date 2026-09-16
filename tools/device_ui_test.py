#!/usr/bin/env python3
"""USB-driven LVGL pointer regression tests on the actual board (no RF simulation)."""
import argparse
import json
import time
from pathlib import Path
import serial
from capture_screen import VIEWS, read_header, read_exact, fnv1a, write_png


class Device:
    def __init__(self, port, output):
        self.output = output
        self.connection = serial.Serial()
        self.connection.port = port
        self.connection.baudrate = 115200
        self.connection.timeout = 0.1
        self.connection.dtr = True
        self.connection.rts = False
        self.connection.open()
        self.connection.reset_input_buffer()

    def until(self, marker, timeout=10):
        data = bytearray()
        end = time.monotonic() + timeout
        while time.monotonic() < end:
            data.extend(self.connection.read(1))
            if data.endswith(marker):
                if len(data)>len(marker):
                    self.output.mkdir(parents=True,exist_ok=True)
                    with (self.output/'serial.log').open('ab') as trace:trace.write(data[:-len(marker)])
                return
            if data.endswith(b'RCER') or data.endswith(b'RSE1'):
                raise RuntimeError(f'Device rejected command: {data[-80:]}')
        raise TimeoutError(f'Waiting for {marker}: {data[-200:]}')

    def command(self, command):
        self.connection.write(command.encode() + b'\n')
        self.until(b'RCOK')

    def state(self):
        self.connection.write(b'RCSTATE\n')
        self.until(b'RCJSON')
        return json.loads(self.connection.readline())

    def view(self, name):
        self.command(VIEWS[name].decode().strip())
        time.sleep(0.3)

    def pointer(self, x, y, pressed):
        self.command(f'RCPTR {int(x)} {int(y)} {int(pressed)}')

    def tap(self, x=233, y=230):
        self.pointer(x, y, True)
        time.sleep(0.07)
        self.pointer(x, y, False)
        time.sleep(0.35)

    def swipe(self, x1, y1, x2, y2):
        self.pointer(x1, y1, True)
        for i in range(1, 13):
            self.pointer(x1+(x2-x1)*i/12, y1+(y2-y1)*i/12, True)
            time.sleep(0.02)
        self.pointer(x2, y2, False)
        time.sleep(0.4)

    def screenshot(self, name):
        self.connection.write(b'RCSHOT\n')
        _, w, h, stride, length, checksum = read_header(self.connection)
        pixels = read_exact(self.connection, length)
        assert fnv1a(pixels) == checksum
        write_png(self.output / f'{name}.png', pixels, w, h, stride)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--port', default='/dev/cu.usbmodem21201')
    parser.add_argument('--output', type=Path, default=Path('artifacts/ui-review'))
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    d = Device(args.port, args.output)
    checks, screens = [], {}

    def check(name, condition):
        checks.append({'name': name, 'passed': bool(condition)})
        print(('PASS ' if condition else 'FAIL ') + name, flush=True)
        if not condition:
            raise AssertionError(name)

    try:
        d.view('desktop')
        start = d.state()['selected']
        targets = [1, 2, 7, 5, 3, 8, 9, 10, 11, 12, 14, 15, 16, 17, 18, 19, 20, 21]
        for i in range(len(targets)):
            before = d.state()['selected']
            d.swipe(330, 230, 145, 230)
            state = d.state()
            selected = (before + 1) % len(targets)
            check(f'left swipe {i}: select without opening', state['view'] == 0 and state['selected'] == selected and not state['animating'])
            d.screenshot(f'launcher-{selected}')
            d.tap()
            check(f'tap {selected}: correct app', d.state()['view'] == targets[selected])
            d.view('desktop')
        check('carousel wraps', d.state()['selected'] == start)
        d.swipe(150, 230, 330, 230)
        check('right swipe reverses', d.state()['selected'] == (start + len(targets) - 1) % len(targets))
        d.tap(400, 230)
        check('tap side preview advances', d.state()['view'] == 0 and d.state()['selected'] == start)
        d.swipe(233, 230, 252, 230)
        check('short drag snaps back without opening', d.state()['view'] == 0 and d.state()['selected'] == start)
        d.swipe(233, 380, 233, 210)
        check('upward swipe opens overview', d.state()['view'] == 13)
        d.view('quota')
        deadline = time.monotonic() + 20
        while d.state()['quota_total'] == '--' and time.monotonic() < deadline:
            time.sleep(1)
        check('quota contains server data', d.state()['quota_total'] != '--')
        d.screenshot('quota-top')
        before = d.state()['quota_scroll']
        d.swipe(240, 350, 240, 190)
        check('quota finger scroll stays in app', d.state()['view'] == 8 and d.state()['quota_scroll'] > before)
        d.screenshot('quota-scrolled')
        d.tap(233, 428)
        check('quota home button', d.state()['view'] == 0)
        for name in VIEWS:
            d.view(name)
            if name == 'quota': d.command('RCQTOP')
            if name == 'settings': d.command('RCSTOP')
            if name == 'overview': d.command('RCATOP')
            time.sleep(0.3)
            screens[name] = d.state()
            check(f'{name}: all displayed glyphs present', all(label.get('missing_glyphs', 0) == 0 for label in screens[name]['labels']))
            d.screenshot(name)
        d.view('settings')
        d.swipe(240, 350, 240, 170)
        check('settings finger scroll', d.state()['view'] == 3 and d.state()['settings_scroll'] > 0)
        d.screenshot('settings-scrolled')
        d.view('settings')
        d.command('RCSTOP')
        for label, y in [('brightness', 265), ('volume', 339)]:
            def read_percent():
                values = [v for v in d.state()['labels'] if v['text'].endswith('%') and abs(v['y'] - y) < 25]
                assert len(values) == 1, values
                return int(values[0]['text'][:-1])
            initial = read_percent()
            delta = -10 if initial > 10 else 10
            d.tap(256 if delta < 0 else 367, y)
            check(f'{label}: control changes value', read_percent() == initial + delta)
            d.tap(367 if delta < 0 else 256, y)
            check(f'{label}: original value restored', read_percent() == initial)
        d.tap(160, 115)
        check('Wi-Fi row opens configuration', d.state()['view'] == 4)
        d.tap(233, 118)
        time.sleep(7)
        check('Wi-Fi scan returns entries', len(d.state()['labels']) > 4)
        d.screenshot('wifi-scan')
        d.tap(233, 180)
        check('Wi-Fi password entry opens', any('输入密码' in v['text'] for v in d.state()['labels']))
        check('Wi-Fi password page glyphs present', all(v.get('missing_glyphs', 0) == 0 for v in d.state()['labels']))
        d.screenshot('wifi-password')
        d.tap(233, 420)
        check('Wi-Fi password cancels without disconnecting', d.state()['view'] == 4 and any('已连接' in v['text'] for v in d.state()['labels']))
        # Read-only presentation tests: selecting a station must not generate music.
        d.view('radio')
        music_before = d.state()['music_status']
        d.tap(230, 160)
        check('radio station selection acknowledged', any('频道已选择' in v['text'] for v in d.state()['labels']))
        check('radio selection preserves existing music state', d.state()['music_status'] == music_before)
        d.screenshot('radio-selected')
        time.sleep(3.1)
        if music_before == 'MUSIC READY - TAP PLAY':
            check('radio restores playable music hint', any('音乐已生成' in v['text'] for v in d.state()['labels']))
        # Very short recordings stay below the upload threshold: no paid model call.
        for name, record_y, stop_y, field, failure in [
            ('music', 210, 210, 'music_status', 'MUSIC IDEA FAILED'),
            ('wallpaper-studio', 370, 370, 'wallpaper_status', 'WALLPAPER IDEA FAILED'),
        ]:
            d.view(name)
            d.pointer(150, record_y, True)
            d.pointer(150, record_y, False)
            d.pointer(310, stop_y, True)
            d.pointer(310, stop_y, False)
            time.sleep(1.2)
            check(f'{name}: short recording exits waiting state', d.state()[field] == failure)
            d.screenshot(name + '-short-recording')
        d.view('desktop')
    finally:
        d.connection.close()
        (args.output / 'results.json').write_text(json.dumps({'checks': checks, 'screens': screens}, ensure_ascii=False, indent=2))


if __name__ == '__main__':
    main()

import json
from pathlib import Path
from device_ui_test import Device

d = Device('/dev/cu.usbmodem21201', Path('artifacts'))
try:
    d.view('desktop')
    before = d.state()
    for _ in range(10):
        d.swipe(330, 230, 145, 230)
    after = d.state()
    count = after['launcher_render_count'] - before['launcher_render_count']
    elapsed = (after['launcher_render_count'] * after['launcher_render_mean_us'] -
               before['launcher_render_count'] * before['launcher_render_mean_us'])
    result = {'rendered_frames': count, 'mean_frame_ms': round(elapsed / count / 1000, 2),
              'scope': 'LVGL render plus submitted SPI flush; not panel refresh rate',
              'state': {k:v for k,v in after.items() if k != 'labels'}}
    print(json.dumps(result, ensure_ascii=False, indent=2))
    Path('artifacts/benchmark.json').write_text(json.dumps(result, ensure_ascii=False, indent=2))
    d.screenshot('benchmark-desktop')
finally:
    d.connection.close()

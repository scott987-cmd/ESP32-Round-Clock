"""Cross-app audio takeover on real hardware. Uses saved music; no paid generation.

All recordings are cancelled, including a >1 second recording that would otherwise
qualify for transcription. USB diagnostic firmware is required.
"""
import argparse
import json
import time
from pathlib import Path
from device_ui_test import Device


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--port', default='/dev/cu.usbmodem21201')
    parser.add_argument('--output', type=Path, default=Path('artifacts/audio-focus'))
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    device = Device(args.port, args.output)
    checks = []

    def wait(predicate, seconds=30):
        end = time.monotonic() + seconds
        while time.monotonic() < end:
            state = device.state()
            if predicate(state):
                return state
            time.sleep(.12)
        raise AssertionError('audio state deadline exceeded')

    def passed(name):
        checks.append(name)
        print('PASS ' + name, flush=True)

    def play():
        wait(lambda s: not s['music_busy'])
        device.command('RCMPLAY')
        return wait(lambda s: s['music_playing'] and s['music_written'] > 16000)

    try:
        wait(lambda s: bool(s['music_saved'][0]) and s['wifi_connected'], 60)
        device.view('clock')
        first = play()['music_written']
        time.sleep(1)
        assert device.state()['music_written'] > first
        passed('clock navigation preserves background music')

        device.view('english')
        device.tap(233, 422)  # Point-reading, also works after quiz auto-read.
        wait(lambda s: not s['music_playing'] and not s['english_playing']
             and s['english_audio_result'] == 0 and s['english_audio_written'] > 0)
        passed('English point-reading preempts streamed music and finishes')
        device.screenshot('english-takeover')

        device.view('clock')
        play()
        device.view('story')
        device.tap(230, 125)  # Built-in book: no network or paid generation.
        wait(lambda s: s['english_playing'] and not s['music_playing'])
        passed('story narration preempts music')
        play()  # Request music while the old story remains visible.
        wait(lambda s: not s['english_playing'])
        device.view('clock')  # Old story's hide callback must not stop music.
        first = device.state()['music_written']
        time.sleep(1)
        assert device.state()['music_written'] > first
        passed('music preempts narration; old story cleanup cannot stop new owner')

        device.view('remote')
        device.tap(150, 210)
        wait(lambda s: s['voice_recording'] and not s['music_playing'])
        time.sleep(1)
        passed('microphone preempts music')
        play()  # Playback preempts an active recording, without navigating away.
        wait(lambda s: not s['voice_recording'] and not s['voice_processing'])
        assert device.state()['voice_status'] == 'RECORDING CANCELLED'
        passed('playback cancels a long recording instead of uploading it')

        device.view('music')
        device.tap(150, 210)
        wait(lambda s: s['voice_recording'] and not s['music_playing'])
        device.view('clock')
        wait(lambda s: not s['voice_recording'] and not s['voice_processing'] and not s['music_busy'])
        assert device.state()['voice_status'] == 'RECORDING CANCELLED'
        passed('music idea recording is available during playback and cancels on leave')

        play()
        device.view('pet')
        device.tap(233, 399)
        wait(lambda s: s['voice_recording'] and s['pet_phase'] == 1 and not s['music_playing'])
        device.view('clock')
        wait(lambda s: not s['voice_recording'] and not s['voice_processing'] and s['pet_phase'] == 4)
        passed('pet microphone takes focus and leaving cancels without a generated reply')

        for _ in range(4):
            device.view('story')
            device.tap(230, 125)
            wait(lambda s: s['english_playing'])
            play()
            device.view('clock')
            first = device.state()['music_written']
            time.sleep(.4)
            assert device.state()['music_written'] > first
        passed('repeated takeovers retain the latest owner without deadlock')
        device.view('music')
        device.tap(170, 405)
        wait(lambda s: not s['music_playing'])
        device.view('clock')
        args.output.joinpath('results.json').write_text(json.dumps({'passed': checks}, indent=2))
    finally:
        # Never leave a microphone capture running if an assertion fails.
        try:
            device.view('clock')
        finally:
            device.connection.close()


if __name__ == '__main__':
    main()

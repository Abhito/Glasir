# SPDX-License-Identifier: MIT
"""Checks every scaling mode end to end, without a capture card.

Pipes ffmpeg's test pattern, smaller than mpv's window, into Glasir.exe
--input-raw, switches the scaling modes the way the in-game menu does
(script-message glasir-scaling), and checks over Glasir's IPC pipe what
glasir_scaling.lua put in place: the shaders, the VSR filter, the mode. Prints
the GPU time mpv's renderer spends per fresh frame (vo-passes) for each mode.
Opens a window for a few seconds per mode.

    python tools/scaling_check.py                        # 960x540 in 1280x720 (1.33x)
    python tools/scaling_check.py --size 640x360         # 2x
    python tools/scaling_check.py --format p010 --hdr
    python tools/scaling_check.py --window 2560x1440 --size 1920x1080   # costs at 1440p
    python tools/scaling_check.py --vsr intel            # this GPU's VSR as if it were Intel's

--vsr changes what glasir_scaling.lua was told about the GPU (normally from
Glasir's GPU check); a VSR the GPU does not have must fall back to FSR 1.
"""
import argparse
import json
import os
import subprocess
import sys
import time

PIPE = r'\\.\pipe\glasir'
FFMPEG_FORMAT = {'nv12': 'nv12', 'p010': 'p010le', 'yuy2': 'yuyv422', 'uyvy': 'uyvy422'}
MODES = ['mpv', 'fsr', 'cnn', 'anime4k', 'vsr', 'mpv']
FSR = 'glasir_fsr1.glsl'
SHADERS = {
    'mpv': [],
    'fsr': [FSR],
    'cnn': ['ArtCNN_C4F16.glsl', FSR],
    'anime4k': ['Anime4K_Clamp_Highlights.glsl', 'Anime4K_Restore_CNN_M.glsl', 'Anime4K_Upscale_CNN_x2_M.glsl',
                'Anime4K_AutoDownscalePre_x2.glsl', 'Anime4K_AutoDownscalePre_x4.glsl',
                'Anime4K_Upscale_CNN_x2_S.glsl'],
}


class Mpv:
    def __init__(self, timeout):
        deadline = time.time() + timeout
        while True:
            try:
                self.pipe = open(PIPE, 'r+b', buffering=0)
                break
            except OSError:
                if time.time() > deadline:
                    raise
                time.sleep(0.1)
        self.next_id = 1
        self.buf = b''

    def __call__(self, *command):
        rid = self.next_id
        self.next_id += 1
        self.pipe.write((json.dumps({'command': list(command), 'request_id': rid}) + '\n').encode())
        while True:
            while b'\n' not in self.buf:
                self.buf += self.pipe.read(65536)
            line, self.buf = self.buf.split(b'\n', 1)
            msg = json.loads(line)
            if msg.get('request_id') == rid:
                return msg.get('data')

    def get(self, name):
        return self('get_property', name)


def median(values):
    v = sorted(values)
    return v[len(v) // 2] if v else 0


def gpu_ms(passes, every=False):
    """GPU time per fresh frame (ms, each pass's median over its last
    samples), and the passes over 0.1 ms."""
    fresh = (passes or {}).get('fresh') or []
    times = [(p.get('desc', '?'), median(p.get('samples') or [p.get('avg', 0)]) / 1e6) for p in fresh]
    big = [f'{d[:48]} {t:.2f}' for d, t in times if every or t > 0.1]
    return sum(t for _, t in times), big


def main():
    p = argparse.ArgumentParser()
    p.add_argument('--app', default=os.path.join(os.path.dirname(__file__), '..', 'out', 'Glasir'))
    p.add_argument('--format', default='nv12', choices=sorted(FFMPEG_FORMAT))
    p.add_argument('--size', default='960x540', help='capture size (smaller than the window)')
    p.add_argument('--window', default='1280x720', help="mpv's window size")
    p.add_argument('--hdr', action='store_true')
    p.add_argument('--vsr', choices=['nvidia', 'intel', 'none'], help='tell the script this VSR instead')
    p.add_argument('--seconds', type=float, default=3, help='per mode')
    p.add_argument('--passes', action='store_true', help="list every render pass, not only the costly ones")
    a = p.parse_args()
    width, height = (int(v) for v in a.size.split('x'))

    ffmpeg = subprocess.Popen(
        ['ffmpeg', '-hide_banner', '-loglevel', 'fatal', '-re', '-f', 'lavfi',
         '-i', f'testsrc2=size={a.size}:rate=60', '-vf', f'format={FFMPEG_FORMAT[a.format]}',
         '-f', 'rawvideo', '-'],
        stdout=subprocess.PIPE)
    glasir = subprocess.Popen(
        [os.path.join(a.app, 'Glasir.exe'), '--no-dialogs', '--input-raw', '-', '--raw-format', a.format,
         '--set', f'capture.width={width}', '--set', f'capture.height={height}', '--set', 'capture.fps=60',
         '--set', 'window.mode=windowed', '--set', f'video.mode={"hdr" if a.hdr else "sdr"}',
         '--set', 'video.scaling=mpv', '--set', 'debug.mpv_log=1'],
        stdin=ffmpeg.stdout)
    ffmpeg.stdout.close()

    ok = True
    try:
        mpv = Mpv(15)
        time.sleep(2)
        if a.window != '1280x720':
            mpv('set_property', 'geometry', a.window)
            time.sleep(1)
        if a.vsr:
            mpv('change-list', 'script-opts', 'append', f'glasir_scaling-vsr={a.vsr}')
            time.sleep(0.5)
        dims = mpv.get('osd-dimensions') or {}
        print(f"capture {a.size} {a.format}{' HDR' if a.hdr else ''} in a {dims.get('w')}x{dims.get('h')} window")
        for mode in MODES:
            mpv('script-message', 'glasir-scaling', mode)
            time.sleep(a.seconds)
            # The timings since the switch only: the passes reset when the shaders change.
            shaders = [os.path.basename(s) for s in (mpv.get('glsl-shaders') or [])]
            vf = [f.get('label') for f in (mpv.get('vf') or []) if f.get('label')]
            shown = mpv.get('user-data/glasir/scaling')
            total, big = gpu_ms(mpv.get('vo-passes'), a.passes)
            vsr_on = 'glasir-vsr' in vf
            if mode == 'vsr':
                # Either the GPU's VSR runs, or FSR 1 stands in for it.
                good = (vsr_on and shaders == []) or (not vsr_on and shaders == [FSR])
                what = 'VSR filter' if vsr_on else 'FSR 1 instead of VSR'
            else:
                good = shaders == SHADERS[mode] and not vsr_on
                what = f'{len(shaders)} shaders' if len(shaders) > 2 else ', '.join(shaders) or 'no shaders'
            good = good and shown == mode
            ok = ok and good
            print(f"  {mode:8s} {'ok  ' if good else 'FAIL'} {what}; {total:.2f} ms GPU per frame")
            for b in big:
                print(f'             {b}')
        try:
            mpv('quit')
        except (OSError, ValueError):
            pass
    except OSError as e:
        print(f'could not talk to mpv: {e}')
        ok = False
    finally:
        try:
            glasir.wait(10)
        except subprocess.TimeoutExpired:
            glasir.kill()
            ok = False
        ffmpeg.kill()

    log = os.path.join(a.app, 'logs', 'mpv.log')
    if os.path.exists(log):
        with open(log, encoding='utf-8', errors='replace') as f:
            for line in f:
                if 'glasir_scaling' in line or 'Super Resolution' in line:
                    print('  ' + line.rstrip())
    print('PASS' if ok and glasir.returncode == 0 else 'FAIL')
    return 0 if ok and glasir.returncode == 0 else 1


if __name__ == '__main__':
    sys.exit(main())

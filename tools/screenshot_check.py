# SPDX-License-Identifier: MIT
"""Checks Glasir's screenshot (the s key) end to end, without a capture card.

Pipes ffmpeg's test pattern into Glasir.exe --input-raw, makes mpv's window
wider than the picture (black bars at the sides), presses s over IPC
(keypress runs mpv's binding) and checks what the overlay saved in
Screenshots\\: a PNG (and a .jxr in HDR) the picture's size, without the bars.
The files are removed afterwards unless --keep. Opens a window for a few
seconds.

    python tools/screenshot_check.py
    python tools/screenshot_check.py --hdr          # 10-bit PQ: .jxr and a tone-mapped PNG
    python tools/screenshot_check.py --hdr --dlss5  # with DLSS 5 on (RTX 50)
    python tools/screenshot_check.py --fallback     # glasir_screenshot.lua: mpv's own shot too

The tone mapping itself is tried on game frames with shot_convert.exe
(build\\vs\\src\\overlay\\Release): shot_convert Captures\\<when>\\f01.png OUT_FOLDER.
"""
import argparse
import json
import os
import subprocess
import sys
import time

PIPE = r'\\.\pipe\glasir'


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


def size_of(path):
    out = subprocess.run(['ffprobe', '-v', 'error', '-select_streams', 'v:0', '-show_entries', 'stream=width,height',
                          '-of', 'csv=p=0', path], capture_output=True, text=True).stdout.strip()
    try:
        w, h = out.split(',')[:2]
        return int(w), int(h)
    except ValueError:
        return None


def main():
    p = argparse.ArgumentParser()
    p.add_argument('--app', default=os.path.join(os.path.dirname(__file__), '..', 'out', 'Glasir'))
    p.add_argument('--hdr', action='store_true')
    p.add_argument('--dlss5', action='store_true')
    p.add_argument('--fallback', action='store_true', help="remove the overlay's user-data first")
    p.add_argument('--keep', action='store_true', help='keep the screenshots')
    a = p.parse_args()
    shots = os.path.join(a.app, 'Screenshots')
    before = set(os.listdir(shots)) if os.path.isdir(shots) else set()

    fmt = 'p010' if a.hdr else 'nv12'
    ffmpeg = subprocess.Popen(
        ['ffmpeg', '-hide_banner', '-loglevel', 'fatal', '-re', '-f', 'lavfi', '-i', 'testsrc2=size=1280x720:rate=60',
         '-vf', f'format={"p010le" if a.hdr else "nv12"}', '-f', 'rawvideo', '-'],
        stdout=subprocess.PIPE)
    glasir = subprocess.Popen(
        [os.path.join(a.app, 'Glasir.exe'), '--no-dialogs', '--input-raw', '-', '--raw-format', fmt,
         '--set', 'capture.width=1280', '--set', 'capture.height=720', '--set', 'capture.fps=60',
         '--set', 'window.mode=windowed', '--set', f'video.mode={"hdr" if a.hdr else "sdr"}',
         '--set', f'dlss5.enabled={"on" if a.dlss5 else "off"}'],
        stdin=ffmpeg.stdout)
    ffmpeg.stdout.close()
    osd = {}
    try:
        mpv = Mpv(15)
        time.sleep(3)
        mpv('set_property', 'geometry', '1600x600')
        time.sleep(1.5)
        osd = mpv('get_property', 'osd-dimensions') or {}
        print(f"window {osd.get('w')}x{osd.get('h')}, bars {osd.get('ml')} + {osd.get('mr')} px; "
              f"overlay: {mpv('get_property', 'user-data/glasir/screenshot')}")
        if a.fallback:
            mpv('del', 'user-data/glasir/screenshot')
        mpv('keypress', 's')
        time.sleep(4)
        try:
            mpv('quit')
        except (OSError, ValueError):
            pass
    finally:
        try:
            glasir.wait(15)
        except subprocess.TimeoutExpired:
            glasir.kill()
        ffmpeg.kill()

    new = sorted(set(os.listdir(shots)) - before) if os.path.isdir(shots) else []
    picture = (osd.get('w', 0) - osd.get('ml', 0) - osd.get('mr', 0), osd.get('h', 0) - osd.get('mt', 0) - osd.get('mb', 0))
    pngs = [f for f in new if f.endswith('.png')]
    jxrs = [f for f in new if f.endswith('.jxr')]
    ok = len(pngs) == 1 and len(jxrs) == (1 if a.hdr else 0)
    for f in new:
        path = os.path.join(shots, f)
        size = size_of(path) if not f.endswith('.jxr') else None
        print(f'  {f}: {os.path.getsize(path)} bytes' + (f', {size[0]}x{size[1]}' if size else ''))
        if f.endswith('.png') and size != picture:
            print(f'  expected the picture without the bars: {picture[0]}x{picture[1]}')
            ok = False
    if a.fallback and not any(f.endswith('.jpg') for f in new):
        print("  expected mpv's own screenshot (.jpg) too")
        ok = False
    log = os.path.join(a.app, 'engine', 'ReShade.log')
    if os.path.exists(log):
        with open(log, encoding='utf-8', errors='replace') as f:
            for line in f:
                if 'Glasir screenshot' in line:
                    print('  ' + line.rstrip())
    if not a.keep:
        for f in new:
            os.remove(os.path.join(shots, f))
    ok = ok and glasir.returncode == 0
    print('PASS' if ok else 'FAIL')
    return 0 if ok else 1


if __name__ == '__main__':
    sys.exit(main())

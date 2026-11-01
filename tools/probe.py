# SPDX-License-Identifier: MIT
"""Runs Glasir.exe with the given arguments in a 1280x720 window, asks mpv what
it is playing (size, format, colour, frames shown per second), closes it and
prints Glasir's log. For checking a real card or the DirectShow fallback.

    python tools/probe.py                                   # glasir.ini as it is
    python tools/probe.py --set video.mode=hdr
    python tools/probe.py --set capture.path=directshow
"""
import json
import os
import subprocess
import sys
import time

APP = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'out', 'Glasir')
PIPE = r'\\.\pipe\glasir'


def connect(timeout=20, process=None):
    deadline = time.time() + timeout
    while True:
        try:
            return open(PIPE, 'r+b', buffering=0)
        except OSError:
            if time.time() > deadline or (process and process.poll() is not None):
                return None
            time.sleep(0.1)


def request(pipe, command, request_id):
    pipe.write((json.dumps({'command': command, 'request_id': request_id}) + '\n').encode())
    buf = b''
    while True:
        chunk = pipe.read(4096)
        if not chunk:
            return None
        buf += chunk
        while b'\n' in buf:
            line, buf = buf.split(b'\n', 1)
            msg = json.loads(line)
            if msg.get('request_id') == request_id:
                return msg.get('data')


def main():
    args = ['--no-dialogs', '--set', 'window.mode=windowed'] + sys.argv[1:]
    glasir = subprocess.Popen([os.path.join(APP, 'Glasir.exe')] + args)
    pipe = connect(process=glasir)
    if pipe:
        try:
            time.sleep(3)
            n0, t0 = request(pipe, ['get_property', 'estimated-frame-number'], 1) or 0, time.time()
            time.sleep(3)
            n1, t1 = request(pipe, ['get_property', 'estimated-frame-number'], 2) or 0, time.time()
            out = request(pipe, ['get_property', 'video-out-params'], 3) or {}
            path = request(pipe, ['get_property', 'path'], 4)
            print(f"mpv: {path}: {out.get('w')}x{out.get('h')} {out.get('pixelformat')} {out.get('colormatrix')} "
                  f"{out.get('colorlevels')} {out.get('gamma')}, {(n1 - n0) / (t1 - t0):.1f} fps shown")
            request(pipe, ['quit'], 5)
        except (OSError, ValueError, TypeError) as e:
            print('mpv:', e)
        pipe.close()
    else:
        print('mpv: no IPC (Glasir ended or mpv did not start)')
    try:
        glasir.wait(15)
    except subprocess.TimeoutExpired:
        glasir.kill()
        print('Glasir did not exit')
    print('Glasir exit code', glasir.returncode)
    with open(os.path.join(APP, 'logs', 'glasir.log'), encoding='utf-8-sig', errors='replace') as f:
        for line in f:
            print('  ' + (line.rstrip()[:160] + ' ...' if 'started mpv' in line else line.rstrip()))


if __name__ == '__main__':
    main()

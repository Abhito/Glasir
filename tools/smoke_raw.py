# SPDX-License-Identifier: MIT
"""End-to-end check without a capture card.

Pipes ffmpeg's test pattern into Glasir.exe --input-raw (the same path a card's
frames take), asks mpv over Glasir's IPC pipe what it is playing, then closes
it and prints Glasir's log. Opens a 1280x720 window for a few seconds.

    python tools/smoke_raw.py                      # NV12 1280x720, 30 fps game in 60
    python tools/smoke_raw.py --format p010 --hdr
    python tools/smoke_raw.py --format yuy2 --game-fps 60
"""
import argparse
import json
import os
import subprocess
import sys
import time

PIPE = r'\\.\pipe\glasir'
FFMPEG_FORMAT = {'nv12': 'nv12', 'p010': 'p010le', 'yuy2': 'yuyv422', 'uyvy': 'uyvy422'}
MPV_FORMAT = {'nv12': 'nv12', 'p010': 'p010', 'yuy2': 'yuyv422', 'uyvy': 'uyvy422'}


def connect(timeout):
    deadline = time.time() + timeout
    while True:
        try:
            return open(PIPE, 'r+b', buffering=0)
        except OSError:
            if time.time() > deadline:
                raise
            time.sleep(0.1)


def request(pipe, command, request_id):
    pipe.write((json.dumps({'command': command, 'request_id': request_id}) + '\n').encode())
    buf = b''
    while True:
        buf += pipe.read(4096)
        while b'\n' in buf:
            line, buf = buf.split(b'\n', 1)
            msg = json.loads(line)
            if msg.get('request_id') == request_id:
                return msg


def main():
    p = argparse.ArgumentParser()
    p.add_argument('--app', default=os.path.join(os.path.dirname(__file__), '..', 'out', 'Glasir'))
    p.add_argument('--format', default='nv12', choices=sorted(FFMPEG_FORMAT))
    p.add_argument('--size', default='1280x720')
    p.add_argument('--fps', type=float, default=60, help='capture rate')
    p.add_argument('--game-fps', type=float, default=30, help='new frames per second (repeats fill up to --fps)')
    p.add_argument('--hdr', action='store_true')
    p.add_argument('--seconds', type=float, default=5)
    a = p.parse_args()
    width, height = (int(v) for v in a.size.split('x'))

    ffmpeg = subprocess.Popen(
        ['ffmpeg', '-hide_banner', '-loglevel', 'fatal', '-re', '-f', 'lavfi',
         '-i', f'testsrc2=size={a.size}:rate={a.game_fps:g}',
         '-vf', f'fps={a.fps:g},format={FFMPEG_FORMAT[a.format]}', '-f', 'rawvideo', '-'],
        stdout=subprocess.PIPE)
    glasir = subprocess.Popen(
        [os.path.join(a.app, 'Glasir.exe'), '--no-dialogs', '--input-raw', '-', '--raw-format', a.format,
         '--set', f'capture.width={width}', '--set', f'capture.height={height}', '--set', f'capture.fps={a.fps:g}',
         '--set', 'window.mode=windowed', '--set', f'video.mode={"hdr" if a.hdr else "sdr"}'],
        stdin=ffmpeg.stdout)
    ffmpeg.stdout.close()

    ok = True
    try:
        pipe = connect(15)
        time.sleep(2)  # past mpv's start
        # Frames mpv shows per second of wall time. (estimated-vf-fps reads the
        # raw stream's timestamps, which count every frame at the capture rate.)
        n0, t0 = request(pipe, ['get_property', 'estimated-frame-number'], 1).get('data') or 0, time.time()
        time.sleep(max(a.seconds - 2, 1))
        n1, t1 = request(pipe, ['get_property', 'estimated-frame-number'], 2).get('data') or 0, time.time()
        fps = (n1 - n0) / (t1 - t0) if n1 > n0 else None
        # The format as decoded; the colour tags after Glasir's format filter
        # (mpv may convert packed formats like yuyv422 on the way out).
        decoded = request(pipe, ['get_property', 'video-params'], 6).get('data') or {}
        params = request(pipe, ['get_property', 'video-out-params'], 3).get('data') or {}
        drops = request(pipe, ['get_property', 'frame-drop-count'], 5).get('data')
        print(f"mpv: {params.get('w')}x{params.get('h')} {decoded.get('pixelformat')} "
              f"{params.get('colormatrix')} {params.get('colorlevels')} {params.get('gamma')}, "
              f"{fps:.1f} fps shown, {drops} dropped" if fps else f'mpv: no video ({params})')
        want_fps = min(a.game_fps, a.fps)  # dedup passes only new frames
        ok = (params.get('w') == width and params.get('h') == height and
              decoded.get('pixelformat') == MPV_FORMAT[a.format] and fps is not None and
              abs(fps - want_fps) < want_fps * 0.15 and params.get('gamma') == ('pq' if a.hdr else 'bt.1886'))
        try:
            request(pipe, ['quit'], 4)
        except (OSError, ValueError):
            pass  # mpv may close the pipe before answering
        pipe.close()
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

    log = os.path.join(a.app, 'logs', 'glasir.log')
    if os.path.exists(log):
        with open(log, encoding='utf-8-sig', errors='replace') as f:
            for line in f:
                if 'started mpv' not in line:
                    print('  ' + line.rstrip())
    print(f'Glasir exit code {glasir.returncode}')
    print('PASS' if ok and glasir.returncode == 0 else 'FAIL')
    return 0 if ok and glasir.returncode == 0 else 1


if __name__ == '__main__':
    sys.exit(main())

# SPDX-License-Identifier: MIT
"""Checks that the picture goes from SDR to HDR and back while playing, without a capture card.

Pipes a 10-bit (P010) test pattern into Glasir.exe --input-raw that looks like
an SDR signal for the first --each seconds, like an HDR one for the next, and
SDR again (strong colours and white, then dim and pale: what signal_kind.h
tells them apart by). Over Glasir's IPC pipe it follows what mpv shows the
frames as, and checks that it changes when the signal does, in the same mpv
(no restart). Opens a 1280x720 window for about half a minute.

    python tools/picture_check.py                 # video.mode = auto: SDR, HDR, SDR
    python tools/picture_check.py --mode hdr      # HDR with RTX Video HDR for the SDR parts (NVIDIA RTX)
    python tools/picture_check.py --mode hdr --no-rtx-hdr   # everything shown as HDR
    python tools/picture_check.py --menu          # the menu's Picture choices instead, on an SDR pattern
    python tools/picture_check.py --set video.scaling=vsr --window 2560x1440   # with the scaling filter

What is checked is mpv's side (the tags, the filters, target-colorspace-hint).
The swap chain's colour space is in engine\\ReShade.log ("colour space" lines
of the frame generation add-on), printed at the end.
"""
import argparse
import json
import os
import subprocess
import sys
import time

PIPE = r'\\.\pipe\glasir'


def connect(timeout):
    deadline = time.time() + timeout
    while True:
        try:
            return open(PIPE, 'r+b', buffering=0)
        except OSError:
            if time.time() > deadline:
                raise
            time.sleep(0.1)


class Mpv:
    def __init__(self, pipe):
        self.pipe = pipe
        self.rid = 0
        self.buf = b''

    def request(self, *command):
        self.rid += 1
        self.pipe.write((json.dumps({'command': list(command), 'request_id': self.rid}) + '\n').encode())
        while True:
            while b'\n' in self.buf:
                line, self.buf = self.buf.split(b'\n', 1)
                msg = json.loads(line)
                if msg.get('request_id') == self.rid:
                    return msg
            self.buf += self.pipe.read(4096)

    def get(self, name):
        return self.request('get_property', name).get('data')

    def shown(self):
        """What mpv shows the frames as: the tags after the filters, the output, the filters (their labels)."""
        vf = self.get('vf') or []
        rtx = any('true-hdr' in k for f in vf for k in (f.get('params') or {}))
        return (self.get('video-out-params/gamma'), self.get('target-colorspace-hint'),
                self.get('user-data/glasir/signal') or '-', self.get('user-data/glasir/rtx-hdr') or '-',
                ' '.join(f.get('label') or f.get('name') for f in vf) + (' (RTX Video HDR)' if rtx else ''))


def main():
    p = argparse.ArgumentParser()
    p.add_argument('--app', default=os.path.join(os.path.dirname(__file__), '..', 'out', 'Glasir'))
    p.add_argument('--mode', default='auto', choices=['auto', 'hdr'])
    p.add_argument('--no-rtx-hdr', action='store_true', help='video.rtx_hdr = 0')
    p.add_argument('--menu', action='store_true', help="send the menu's Picture choices (the pattern stays SDR)")
    p.add_argument('--each', type=float, default=10, help='seconds of each kind of signal')
    p.add_argument('--size', default='1280x720')
    p.add_argument('--window', default='', help="mpv's window size (as it comes: 1280x720)")
    p.add_argument('--set', action='append', default=[], help='another glasir.ini value, e.g. video.scaling=vsr')
    a = p.parse_args()
    width, height = (int(v) for v in a.size.split('x'))

    # 10-bit codes: SDR as the pattern comes; "HDR" with white at 675 and the
    # colours a third as far from grey.
    pale = ("lutyuv=y='val*0.65+64':u='(val-512)*0.3+512':v='(val-512)*0.3+512'"
            f":enable='between(mod(t,{2 * a.each:g}),{a.each:g},{2 * a.each:g})'")
    filters = 'fps=60,format=yuv420p10le' + ('' if a.menu else ',' + pale) + ',format=p010le'
    ffmpeg = subprocess.Popen(
        ['ffmpeg', '-hide_banner', '-loglevel', 'fatal', '-re', '-f', 'lavfi', '-i', f'testsrc2=size={a.size}:rate=30',
         '-vf', filters, '-f', 'rawvideo', '-'], stdout=subprocess.PIPE)
    sets = [f'capture.width={width}', f'capture.height={height}', 'capture.fps=60', 'window.mode=windowed',
            f'video.mode={a.mode}', 'video.auto_found=sdr', f'video.rtx_hdr={0 if a.no_rtx_hdr else 1}'] + a.set
    command = [os.path.join(a.app, 'Glasir.exe'), '--no-dialogs', '--input-raw', '-', '--raw-format', 'p010']
    for s in sets:
        command += ['--set', s]
    env = dict(os.environ, GLASIR_SIGNAL_LOG='1')
    glasir = subprocess.Popen(command, stdin=ffmpeg.stdout, env=env)
    ffmpeg.stdout.close()

    changes = []  # (seconds, what mpv shows)
    ok = True
    try:
        mpv = Mpv(connect(15))
        if a.window:
            mpv.request('set_property', 'geometry', a.window)
        start = time.time()
        pid = mpv.get('pid')
        asked = []
        if a.menu:
            # What the menu sends (script-message), a few seconds apart.
            asked = [(4, ['glasir-picture', 'hdr']), (8, ['glasir-rtx-hdr', 'off']), (12, ['glasir-rtx-hdr', 'on']),
                     (16, ['glasir-picture', 'sdr']), (20, ['glasir-picture', 'auto'])]
        length = 24 if a.menu else a.each * 2.6
        while time.time() - start < length:
            t = time.time() - start
            while asked and t >= asked[0][0]:
                print(f'{t:5.1f} s  menu: {" ".join(asked[0][1])}')
                mpv.request('script-message', *asked.pop(0)[1])
            now = mpv.shown()
            if now[0] and (not changes or changes[-1][1] != now):
                changes.append((t, now))
                print(f'{t:5.1f} s  gamma {now[0]}, target-colorspace-hint {"yes" if now[1] in (True, "yes") else "no"}, signal {now[2]}, '
                      f'rtx-hdr {now[3]}; filters: {now[4]}')
            time.sleep(0.2)
        same_mpv = mpv.get('pid') == pid
        try:
            mpv.request('quit')
        except (OSError, ValueError):
            pass  # mpv may close the pipe before answering
        mpv.pipe.close()

        # The kinds of picture in order: sdr, hdr, or rtx (SDR in, HDR out).
        def kind(s):
            gamma, hint, _, _, filters = s
            hint = hint in (True, 'yes')  # mpv answers with a flag
            if 'RTX Video HDR' in filters and gamma == 'pq' and hint:
                return 'rtx'
            return 'hdr' if gamma == 'pq' and hint else 'sdr' if gamma != 'pq' and not hint else '?'
        seen = []
        for _, s in changes:
            k = kind(s)
            if k != '?' and (not seen or seen[-1] != k):
                seen.append(k)
        rtx = a.mode == 'hdr' and not a.no_rtx_hdr
        if a.menu:
            first = ['rtx'] if rtx else ['hdr'] if a.mode == 'hdr' else ['sdr']
            # HDR (an SDR pattern: through RTX Video HDR), the switch off (all HDR), on, SDR, Automatic (SDR).
            want = first + (['rtx', 'hdr', 'rtx', 'sdr'] if not a.no_rtx_hdr else ['hdr', 'sdr'])
            want = [k for i, k in enumerate(want) if i == 0 or want[i - 1] != k]
        elif a.mode == 'auto':
            want = ['sdr', 'hdr', 'sdr']
        else:
            want = ['rtx', 'hdr', 'rtx'] if rtx else ['hdr']
        print(f'shown as: {" -> ".join(seen)}; expected: {" -> ".join(want)}; '
              f'{"the same mpv throughout" if same_mpv else "mpv was started again"}')
        ok = seen == want and same_mpv
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

    for name, words in (('logs\\glasir.log', ('picture:', 'signal: it is', 'starting the picture')),
                        ('engine\\ReShade.log', ('colour space',))):
        path = os.path.join(a.app, name)
        if os.path.exists(path):
            with open(path, encoding='utf-8-sig', errors='replace') as f:
                for line in f:
                    if any(w in line for w in words):
                        print('  ' + line.rstrip())
    print(f'Glasir exit code {glasir.returncode}')
    print('PASS' if ok and glasir.returncode == 0 else 'FAIL')
    return 0 if ok and glasir.returncode == 0 else 1


if __name__ == '__main__':
    sys.exit(main())

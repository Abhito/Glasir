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
    python tools/scaling_check.py --target 2160 --size 1920x1080 --window 2560x1440   # "Scale to": 4K, then down

--vsr changes what glasir_scaling.lua was told about the GPU (normally from
Glasir's GPU check); a VSR the GPU does not have must fall back to FSR 1.
--nvidia also tries scaling = nvidia (NVIDIA's Video Super Resolution, run by
Glasir's add-on): where it runs, mpv draws the picture as large as it comes
(video-unscaled) with no shader and says so (user-data/glasir/sr = on); where
it cannot (not downloaded, another GPU, no ReShade), FSR 1 scales in mpv.
Either is a pass; the line says which. With --target the add-on has it make
that size and takes it to the window (engine\ReShade.log has the sizes: the
tool prints the add-on's lines).
--target is glasir.ini's video.scale_to: the shaders must be the copies
sized for that height (sized_<w>x<h>_<name>), the VSR filter's output that
tall, and mpv scales from there to the window. It is also switched while
playing (script-message glasir-target, as the menu does): to the screen and
back.
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
    p.add_argument('--target', type=int, default=0, help='video.scale_to: a height in lines (0: the screen)')
    p.add_argument('--nvidia', action='store_true', help="also scaling = nvidia (NVIDIA's Video Super Resolution)")
    a = p.parse_args()
    width, height = (int(v) for v in a.size.split('x'))
    # What glasir_scaling.lua makes of the target: an even height, the width to match.
    out_h = int(height * min(a.target / height, 4) / 2) * 2 if a.target else 0
    out_w = int(width * (out_h / height) / 2 + 0.5) * 2 if a.target else 0
    scaled = bool(a.target) and out_h > height * 1.001  # else the capture is already that tall

    def names(mode, target=True):
        prefix = f'sized_{out_w}x{out_h}_' if a.target and target else ''
        return [prefix + s for s in SHADERS[mode]]

    ffmpeg = subprocess.Popen(
        ['ffmpeg', '-hide_banner', '-loglevel', 'fatal', '-re', '-f', 'lavfi',
         '-i', f'testsrc2=size={a.size}:rate=60', '-vf', f'format={FFMPEG_FORMAT[a.format]}',
         '-f', 'rawvideo', '-'],
        stdout=subprocess.PIPE)
    glasir = subprocess.Popen(
        [os.path.join(a.app, 'Glasir.exe'), '--no-dialogs', '--input-raw', '-', '--raw-format', a.format,
         '--set', f'capture.width={width}', '--set', f'capture.height={height}', '--set', 'capture.fps=60',
         '--set', 'window.mode=windowed', '--set', f'video.mode={"hdr" if a.hdr else "sdr"}', '--set', 'video.rtx_hdr=0',
         '--set', 'video.scaling=mpv', '--set', f'video.scale_to={a.target or "screen"}', '--set', 'debug.mpv_log=1'],
        stdin=ffmpeg.stdout)
    ffmpeg.stdout.close()

    ok = True
    try:
        mpv = Mpv(15)
        time.sleep(2)
        if a.window != '1280x720':
            mpv('set_property', 'geometry', a.window)
            time.sleep(2)
        if a.vsr:
            mpv('change-list', 'script-opts', 'append', f'glasir_scaling-vsr={a.vsr}')
            time.sleep(0.5)
        dims = mpv.get('osd-dimensions') or {}
        print(f"capture {a.size} {a.format}{' HDR' if a.hdr else ''} in a {dims.get('w')}x{dims.get('h')} window" +
              (f', scaled to {out_w}x{out_h} first' if scaled else ', which is already as tall as the target' if a.target else ''))
        if a.target and mpv.get('user-data/glasir/target') != str(a.target):
            print(f"  FAIL the script's target is {mpv.get('user-data/glasir/target')}, not {a.target}")
            ok = False

        def check(mode, target=True):
            """What is in place for this mode; target: with --target's size (else by the window)."""
            shaders = [os.path.basename(s) for s in (mpv.get('glsl-shaders') or [])]
            vf = [f.get('label') for f in (mpv.get('vf') or []) if f.get('label')]
            shown = mpv.get('user-data/glasir/scaling')
            total, big = gpu_ms(mpv.get('vo-passes'), a.passes)
            vsr_on = 'glasir-vsr' in vf
            fixed = a.target and target
            # Nothing to do where the picture is as large as what it is scaled to: the target, or the window.
            now = mpv.get('osd-dimensions') or {}  # the window as it is now
            larger = scaled if fixed else min((now.get('w') or 0) / width, (now.get('h') or 0) / height) > 1.001
            want = [] if not larger else names(mode, target) if mode != 'vsr' else None
            if mode == 'vsr':
                # Either the GPU's VSR runs, or FSR 1 stands in for it.
                good = (vsr_on and shaders == []) or (not vsr_on and shaders == names('fsr', target))
                if not larger:
                    good = not vsr_on and shaders == []
                what = 'VSR filter' if vsr_on else 'FSR 1 instead of VSR' if shaders else 'nothing to do'
                if vsr_on and fixed:
                    # The filter's output is the target's size.
                    out = mpv.get('video-out-params') or {}
                    good = good and out.get('h') == out_h
                    what += f" to {out.get('w')}x{out.get('h')}"
            else:
                good = shaders == want and not vsr_on
                what = f'{len(shaders)} shaders' if len(shaders) > 2 else ', '.join(shaders) or 'no shaders'
            good = good and shown == mode
            print(f"  {mode:8s} {'ok  ' if good else 'FAIL'} {what}; {total:.2f} ms GPU per frame")
            for b in big:
                print(f'             {b}')
            return good

        for mode in MODES:
            mpv('script-message', 'glasir-scaling', mode)
            time.sleep(a.seconds)
            # The timings since the switch only: the passes reset when the shaders change.
            ok = check(mode) and ok
        if a.nvidia:
            # The add-on scales (mpv draws the picture as it comes), or FSR 1 stands in for it.
            mpv('script-message', 'glasir-scaling', 'nvidia')
            time.sleep(max(a.seconds, 6))  # its model is loaded at the first frame
            shaders = [os.path.basename(s) for s in (mpv.get('glsl-shaders') or [])]
            own = mpv.get('video-unscaled') in (True, 'yes') and mpv.get('user-data/glasir/sr') == 'on'
            stand_in = names('fsr') if scaled or not a.target else []  # FSR 1, sized for the target
            good = mpv.get('user-data/glasir/scaling') == 'nvidia' and (
                (own and shaders == []) or
                (not own and mpv.get('video-unscaled') in (False, 'no') and shaders == stand_in))
            print(f"  nvidia   {'ok  ' if good else 'FAIL'} "
                  f"{'the add-on scales: mpv draws the picture as it comes' if own else 'FSR 1 instead (it cannot run here)'}")
            ok = good and ok
            mpv('script-message', 'glasir-scaling', 'mpv')
            time.sleep(1.5)
            back = mpv.get('video-unscaled') in (False, 'no') and mpv.get('user-data/glasir/sr') == 'off'
            print(f"  mpv      {'ok  ' if back else 'FAIL'} mpv scales again")
            ok = back and ok
        if a.target:
            # As the menu's "Scale to" switches it: to the screen and back, with FSR 1.
            mpv('script-message', 'glasir-scaling', 'fsr')
            mpv('script-message', 'glasir-target', '0')
            time.sleep(a.seconds)
            print('  to the screen:')
            ok = (mpv.get('user-data/glasir/target') == '0') and check('fsr', target=False) and ok
            mpv('script-message', 'glasir-target', str(a.target))
            time.sleep(a.seconds)
            print(f'  to {a.target} again:')
            ok = (mpv.get('user-data/glasir/target') == str(a.target)) and check('fsr') and ok
            print(f"  mpv's own scaler from there: dscale={mpv.get('dscale')}, scale={mpv.get('scale')}, "
                  f"correct-downscaling={mpv.get('correct-downscaling')}, linear-downscaling={mpv.get('linear-downscaling')}")
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
    log = os.path.join(a.app, 'engine', 'ReShade.log')
    if a.nvidia and os.path.exists(log):
        with open(log, encoding='utf-8', errors='replace') as f:
            for line in f:
                if 'NVIDIA Video Super Resolution' in line:
                    print('  ' + line.rstrip().split(' | ')[-1])
    # mpv must end as cleanly as it ran: Glasir's own exit code does not say.
    log = os.path.join(a.app, 'logs', 'glasir.log')
    if os.path.exists(log):
        with open(log, encoding='utf-8-sig', errors='replace') as f:
            for line in f:
                if 'mpv exited (code ' in line and 'mpv exited (code 0)' not in line:
                    print('  FAIL ' + line.rstrip().split(' ', 1)[-1])
                    ok = False
    print('PASS' if ok and glasir.returncode == 0 else 'FAIL')
    return 0 if ok and glasir.returncode == 0 else 1


if __name__ == '__main__':
    sys.exit(main())

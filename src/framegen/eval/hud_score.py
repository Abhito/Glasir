# SPDX-License-Identifier: MPL-2.0
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
# Copyright (c) 2026 Abhito
"""HUD score on ADJACENT generated frames: what stands still on the screen while the scene moves.

The pixels that are the same in all five real frames of a capture and have detail around them (icons,
digits, a minimap's frame) should be the same in every frame drawn between. Per capture: the mask's size,
how much the scene moves, and for each version its mean error on the mask (8-bit levels) over the four
pairs and the share of mask pixels more than 24 levels off. Captures with a small mask are the ones with
a HUD over the scenery; a large one is a HUD with its own opaque panels, which any version keeps.

It does not see a copy of the HUD carried off with the scenery (that is beside the mask): look at the
frames for that.

Usage: hud_score.py DIR[,DIR...] [MIN_MOTION]
  DIR = an fg_eval --adjacent output folder (<capture>_motion_adjNM.png; whole frames, or the picture
        alone as --picture writes them back into the frame).
  MIN_MOTION = captures moving less than this are left out (mean level change between frames; 4).
The captures are looked for in FG_CAP (several folders: separate with ';') or out/Glasir/Captures;
the picture is taken to be x = 440-3000 of a 3440-wide frame, all of any other.
"""
import os
import sys

import numpy as np
from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
CAPS = (os.environ.get('FG_CAP') or os.path.join(HERE, '..', '..', '..', 'out', 'Glasir', 'Captures')).split(';')
STILL = 6.0    # a pixel stands still: its level moves less than this over the five frames
DETAIL = 40.0  # ... and has detail: the mean frame varies by this within 9x9 pixels
FAR = 24.0


def spread(a, size, fn):
    """A size x size maximum or minimum filter, edges repeated."""
    r = size // 2
    for axis in (0, 1):
        p = np.pad(a, [(r, r) if k == axis else (0, 0) for k in (0, 1)], mode='edge')
        a = fn([np.take(p, range(o, o + a.shape[axis]), axis=axis) for o in range(size)], axis=0)
    return a


def load(path):
    a = np.asarray(Image.open(path).convert('RGB')).astype(np.float32)
    return a[:, 440:3000] if a.shape[1] == 3440 else a


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 2
    dirs = sys.argv[1].split(',')
    min_motion = float(sys.argv[2]) if len(sys.argv) > 2 else 4.0
    names = sorted(set(n.split('_')[0] for n in os.listdir(dirs[0]) if '_motion_adj' in n))
    print('%-22s %7s %6s  %s' % ('capture', 'mask px', 'moves', '   '.join('%-14s' % os.path.basename(d.rstrip('/\\')) for d in dirs)))
    total = np.zeros((len(dirs), 2))
    weight = 0
    for cap in names:
        folder = next((os.path.join(c, cap) for c in CAPS if os.path.isdir(os.path.join(c, cap))), None)
        if folder is None or not all(os.path.isfile(os.path.join(d, '%s_motion_adj%d%d.png' % (cap, i, i + 1)))
                                     for d in dirs for i in range(1, 5)):
            continue
        real = np.stack([load(os.path.join(folder, 'f0%d.png' % i)) for i in range(1, 6)])
        luma = real.mean(axis=3)
        mean = luma.mean(axis=0)
        mask = ((luma.max(axis=0) - luma.min(axis=0)) < STILL) & ((spread(mean, 9, np.max) - spread(mean, 9, np.min)) > DETAIL)
        moves = float(np.abs(luma[1:] - luma[:-1]).mean())
        n = int(mask.sum())
        if n < 500 or moves < min_motion:
            continue
        want = real.mean(axis=0)[mask]
        cells = []
        for k, d in enumerate(dirs):
            err = far = 0.0
            for i in range(1, 5):
                e = np.abs(load(os.path.join(d, '%s_motion_adj%d%d.png' % (cap, i, i + 1)))[mask] - want).mean(axis=1)
                err += e.mean() / 4
                far += (e > FAR).mean() / 4
            total[k] += (err * n, far * n)
            cells.append('%5.2f %5.2f%%' % (err, 100 * far))
        weight += n
        print('%-22s %7d %6.1f  %s' % (cap, n, moves, '   '.join('%-14s' % c for c in cells)))
    if weight:
        print('%-22s %7d %6s  %s' % ('all (by mask size)', weight, '',
                                     '   '.join('%-14s' % ('%5.2f %5.2f%%' % (t[0] / weight, 100 * t[1] / weight)) for t in total)))
    return 0


if __name__ == '__main__':
    sys.exit(main())

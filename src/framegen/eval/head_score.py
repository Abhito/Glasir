# SPDX-License-Identifier: MPL-2.0
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
# Copyright (c) 2026 Abhito
"""Head-region PSNR of skip-one frames (fg_eval --out without --adjacent: <clip>_motion.png = f01+f03 -> f02,
<clip>_motion_2.png = f03+f05 -> f04) against the real f02/f04, inside the head ROIs of head_rois.json.

Usage: head_score.py OUTDIR [OUTDIR2 ...]   (several dirs -> one column each)
Captures are read from Glasir's Captures folder (out/Glasir/Captures) (or FG_CAP); ROIs whose capture
was deleted are listed as missing and left out of the mean.
"""
import sys, os, json, math
import numpy as np
from PIL import Image

root = os.environ.get('FG_CAP') or os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..', '..', 'out', 'Glasir', 'Captures')
root = root.rstrip('/\\') + '/'
rois = json.load(open(os.path.join(os.path.dirname(os.path.abspath(__file__)), 'head_rois.json')))
S = 240


def load(p):
    return np.asarray(Image.open(p).convert('RGB')).astype(np.float64) / 255.0


def psnr(a, b):
    m = ((a - b) ** 2).mean()
    return 99.0 if m <= 0 else 10 * math.log10(1.0 / m)


dirs = sys.argv[1:]
cols = {d: [] for d in dirs}
print('%-22s' % 'capture/pair' + ''.join('%14s' % os.path.basename(d.rstrip('/\\'))[:13] for d in dirs))
for c, r in rois.items():
    if not os.path.isdir(root + c):
        print('%-22s missing' % c)
        continue
    for name, truth, key in ((c + '_motion.png', 'f02', 'f02'), (c + '_motion_2.png', 'f04', 'f04')):
        x, y = r[key]
        t = load(root + c + '/' + truth + '.png')[y - S // 2:y + S // 2, x - S // 2:x + S // 2]
        row = '%-22s' % (c[-10:] + ' ' + truth)
        for d in dirs:
            p = os.path.join(d, name)
            if not os.path.exists(p):
                row += '%14s' % '-'
                continue
            g = load(p)[y - S // 2:y + S // 2, x - S // 2:x + S // 2]
            v = psnr(g, t)
            cols[d].append(v)
            row += '%14.2f' % v
        print(row)
print('%-22s' % 'MEAN' + ''.join('%14.3f' % (sum(v) / len(v) if v else 0) for v in cols.values()))

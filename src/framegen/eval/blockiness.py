# SPDX-License-Identifier: MPL-2.0
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
# Copyright (c) 2026 Abhito
"""Blockiness of frames: how much more the picture jumps across the 16-pixel
cell edges of the motion at the frame in between than between any other two
neighbouring pixels (1.0 = no more; a frame built from mismatched cells shows
more). Also, for two versions of the same frame, where they differ most.

    python blockiness.py FRAME.png [FRAME.png ...]
    python blockiness.py --diff A.png B.png [N]   # the N 64x64 tiles that differ most

8- and 16-bit PNGs; only the 2560 pixels in the middle of a 3440-wide frame
(the 16:9 picture between the black bars) are counted.
"""
import sys

import numpy as np
from PIL import Image

CELL = 16


def load(path):
    img = Image.open(path)
    a = np.asarray(img).astype(np.float64)
    if a.dtype != np.float64 or a.max() > 255:
        pass
    scale = 65535.0 if np.asarray(img).dtype == np.uint16 or a.max() > 255 else 255.0
    a = a[..., :3] / scale
    if a.shape[1] == 3440:
        a = a[:, 440:3000]
    return a, 440 if np.asarray(img).shape[1] == 3440 else 0


def luma(a):
    return a @ np.array([0.2126, 0.7152, 0.0722])


def blockiness(path):
    a, offset = load(path)
    y = luma(a)
    gx = np.abs(np.diff(y, axis=1))  # between column x and x+1
    gy = np.abs(np.diff(y, axis=0))
    # Cell edges in frame coordinates are at multiples of 16; gx[:, x] lies
    # between frame columns offset+x and offset+x+1.
    cols = np.arange(gx.shape[1]) + offset + 1
    rows = np.arange(gy.shape[0]) + 1
    edge_x = (cols % CELL) == 0
    edge_y = (rows % CELL) == 0
    ex = gx[:, edge_x].mean() / max(gx[:, ~edge_x].mean(), 1e-9)
    ey = gy[edge_y, :].mean() / max(gy[~edge_y, :].mean(), 1e-9)
    return 0.5 * (ex + ey)


def diff(pa, pb, n):
    a, offset = load(pa)
    b, _ = load(pb)
    d = np.abs(luma(a) - luma(b))
    h, w = d.shape
    t = 64
    tiles = d[: h // t * t, : w // t * t].reshape(h // t, t, w // t, t).mean(axis=(1, 3))
    order = np.argsort(tiles, axis=None)[::-1][:n]
    for k in order:
        ty, tx = divmod(int(k), tiles.shape[1])
        print(f"tile x={tx * t + offset} y={ty * t} mean |diff| {tiles[ty, tx]:.4f}")


if __name__ == "__main__":
    if sys.argv[1] == "--diff":
        diff(sys.argv[2], sys.argv[3], int(sys.argv[4]) if len(sys.argv) > 4 else 8)
    else:
        for p in sys.argv[1:]:
            print(f"{blockiness(p):.3f}  {p}")

# SPDX-License-Identifier: MPL-2.0
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
# Copyright (c) 2026 Abhito
"""Needle (thin hair) score on ADJACENT generated frames - what the user sees.

For each capture and pair fN->fM: tracks the head in the real frames, finds it in the generated frame,
and measures the thin top of the hair (rows narrower than 8 px, counted from the top until the
silhouette reaches 20 px wide) in A, G and B, plus dark "ghost" pixels in G outside both real
silhouettes (after aligning the heads). Based on fg_research/r2_critic/measure.py.

Usage: needle_score.py DIR [DIR2 ...]
  DIR = an fg_eval --adjacent output folder (<capture>_motion_adjNM.png), or the word 'live'
        (the add-on's own between_f0N_f0M.png in the capture folders).
Prints per pair: A/G/B thin px, ghost px; summary per DIR: pairs passing (G >= half of the
smaller real), mean min(1, G/minAB) on the failure set, total ghost px; controls listed separately.
"""
import os, sys, json
import numpy as np
from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
# Glasir's Captures folder (out/Glasir/Captures), or FG_CAP (e.g. a set that still has deleted captures).
CAP = (os.environ.get('FG_CAP') or os.path.join(HERE, '..', '..', '..', 'out', 'Glasir', 'Captures')).rstrip('/\\')
ROIS = json.load(open(os.path.join(HERE, 'head_rois.json')))
FAIL = ['20261028-210747-526', '20261028-210759-326', '20261028-210851-361', '20261028-210904-761']
CONTROL = ['20261028-210811-627', '20261028-210824-361']  # no needle loss today; must not grow fake spikes
ORBIT = ['20261028-012438-666', '20261028-012445-734']   # ghost outline cases
CENTRES = os.path.join(HERE, 'needle_centres.json')


def luma(path):
    rgb = np.asarray(Image.open(path))[:, :, :3].astype(np.float32)
    return 0.2126 * rgb[:, :, 0] + 0.7152 * rgb[:, :, 1] + 0.0722 * rgb[:, :, 2]


def track(y, ref, cx, cy, rad=30, win=40):
    h, w = y.shape
    x0, y0 = int(cx), int(cy)
    best, bx, by = 1e18, x0, y0
    for step in (4, 1):
        for dy in range(-rad, rad + 1, step):
            for dx in range(-rad, rad + 1, step):
                xx, yy = x0 + dx, y0 + dy
                if yy - win < 0 or xx - win < 0 or yy + win >= h or xx + win >= w:
                    continue
                sad = np.abs(y[yy - win:yy + win, xx - win:xx + win] - ref).mean()
                if sad < best:
                    best, bx, by = sad, xx, yy
        x0, y0, rad = bx, by, max(step, 2)
    return bx, by


def hair_mask(y, cx, cy, floor=None, box_x=150, box_up=130, box_dn=70):
    crop = y[cy - box_up:cy + box_dn, cx - box_x:cx + box_x]
    if floor is None:
        floor = np.percentile(crop[:40, :], 60)
    hair = crop < (floor - 30)
    p = np.pad(hair, 1)
    hair = hair & (p[1:-1, 2:] | p[1:-1, :-2] | p[2:, 1:-1] | p[:-2, 1:-1])
    return hair, floor


def thin_px(hair):
    rows = hair.sum(axis=1)
    idx = np.where(rows >= 1)[0]
    if len(idx) == 0:
        return 0
    total = 0
    for r in range(idx[0], len(rows)):
        if rows[r] >= 20:
            break
        if rows[r] < 8:
            total += int(rows[r])
    return total


def dilate(m, r):
    o = m.copy()
    for dy in range(-r, r + 1):
        for dx in range(-r, r + 1):
            if dx * dx + dy * dy <= r * r:
                o |= np.roll(np.roll(m, dy, 0), dx, 1)
    return o


def centres_for(name):
    cache = json.load(open(CENTRES)) if os.path.exists(CENTRES) else {}
    if name in cache:
        return {int(k): tuple(v) for k, v in cache[name].items()}
    fr = {i: luma(f'{CAP}/{name}/f0{i}.png') for i in range(1, 6)}
    cx, cy = ROIS[name]['f02']
    win = 40
    c = {2: (cx, cy)}
    ref = fr[2][cy - win:cy + win, cx - win:cx + win]
    prev = ref
    for i in (3, 4, 5):
        px, py = c[i - 1]
        c[i] = track(fr[i], prev, px, py)
        nx, ny = c[i]
        prev = fr[i][ny - win:ny + win, nx - win:nx + win]
    c[1] = track(fr[1], ref, cx, cy)
    cache[name] = {str(k): [int(v[0]), int(v[1])] for k, v in c.items()}
    json.dump(cache, open(CENTRES, 'w'), indent=1)
    return c


def gen_path(d, name, i):
    if d == 'live':
        return f'{CAP}/{name}/between_f0{i}_f0{i + 1}.png'
    return os.path.join(d, f'{name}_motion_adj{i}{i + 1}.png')


def score(d, name):
    c = centres_for(name)
    out = []
    for i in range(1, 5):
        p = gen_path(d, name, i)
        if not os.path.exists(p):
            continue
        ya, yb, yg = luma(f'{CAP}/{name}/f0{i}.png'), luma(f'{CAP}/{name}/f0{i + 1}.png'), luma(p)
        (ax, ay), (bx, by) = c[i], c[i + 1]
        win = 40
        gx, gy = track(yg, ya[ay - win:ay + win, ax - win:ax + win], (ax + bx) // 2, (ay + by) // 2, rad=16)
        ha, floor = hair_mask(ya, ax, ay)
        hb, _ = hair_mask(yb, bx, by, floor)
        hg, _ = hair_mask(yg, gx, gy, floor)
        ta, tb, tg = thin_px(ha), thin_px(hb), thin_px(hg)
        # ghost: dark in G outside both real silhouettes (dilated 3 px), upper part of the box
        up = slice(0, 140)
        ghost = int((hg & ~dilate(ha, 3) & ~dilate(hb, 3))[up].sum())
        out.append((i, ta, tg, tb, ghost))
    return out


def main():
    dirs = sys.argv[1:]
    summary = {d: {'pass': 0, 'n': 0, 'ratio': [], 'ghost': 0, 'ghost_orbit': 0, 'ctrl_extra': 0} for d in dirs}
    for name in FAIL + CONTROL + ORBIT:
        if not os.path.isdir(f'{CAP}/{name}'):
            print(f'== {name}: missing from {CAP}, skipped')
            continue
        print(f'== {name}')
        for d in dirs:
            for i, ta, tg, tb, gh in score(d, name):
                s = summary[d]
                mn = min(ta, tb)
                tag = ''
                if name in FAIL and mn >= 10:
                    ok = tg >= 0.5 * mn
                    s['pass'] += ok
                    s['n'] += 1
                    s['ratio'].append(min(1.0, tg / mn))
                    tag = 'PASS' if ok else 'fail'
                if name in FAIL:
                    s['ghost'] += gh
                if name in ORBIT:
                    s['ghost_orbit'] += gh
                if name in CONTROL:
                    s['ctrl_extra'] += max(0, tg - max(ta, tb))
                print(f'  {os.path.basename(d.rstrip("/"))[:14]:14s} p{i}{i + 1}  thin A {ta:3d}  G {tg:3d}  B {tb:3d}  ghost {gh:4d}  {tag}')
    print('== SUMMARY')
    for d, s in summary.items():
        r = sum(s['ratio']) / len(s['ratio']) if s['ratio'] else 0
        print(f'{d}: needle pass {s["pass"]}/{s["n"]} | needle ratio {r:.2f} | ghost(fail set) {s["ghost"]} | ghost(orbit) {s["ghost_orbit"]} | control extra thin px {s["ctrl_extra"]}')


if __name__ == '__main__':
    main()

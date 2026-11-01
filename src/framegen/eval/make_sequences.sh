#!/usr/bin/env bash
# SPDX-License-Identifier: MPL-2.0
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
# Copyright (c) 2026 Abhito
# Builds the test clips for fg_eval: short image sequences with known motion,
# made from game screenshots, rendered at twice the rate frame generation
# works at. Odd files (f01, f03, ...) are the real frames; even files (f02,
# f04, ...) are the true in-between frames a generated frame is scored against.
#
#   make_sequences.sh [SCREENSHOT_DIR]
#
# Writes eval/seq/<name>/f01.png ... f09.png (9 frames = 4 scored pairs).
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
ffmpeg="$here/../../ffmpeg.exe"
shots="${1:-$here/../../DLSS5 Screenshots}"
src="$here/src"
seq="$here/seq"
mkdir -p "$src" "$seq"

# The game picture is 2560x1440 in the middle of the 3440x1440 screenshots.
town="$src/town.png"
menu="$src/menu.png"
"$ffmpeg" -hide_banner -loglevel error -y -i "$shots/NR_CAPTURE_1791865662984_1.png" -vf "crop=2560:1440:440:0,format=rgb24" "$town"
"$ffmpeg" -hide_banner -loglevel error -y -i "$shots/NR_CAPTURE_1792303819340_1.png" -vf "crop=2560:1440:440:0,format=rgb24" "$menu"

clip() {  # name, inputs..., -- filter
    local name="$1"; shift
    local inputs=()
    while [ "$1" != "--" ]; do inputs+=(-loop 1 -i "$1"); shift; done
    shift
    rm -rf "$seq/$name"; mkdir -p "$seq/$name"
    "$ffmpeg" -hide_banner -loglevel error -y "${inputs[@]}" -filter_complex "$1,format=rgb24" -frames:v 9 "$seq/$name/f%02d.png"
    echo "  $name"
}

echo "clips (per step = half a real frame):"
# Camera pans over the town at three speeds (6, 20, 48 px per step).
clip pan_slow "$town" -- "[0]crop=1280:720:x='200+n*6':y=300"
clip pan_med  "$town" -- "[0]crop=1280:720:x='200+n*20':y=300"
clip pan_fast "$town" -- "[0]crop=1280:720:x='200+n*48':y=300"
# Diagonal pan.
clip pan_diag "$town" -- "[0]crop=1280:720:x='300+n*16':y='200+n*10'"
# Zoom in by 1.2% per step around the middle.
clip zoom "$town" -- "[0]scale=w='trunc(2560*(1+n*0.012)/2)*2':h=-2:eval=frame,crop=1280:720:(iw-1280)/2:(ih-720)/2"
# A sprite (the character) crossing a slowly panning background: occlusion.
clip sprite "$town" "$town" -- "[0]crop=1280:720:x='200+n*6':y=300[bg];[1]crop=300:520:1330:780[sp];[bg][sp]overlay=x='900-n*28':y=150"
# Background pans while the HUD (minimap, bottom bar) stays put.
clip hud "$town" "$town" "$town" -- "[0]crop=1280:720:x='200+n*20':y=300[bg];[1]crop=360:380:2160:20,scale=180:190[map];[2]crop=1800:120:0:1320,scale=900:60[bar];[bg][map]overlay=x=1090:y=10[t];[t][bar]overlay=x=10:y=650"
# Text-heavy menu scrolling up slowly.
clip menu_scroll "$menu" -- "[0]crop=1280:720:x=500:y='300+n*8'"
# Full-resolution pan, for timing at the capture size.
clip pan_1440 "$town" -- "[0]scale=3584:2016,crop=2560:1440:x='200+n*20':y=300"
# Static subtitles (white text, black outline) over a pan.
clip subs "$town" -- "[0]crop=1280:720:x='200+n*20':y=300,drawtext=fontfile='C\:/Windows/Fonts/arialbd.ttf':text='Static subtitles stay sharp':fontsize=44:fontcolor=white:borderw=3:bordercolor=black:x=(w-tw)/2:y=600"
# Fade to black with the camera still (4% per step), and while panning (2% per step).
clip fade_static "$town" -- "[0]crop=1280:720:x=400:y=300,fade=t=out:s=0:n=25"
clip fade_pan "$town" -- "[0]crop=1280:720:x='200+n*20':y=300,fade=t=out:s=0:n=50"
# A hard cut from the town pan to the menu after the 5th frame (no true
# in-between frame exists there; look at it rather than score it).
clip cut "$town" "$menu" -- "[0]crop=1280:720:x='200+n*20':y=300[a];[1]crop=1280:720:x=500:y='300+n*8'[b];[a][b]overlay=enable='gte(n,5)'"
# UI that animates by itself: the minimap fading in over a pan (alpha 0 -> 1
# over 8 steps), and a menu panel sliding in over a still background.
clip ui_fade "$town" "$town" -- "[0]crop=1280:720:x='200+n*20':y=300[bg];[1]crop=360:380:2160:20,scale=270:285,format=rgba,fade=t=in:s=0:n=8:alpha=1[ui];[bg][ui]overlay=x=980:y=20"
clip ui_slide "$town" "$menu" -- "[0]crop=1280:720:x=600:y=300[bg];[1]crop=520:640:160:200[ui];[bg][ui]overlay=x='-520+n*60':y=40"
# A slow pan with a still HUD and fresh grain on every frame (like a neural
# pass that never draws the same pixel twice).
clip noisy_hud "$town" "$town" "$town" -- "[0]crop=1280:720:x='200+n*6':y=300[bg];[1]crop=360:380:2160:20,scale=180:190[map];[2]crop=1800:120:0:1320,scale=900:60[bar];[bg][map]overlay=x=1090:y=10[t];[t][bar]overlay=x=10:y=650,noise=alls=4:allf=t"
# Camera orbiting a character: the scenery sweeps past (30 px per step) while
# the character (an oval cut from the screenshot) stays put, or drifts a little.
clip orbit "$town" "$town" -- "[0]crop=1280:720:x='200+n*30':y=300[bg];[1]crop=560:900:880:540,scale=336:540,format=rgba,geq=r='r(X,Y)':g='g(X,Y)':b='b(X,Y)':a='255*lt(pow((X-W/2)/(W/2),2)+pow((Y-H/2)/(H/2),2),1)'[ch];[bg][ch]overlay=x=470:y=150"
clip orbit_drift "$town" "$town" -- "[0]crop=1280:720:x='200+n*30':y=300[bg];[1]crop=560:900:880:540,scale=336:540,format=rgba,geq=r='r(X,Y)':g='g(X,Y)':b='b(X,Y)':a='255*lt(pow((X-W/2)/(W/2),2)+pow((Y-H/2)/(H/2),2),1)'[ch];[bg][ch]overlay=x='470+n*3':y=150"

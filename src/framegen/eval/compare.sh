#!/usr/bin/env bash
# SPDX-License-Identifier: MPL-2.0
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
# Copyright (c) 2026 Abhito
# Puts crops of several PQ (HDR10) frames side by side, tone-mapped for an
# SDR screen, to compare frame generation variants by eye.
#
#   compare.sh OUT.png CROP SCALE IN1.png IN2.png ...
#
# CROP is ffmpeg's w:h:x:y in the frames' own pixels; SCALE the factor the
# crops are resized by (1 = as is).
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
ffmpeg="$here/../../ffmpeg.exe"
out="$1"; crop="$2"; scale="$3"; shift 3
inputs=()
filter=""
labels=""
i=0
for f in "$@"; do
    inputs+=(-i "$f")
    filter+="[$i]setparams=color_primaries=bt2020:color_trc=smpte2084:colorspace=bt2020nc,zscale=t=linear:npl=203,format=gbrpf32le,tonemap=hable:desat=0,zscale=p=bt709:t=bt709:m=bt709,format=rgb24,crop=$crop,scale=iw*$scale:-1[v$i];"
    labels+="[v$i]"
    i=$((i + 1))
done
"$ffmpeg" -hide_banner -loglevel error -y "${inputs[@]}" -filter_complex "${filter}${labels}hstack=$i" "$out"

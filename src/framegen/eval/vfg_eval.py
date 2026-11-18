# SPDX-License-Identifier: MPL-2.0
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
# Copyright (c) 2026 Abhito
"""vfg_eval - NVIDIA's Video Frame Generation on fg_eval's clips, scored the same way.

Makes the frame between two real frames with the Video Frame Generation filter of NVIDIA's Video Effects
SDK, which the user has on their PC (nothing of NVIDIA's is in this repository), and compares it with the
true one as fg_eval does: PSNR over RGB on 16-bit values, "repeat" (the earlier frame again) beside it.
The files it saves have fg_eval's names (<clip>_motion.png, <clip>_motion_adjNM.png), so that
needle_score.py, hud_score.py, head_score.py and blockiness.py read them as they are.

  python vfg_eval.py --runtime DIR [--mode low|medium|high] [--clip NAME] [--out DIR] [--adjacent]
                     [--picture X0 Y0 X1 Y1] [--device N] [--no-auto-cut] [--multiplier M] SEQ_DIR

--multiplier M (3 or 4): every Mth file is a real frame, the M - 1 between two of them are the true frames
for the M - 1 the filter makes between that pair (fg_eval's --multiplier).
--runtime: the folder with NVVideoEffects.dll, NVCVImage.dll and nvVFXVideoFrameGeneration.dll (or
GLASIR_VFG_RUNTIME). --picture, --adjacent, --clip, --out: as fg_eval's. 8-bit clips go to the filter as
RGBA 8-bit, 16-bit ones (HDR captures, PQ) as RGB10A2. Needs an RTX 40 or 50, numpy, Pillow and ffmpeg on
the PATH (it reads the 16-bit PNGs, which Pillow cuts to 8 bits).

NVIDIA's licence forbids giving its results (quality or speed) to others without NVIDIA's permission:
keep what this prints out of the repository, issues and bug reports.
"""
import argparse
import ctypes
import math
import os
import struct
import subprocess
import sys
import time

import numpy as np
from PIL import Image

MODES = {'low': 0, 'medium': 1, 'high': 2}
# NvCVImage's pixel formats and component types (nvCVImage.h, MIT; RGB10A2 and P32 were added after it).
NVCV_RGBA, NVCV_RGB10A2 = 6, 13
NVCV_U8, NVCV_P32 = 1, 11
NVCV_CHUNKY, NVCV_CPU, NVCV_GPU = 0, 0, 1
CU_COMPUTE_MAJOR, CU_COMPUTE_MINOR = 75, 76


def png_info(path):
    with open(path, 'rb') as f:
        head = f.read(26)
    w, h, depth = struct.unpack('>IIB', head[16:25])
    return w, h, depth


def load_png(path):
    """16 bits a channel (RGBA) whatever the file had, and the file's bit depth."""
    w, h, depth = png_info(path)
    fmt = 'rgba64le' if depth == 16 else 'rgba'
    raw = subprocess.run(['ffmpeg', '-v', 'error', '-i', path, '-f', 'rawvideo', '-pix_fmt', fmt, '-'],
                         capture_output=True, check=True).stdout
    if depth == 16:
        return np.frombuffer(raw, dtype='<u2').reshape(h, w, 4).copy(), 16
    return np.frombuffer(raw, dtype=np.uint8).reshape(h, w, 4).astype(np.uint16) * 257, 8


def save_png(path, img16):
    """An 8-bit preview, as fg_eval saves (the top 8 bits)."""
    Image.fromarray((img16[..., :3] >> 8).astype(np.uint8)).save(path, compress_level=1)


def psnr(a, b):
    d = a[..., :3].astype(np.float64) - b[..., :3].astype(np.float64)
    mse = float(np.mean(d * d))
    return math.inf if mse <= 0 else 10.0 * math.log10(65535.0 * 65535.0 / mse)


def pack(img16, depth):
    """The filter's pixels: RGBA 8-bit, or R10 G10 B10 A2 in 32 bits (red lowest) for 16-bit files."""
    if depth == 8:
        return np.ascontiguousarray((img16 // 257).astype(np.uint8))
    v = ((img16[..., :3].astype(np.uint32) * 1023 + 32767) // 65535)
    return np.ascontiguousarray(v[..., 0] | (v[..., 1] << 10) | (v[..., 2] << 20) | np.uint32(3 << 30))


def unpack(buf, depth, w, h):
    if depth == 8:
        out = np.frombuffer(buf, dtype=np.uint8).reshape(h, w, 4).astype(np.uint16) * 257
        out[..., 3] = 65535
        return out
    p = np.frombuffer(buf, dtype=np.uint32).reshape(h, w)
    out = np.empty((h, w, 4), dtype=np.uint16)
    for c in range(3):
        out[..., c] = (((p >> (10 * c)) & 1023) * 65535 + 511) // 1023
    out[..., 3] = 65535
    return out


class Vfg:
    """The filter on one CUDA device: make(a, b) gives the frame half-way between two packed frames."""

    def __init__(self, runtime, device, mode, auto_cut):
        self.mode, self.auto_cut = mode, auto_cut
        self.multiplier = 2
        self.size = None
        self.effect = ctypes.c_void_p()
        self.images, self.buffers = [], []
        cu = self.cu = ctypes.CDLL(os.path.join(os.environ['SystemRoot'], 'System32', 'nvcuda.dll'))
        P, U64, SZ, I = ctypes.c_void_p, ctypes.c_uint64, ctypes.c_size_t, ctypes.c_int
        for name, args in (('cuInit', [ctypes.c_uint]), ('cuDeviceGetCount', [P]), ('cuDeviceGet', [P, I]),
                           ('cuDeviceGetName', [ctypes.c_char_p, I, I]), ('cuDeviceGetAttribute', [P, I, I]),
                           ('cuDevicePrimaryCtxRetain', [P, I]), ('cuCtxPushCurrent_v2', [P]),
                           ('cuStreamCreate', [P, ctypes.c_uint]), ('cuStreamSynchronize', [P]),
                           ('cuMemAlloc_v2', [P, SZ]), ('cuMemFree_v2', [U64]),
                           ('cuMemcpyHtoD_v2', [U64, P, SZ]), ('cuMemcpyDtoH_v2', [P, U64, SZ])):
            fn = getattr(cu, name)
            fn.argtypes, fn.restype = args, I
        self.check_cu(cu.cuInit(0), 'cuInit')
        dev, ctx, stream = I(), P(), P()
        self.check_cu(cu.cuDeviceGet(ctypes.byref(dev), device), 'cuDeviceGet')
        name = ctypes.create_string_buffer(128)
        cu.cuDeviceGetName(name, 128, dev)
        major, minor = I(), I()
        cu.cuDeviceGetAttribute(ctypes.byref(major), CU_COMPUTE_MAJOR, dev)
        cu.cuDeviceGetAttribute(ctypes.byref(minor), CU_COMPUTE_MINOR, dev)
        self.gpu = '%s (compute %d.%d)' % (name.value.decode(), major.value, minor.value)
        self.check_cu(cu.cuDevicePrimaryCtxRetain(ctypes.byref(ctx), dev), 'cuDevicePrimaryCtxRetain')
        self.check_cu(cu.cuCtxPushCurrent_v2(ctx), 'cuCtxPushCurrent')
        self.check_cu(cu.cuStreamCreate(ctypes.byref(stream), 0), 'cuStreamCreate')
        self.stream = stream

        os.add_dll_directory(runtime)
        libs = {}
        for dll in ('cudart64_12.dll', 'NVCVImage.dll', 'nvngxruntime.dll', 'NVVideoEffects.dll',
                    'nvVFXVideoFrameGeneration.dll'):
            libs[dll] = ctypes.CDLL(os.path.join(runtime, dll))
        cv, fx = self.cv, self.fx = libs['NVCVImage.dll'], libs['NVVideoEffects.dll']
        U = ctypes.c_uint
        cv.NvCVImage_Create.argtypes = [U, U, I, I, U, U, U, P]
        cv.NvCVImage_Dealloc.argtypes = [P]
        cv.NvCVImage_Dealloc.restype = None
        cv.NvCVImage_Destroy.argtypes = [P]
        cv.NvCVImage_Destroy.restype = None
        cv.NvCVImage_Init.argtypes = [P, U, U, I, P, I, I, U, U]
        cv.NvCV_GetErrorStringFromCode.argtypes = [I]
        cv.NvCV_GetErrorStringFromCode.restype = ctypes.c_char_p
        fx.NvVFX_CreateEffect.argtypes = [ctypes.c_char_p, P]
        fx.NvVFX_DestroyEffect.argtypes = [P]
        fx.NvVFX_DestroyEffect.restype = None
        fx.NvVFX_SetU32.argtypes = [P, ctypes.c_char_p, U]
        fx.NvVFX_SetImage.argtypes = [P, ctypes.c_char_p, P]
        fx.NvVFX_SetCudaStream.argtypes = [P, ctypes.c_char_p, P]
        fx.NvVFX_Load.argtypes = [P]
        fx.NvVFX_Run.argtypes = [P, I]
        fx.NvVFX_GetVersion.argtypes = [P]
        version = U()
        self.check(fx.NvVFX_GetVersion(ctypes.byref(version)), 'NvVFX_GetVersion')
        self.version = '%d.%d.%d' % (version.value >> 24, (version.value >> 16) & 255, (version.value >> 8) & 255)

    def check_cu(self, code, what):
        if code:
            raise RuntimeError('%s: CUDA error %d' % (what, code))

    def check(self, code, what):
        if code:
            raise RuntimeError('%s: %s (%d)' % (what, self.cv.NvCV_GetErrorStringFromCode(code).decode(), code))

    def release(self):
        if self.effect:
            self.fx.NvVFX_DestroyEffect(self.effect)
            self.effect = ctypes.c_void_p()
        for image in self.images:
            self.cv.NvCVImage_Destroy(image)
        for buffer in self.buffers:
            self.cu.cuMemFree_v2(buffer)
        self.images, self.buffers = [], []

    def create(self, w, h, depth):
        """The filter for frames of this size and bit depth (its model is loaded for one size)."""
        if self.size == (w, h, depth):
            return
        self.release()
        self.size, self.bytes = (w, h, depth), w * h * 4
        fmt, kind = (NVCV_RGBA, NVCV_U8) if depth == 8 else (NVCV_RGB10A2, NVCV_P32)
        for _ in range(3):  # the earlier frame, the later one, the frame between
            buffer, image = ctypes.c_uint64(), ctypes.c_void_p()
            self.check_cu(self.cu.cuMemAlloc_v2(ctypes.byref(buffer), self.bytes), 'cuMemAlloc')
            # A descriptor from the SDK, then our CUDA memory in place of the pixels it came with.
            self.check(self.cv.NvCVImage_Create(1, 1, fmt, kind, NVCV_CHUNKY, NVCV_CPU, 1, ctypes.byref(image)),
                       'NvCVImage_Create')
            self.cv.NvCVImage_Dealloc(image)
            self.check(self.cv.NvCVImage_Init(image, w, h, w * 4, buffer.value, fmt, kind, NVCV_CHUNKY, NVCV_GPU),
                       'NvCVImage_Init')
            self.buffers.append(buffer.value)
            self.images.append(image)
        fx, e = self.fx, ctypes.c_void_p()
        self.check(fx.NvVFX_CreateEffect(b'VideoFrameGeneration', ctypes.byref(e)), 'NvVFX_CreateEffect')
        self.effect = e
        self.check(fx.NvVFX_SetCudaStream(e, b'CudaStream', self.stream), 'CudaStream')
        self.check(fx.NvVFX_SetU32(e, b'InputWidth', w), 'InputWidth')
        self.check(fx.NvVFX_SetU32(e, b'InputHeight', h), 'InputHeight')
        self.check(fx.NvVFX_SetU32(e, b'Mode', self.mode), 'Mode')
        self.check(fx.NvVFX_SetU32(e, b'AutomaticShotChangeDetectionEnabled', 1 if self.auto_cut else 0),
                   'AutomaticShotChangeDetectionEnabled')
        self.check(fx.NvVFX_SetImage(e, b'SrcImage0', self.images[0]), 'SrcImage0')
        self.check(fx.NvVFX_SetImage(e, b'SrcImage1', self.images[1]), 'SrcImage1')
        self.check(fx.NvVFX_Load(e), 'NvVFX_Load')
        self.check(fx.NvVFX_SetImage(e, b'DstImage0', self.images[2]), 'DstImage0')
        self.check(fx.NvVFX_SetU32(e, b'FrameMultiplier', 2), 'FrameMultiplier')
        self.multiplier = 2

    def make(self, a, b, index=1, multiplier=2):
        """Frame `index` of the `multiplier - 1` between a and b (packed pixels; half-way as it comes), and the
        milliseconds one run took. Every call is a new pair to the filter."""
        cu, fx, e = self.cu, self.fx, self.effect
        self.check_cu(cu.cuMemcpyHtoD_v2(self.buffers[0], a.ctypes.data, self.bytes), 'upload')
        self.check_cu(cu.cuMemcpyHtoD_v2(self.buffers[1], b.ctypes.data, self.bytes), 'upload')
        ms = 0.0
        for run in range(2):  # the first run of a pair untimed
            # Binding the inputs again tells the filter that this is a new pair.
            self.check(fx.NvVFX_SetImage(e, b'SrcImage0', self.images[0]), 'SrcImage0')
            self.check(fx.NvVFX_SetImage(e, b'SrcImage1', self.images[1]), 'SrcImage1')
            self.check(fx.NvVFX_SetU32(e, b'ShotChange', 0), 'ShotChange')
            if multiplier != self.multiplier:  # setting it clears the index
                self.check(fx.NvVFX_SetU32(e, b'FrameMultiplier', multiplier), 'FrameMultiplier')
                self.multiplier = multiplier
            self.check(fx.NvVFX_SetImage(e, b'DstImage0', self.images[2]), 'DstImage0')
            self.check(fx.NvVFX_SetU32(e, b'FrameIndex', index), 'FrameIndex')
            self.check_cu(cu.cuStreamSynchronize(self.stream), 'cuStreamSynchronize')
            start = time.perf_counter()
            self.check(fx.NvVFX_Run(e, 0), 'NvVFX_Run')
            self.check_cu(cu.cuStreamSynchronize(self.stream), 'cuStreamSynchronize')
            ms = (time.perf_counter() - start) * 1000.0
        out = ctypes.create_string_buffer(self.bytes)
        self.check_cu(cu.cuMemcpyDtoH_v2(out, self.buffers[2], self.bytes), 'download')
        return out, ms


def main():
    ap = argparse.ArgumentParser(description="NVIDIA's Video Frame Generation on fg_eval's clips")
    ap.add_argument('seq_dir')
    ap.add_argument('--runtime', default=os.environ.get('GLASIR_VFG_RUNTIME'))
    ap.add_argument('--mode', choices=sorted(MODES), default='medium')
    ap.add_argument('--clip')
    ap.add_argument('--out')
    ap.add_argument('--adjacent', action='store_true')
    ap.add_argument('--picture', type=int, nargs=4, metavar=('X0', 'Y0', 'X1', 'Y1'))
    ap.add_argument('--device', type=int, default=0)
    ap.add_argument('--no-auto-cut', action='store_true', help="the filter's own cut detection off")
    ap.add_argument('--multiplier', type=int, default=2, choices=range(2, 9), help='real frames this many files apart')
    args = ap.parse_args()
    if not args.runtime or not os.path.isfile(os.path.join(args.runtime, 'NVVideoEffects.dll')):
        sys.exit('--runtime DIR (or GLASIR_VFG_RUNTIME): the folder with NVVideoEffects.dll')
    if args.out:
        os.makedirs(args.out, exist_ok=True)

    vfg = Vfg(os.path.abspath(args.runtime), args.device, MODES[args.mode], not args.no_auto_cut)
    print('GPU: %s; Video Effects SDK %s, mode %s' % (vfg.gpu, vfg.version, args.mode))
    print('\n%-20s  %10s dB  %10s dB %8s' % ('clip', 'repeat', 'vfg', 'ms'))
    clips = sorted(d for d in os.listdir(args.seq_dir)
                   if os.path.isdir(os.path.join(args.seq_dir, d)) and (not args.clip or d == args.clip))
    totals, count = [0.0, 0.0], 0
    for clip in clips:
        frames, depth = [], 8
        for i in range(1, 100):
            path = os.path.join(args.seq_dir, clip, 'f%02d.png' % i)
            if not os.path.isfile(path):
                break
            img, depth = load_png(path)
            frames.append(img)
        step = 1 if args.adjacent else args.multiplier
        between = 1 if args.adjacent else args.multiplier - 1
        if len(frames) < step + 1:
            continue
        h, w = frames[0].shape[:2]
        x0, y0, x1, y1 = args.picture if args.picture and args.picture[2] <= w and args.picture[3] <= h else (0, 0, w, h)
        try:
            vfg.create(x1 - x0, y1 - y0, depth)
        except RuntimeError as e:
            print('%-20s  %s' % (clip, e))
            continue
        packed = [pack(f[y0:y1, x0:x1], depth) for f in frames]
        sums, n, times = [0.0, 0.0], [0, 0], []
        for i, k in ((i, k) for i in range(0, len(frames) - step, step) for k in range(1, between + 1)):
            truth = frames[i if args.adjacent else i + k]
            raw, ms = vfg.make(packed[i], packed[i + step], k, between + 1)
            times.append(ms)
            out = frames[i].copy()  # outside the picture: as the earlier frame has it
            out[y0:y1, x0:x1] = unpack(raw, depth, x1 - x0, y1 - y0)
            for k, made in enumerate((frames[i], out)):
                p = psnr(made, truth)
                if not math.isinf(p):
                    sums[k] += p
                    n[k] += 1
            if args.out:
                name = clip + '_motion'  # fg_eval's names, for the score scripts
                if args.adjacent:
                    name += '_adj%d%d' % (i + 1, i + 2)
                elif i > 0:
                    name += '_%d' % (i // step + 1)
                if between > 1:
                    name += '_%dof%d' % (k, between + 1)
                save_png(os.path.join(args.out, name + '.png'), out)
        avg = [sums[k] / n[k] if n[k] else math.inf for k in range(2)]
        for k in range(2):
            totals[k] += 99.0 if math.isinf(avg[k]) else avg[k]
        count += 1
        print('%-20s  %10.2f dB  %10.2f dB %8.3f' % (clip, avg[0], avg[1], sorted(times)[len(times) // 2]), flush=True)
    print('%-20s  %10.2f dB  %10.2f dB' % ('mean', totals[0] / max(1, count), totals[1] / max(1, count)))
    vfg.release()


if __name__ == '__main__':
    main()

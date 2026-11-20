# SPDX-License-Identifier: MPL-2.0
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
# Copyright (c) 2026 Abhito
"""sr_eval - NVIDIA's Video Super Resolution on game frames: how close to the true frame.

A frame's picture is the source: a smaller version of it is the input, a larger one the truth (both made
with Lanczos, which favours Lanczos in the scores). The input is made as large as the truth by

  - ffmpeg's bicubic and Lanczos scalers, to have something to compare with;
  - the "VideoSuperRes" effect of NVIDIA's Video Effects SDK, which the user has on their PC (nothing of
    NVIDIA's is in this repository), each of its levels, run straight on CUDA memory;
  - with --exe, sr_eval.exe: the same effect through fg_sr.h, as Glasir's add-on runs it (Direct3D 11
    buffers shared with CUDA). It must give the picture the SDK gives without Direct3D.

  python sr_eval.py --runtime DIR [--exe build\\vs\\src\\framegen\\Release\\sr_eval.exe]
                    [--from 1280x720] [--to 1920x1080] [--crop X Y W H] [--device N] IMAGE [IMAGE ...]

--runtime: the folder with NVVideoEffects.dll, NVCVImage.dll, nvVFXVideoSuperRes.dll and nvngx_vsr.dll
(Glasir's engine\\nvidia-vfg, or GLASIR_VFG_RUNTIME). --crop: the picture's part of the frame (the
prototype's 3440x1440 captures: 440 0 2560 1440, taken by itself for that size). 8-bit files go to the
effect as RGBA 8-bit, 16-bit ones (HDR captures, PQ) as R10G10B10A2. Needs an NVIDIA RTX GPU, numpy and
ffmpeg on the PATH. --device: CUDA's number of the GPU (0 need not be the one with the screen).

NVIDIA's licence forbids giving its results (quality or speed) to others without NVIDIA's permission:
keep what this prints out of the repository, issues and bug reports.
"""
import argparse
import ctypes
import os
import struct
import subprocess
import sys
import tempfile
import time

import numpy as np

# The effect's QualityLevel (fg_sr.h has the ones Glasir offers: the high-bitrate ones).
LEVELS = [(0, 'bicubic'), (1, 'low'), (2, 'medium'), (3, 'high'), (4, 'ultra'),
          (16, 'high-bitrate low'), (17, 'high-bitrate medium'), (18, 'high-bitrate high'), (19, 'high-bitrate ultra')]
GLASIR = {16: 'low', 17: 'medium', 18: 'high', 19: 'ultra'}  # sr_eval.exe's --level names
# NvCVImage's pixel formats and component types (nvCVImage.h, MIT; RGB10A2 and P32 were added after it).
NVCV_RGBA, NVCV_RGB10A2 = 6, 13
NVCV_U8, NVCV_P32 = 1, 11
NVCV_CHUNKY, NVCV_CPU, NVCV_GPU = 0, 0, 1


def png_info(path):
    with open(path, 'rb') as f:
        head = f.read(26)
    return struct.unpack('>IIB', head[16:25])


def load_scaled(path, crop, size, deep):
    """The picture's part of a frame at this size (Lanczos): (h, w, 3), uint8 or uint16."""
    x, y, w, h = crop
    fmt = 'rgb48le' if deep else 'rgb24'
    vf = f'crop={w}:{h}:{x}:{y},scale={size[0]}:{size[1]}:flags=lanczos+accurate_rnd+full_chroma_int'
    raw = subprocess.run(['ffmpeg', '-v', 'error', '-i', path, '-vf', vf, '-f', 'rawvideo', '-pix_fmt', fmt, '-'],
                         capture_output=True, check=True).stdout
    return np.frombuffer(raw, dtype='<u2' if deep else np.uint8).reshape(size[1], size[0], 3).copy()


def rescale(img, size, deep, flags):
    h, w = img.shape[:2]
    fmt = 'rgb48le' if deep else 'rgb24'
    raw = subprocess.run(['ffmpeg', '-v', 'error', '-f', 'rawvideo', '-pix_fmt', fmt, '-s', f'{w}x{h}', '-i', '-',
                          '-vf', f'scale={size[0]}:{size[1]}:flags={flags}', '-f', 'rawvideo', '-pix_fmt', fmt, '-'],
                         input=img.tobytes(), capture_output=True, check=True).stdout
    return np.frombuffer(raw, dtype=img.dtype).reshape(size[1], size[0], 3).copy()


def pack(img, deep):
    """The effect's pixels: RGBA 8-bit, or R10 G10 B10 A2 in 32 bits (red lowest)."""
    h, w = img.shape[:2]
    if not deep:
        out = np.empty((h, w, 4), dtype=np.uint8)
        out[..., :3] = img
        out[..., 3] = 255
        return np.ascontiguousarray(out)
    v = (img.astype(np.uint32) * 1023 + 32767) // 65535
    return np.ascontiguousarray(v[..., 0] | (v[..., 1] << 10) | (v[..., 2] << 20) | np.uint32(3 << 30))


def unpack(buf, deep, w, h):
    if not deep:
        return np.frombuffer(buf, dtype=np.uint8).reshape(h, w, 4)[..., :3].copy()
    p = np.frombuffer(buf, dtype=np.uint32).reshape(h, w)
    out = np.empty((h, w, 3), dtype=np.uint16)
    for c in range(3):
        out[..., c] = (((p >> (10 * c)) & 1023) * 65535 + 511) // 1023
    return out


def luma(img):
    scale = 255.0 / (65535.0 if img.dtype == np.uint16 else 255.0)
    return (img.astype(np.float64) @ np.array([0.2126, 0.7152, 0.0722])) * scale


def box(a, n):
    c = np.cumsum(np.cumsum(np.pad(a, ((1, 0), (1, 0))), axis=0), axis=1)
    return (c[n:, n:] - c[:-n, n:] - c[n:, :-n] + c[:-n, :-n]) / (n * n)


def ssim(a, b, n=8):
    c1, c2 = (0.01 * 255) ** 2, (0.03 * 255) ** 2
    ma, mb = box(a, n), box(b, n)
    va, vb, cov = box(a * a, n) - ma * ma, box(b * b, n) - mb * mb, box(a * b, n) - ma * mb
    return float((((2 * ma * mb + c1) * (2 * cov + c2)) / ((ma * ma + mb * mb + c1) * (va + vb + c2))).mean())


def psnr(a, b):
    mse = float(((a - b) ** 2).mean())
    return 99.0 if mse == 0 else 10 * np.log10(255.0 ** 2 / mse)


class Sdk:
    """The effect on one CUDA device, on CUDA memory of its own."""

    def __init__(self, runtime, device):
        self.effect = ctypes.c_void_p()
        self.images, self.buffers, self.shape = [], [], None
        cu = self.cu = ctypes.CDLL(os.path.join(os.environ['SystemRoot'], 'System32', 'nvcuda.dll'))
        P, U64, SZ, I = ctypes.c_void_p, ctypes.c_uint64, ctypes.c_size_t, ctypes.c_int
        for name, args in (('cuInit', [ctypes.c_uint]), ('cuDeviceGet', [P, I]), ('cuDeviceGetName', [ctypes.c_char_p, I, I]),
                           ('cuDevicePrimaryCtxRetain', [P, I]), ('cuCtxPushCurrent_v2', [P]),
                           ('cuStreamCreate', [P, ctypes.c_uint]), ('cuStreamSynchronize', [P]),
                           ('cuMemAlloc_v2', [P, SZ]), ('cuMemFree_v2', [U64]),
                           ('cuMemcpyHtoD_v2', [U64, P, SZ]), ('cuMemcpyDtoH_v2', [P, U64, SZ])):
            fn = getattr(cu, name)
            fn.argtypes, fn.restype = args, I
        self.ok_cu(cu.cuInit(0), 'cuInit')
        dev, ctx, stream = I(), P(), P()
        self.ok_cu(cu.cuDeviceGet(ctypes.byref(dev), device), 'cuDeviceGet')
        name = ctypes.create_string_buffer(128)
        cu.cuDeviceGetName(name, 128, dev)
        self.gpu = name.value.decode()
        self.ok_cu(cu.cuDevicePrimaryCtxRetain(ctypes.byref(ctx), dev), 'cuDevicePrimaryCtxRetain')
        self.ok_cu(cu.cuCtxPushCurrent_v2(ctx), 'cuCtxPushCurrent')
        self.ok_cu(cu.cuStreamCreate(ctypes.byref(stream), 0), 'cuStreamCreate')
        self.stream = stream
        os.add_dll_directory(runtime)
        libs = {}
        for dll in ('cudart64_12.dll', 'NVCVImage.dll', 'nvngxruntime.dll', 'NVVideoEffects.dll', 'nvVFXVideoSuperRes.dll'):
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
        fx.NvVFX_SetF32.argtypes = [P, ctypes.c_char_p, ctypes.c_float]
        fx.NvVFX_SetImage.argtypes = [P, ctypes.c_char_p, P]
        fx.NvVFX_SetCudaStream.argtypes = [P, ctypes.c_char_p, P]
        fx.NvVFX_Load.argtypes = [P]
        fx.NvVFX_Run.argtypes = [P, I]

    def ok_cu(self, code, what):
        if code:
            raise RuntimeError('%s: CUDA error %d' % (what, code))

    def ok(self, code, what):
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
        self.images, self.buffers, self.shape = [], [], None

    def create(self, w, h, ow, oh, deep, level):
        """The effect for these sizes at this level (its model is loaded once for a size and bit depth:
        another level is taken as it runs)."""
        fx = self.fx
        if self.shape != (w, h, ow, oh, deep):
            self.release()
            self.shape = (w, h, ow, oh, deep)
            fmt, kind = (NVCV_RGB10A2, NVCV_P32) if deep else (NVCV_RGBA, NVCV_U8)
            for iw, ih in ((w, h), (ow, oh)):
                buffer, image = ctypes.c_uint64(), ctypes.c_void_p()
                self.ok_cu(self.cu.cuMemAlloc_v2(ctypes.byref(buffer), iw * ih * 4), 'cuMemAlloc')
                # A descriptor from the SDK, then our CUDA memory in place of the pixels it came with.
                self.ok(self.cv.NvCVImage_Create(1, 1, fmt, kind, NVCV_CHUNKY, NVCV_CPU, 1, ctypes.byref(image)), 'NvCVImage_Create')
                self.cv.NvCVImage_Dealloc(image)
                self.ok(self.cv.NvCVImage_Init(image, iw, ih, iw * 4, buffer.value, fmt, kind, NVCV_CHUNKY, NVCV_GPU), 'NvCVImage_Init')
                self.buffers.append(buffer.value)
                self.images.append(image)
            e = ctypes.c_void_p()
            self.ok(fx.NvVFX_CreateEffect(b'VideoSuperRes', ctypes.byref(e)), 'NvVFX_CreateEffect')
            self.effect = e
            self.ok(fx.NvVFX_SetCudaStream(e, b'CudaStream', self.stream), 'CudaStream')
            # Before the images: it turns down an image of the other encoding.
            self.ok(fx.NvVFX_SetU32(e, b'ImageEncodingMode', 1 if deep else 0), 'ImageEncodingMode')
            self.ok(fx.NvVFX_SetImage(e, b'SrcImage0', self.images[0]), 'SrcImage0')
            self.ok(fx.NvVFX_SetImage(e, b'DstImage0', self.images[1]), 'DstImage0')
            self.loaded = False
        e = self.effect
        self.ok(fx.NvVFX_SetU32(e, b'QualityLevel', level), 'QualityLevel')
        self.ok(fx.NvVFX_SetF32(e, b'Strength', 1.0), 'Strength')
        if not self.loaded:
            self.ok(fx.NvVFX_Load(e), 'NvVFX_Load')
            self.loaded = True

    def run(self, packed, runs=12):
        """The output's bytes, and the median milliseconds of a run once it is warm."""
        w, h, ow, oh, _ = self.shape
        cu, fx, e = self.cu, self.fx, self.effect
        self.ok_cu(cu.cuMemcpyHtoD_v2(self.buffers[0], packed.ctypes.data, w * h * 4), 'upload')
        times = []
        for i in range(runs + 8):
            self.ok_cu(cu.cuStreamSynchronize(self.stream), 'sync')
            start = time.perf_counter()
            self.ok(fx.NvVFX_Run(e, 0), 'NvVFX_Run')
            self.ok_cu(cu.cuStreamSynchronize(self.stream), 'sync')  # the run returns before its work is done
            if i >= 8:
                times.append((time.perf_counter() - start) * 1000.0)
        out = ctypes.create_string_buffer(ow * oh * 4)
        self.ok_cu(cu.cuMemcpyDtoH_v2(out, self.buffers[1], ow * oh * 4), 'download')
        return out, sorted(times)[len(times) // 2]


def through_exe(exe, runtime, packed, size, to, deep, level, work):
    """The same input through sr_eval.exe (fg_sr.h): the output's bytes."""
    src, dst = os.path.join(work, 'in.raw'), os.path.join(work, 'out.raw')
    packed.tofile(src)
    cmd = [exe, '--runtime', runtime, '--in', src, '--size', '%dx%d' % size, '--to', '%dx%d' % to, '--out', dst,
           '--level', level] + (['--deep'] if deep else [])
    done = subprocess.run(cmd, capture_output=True, text=True)
    if done.returncode != 0:
        raise RuntimeError((done.stderr or done.stdout).strip())
    with open(dst, 'rb') as f:
        return f.read()


def main():
    ap = argparse.ArgumentParser(description="NVIDIA's Video Super Resolution on game frames")
    ap.add_argument('images', nargs='+')
    ap.add_argument('--runtime', default=os.environ.get('GLASIR_VFG_RUNTIME'))
    ap.add_argument('--exe', help='sr_eval.exe: the effect through fg_sr.h as well, compared with the SDK alone')
    ap.add_argument('--from', dest='small', default='1280x720')
    ap.add_argument('--to', default='1920x1080')
    ap.add_argument('--crop', type=int, nargs=4, metavar=('X', 'Y', 'W', 'H'))
    ap.add_argument('--device', type=int, default=0)
    args = ap.parse_args()
    if not args.runtime or not os.path.isfile(os.path.join(args.runtime, 'nvVFXVideoSuperRes.dll')):
        sys.exit('--runtime DIR (or GLASIR_VFG_RUNTIME): the folder with nvVFXVideoSuperRes.dll')
    small = tuple(int(v) for v in args.small.split('x'))
    large = tuple(int(v) for v in args.to.split('x'))
    runtime = os.path.abspath(args.runtime)
    if args.exe:
        args.exe = os.path.abspath(args.exe)
    sdk = Sdk(runtime, args.device)
    print('GPU (CUDA device %d): %s' % (args.device, sdk.gpu))
    rows, times, same = {}, {}, {}
    with tempfile.TemporaryDirectory() as work:
        for path in args.images:
            w, h, depth = png_info(path)
            deep = depth == 16
            crop = args.crop or ((440, 0, 2560, 1440) if (w, h) == (3440, 1440) else (0, 0, w, h))
            truth, source = load_scaled(path, crop, large, deep), load_scaled(path, crop, small, deep)
            ref = luma(truth)
            kind = '10-bit' if deep else '8-bit'
            for name, flags in (('ffmpeg bicubic', 'bicubic'), ('ffmpeg lanczos', 'lanczos+accurate_rnd')):
                got = luma(rescale(source, large, deep, flags))
                rows.setdefault((kind, name), []).append((psnr(got, ref), ssim(got, ref)))
            packed = pack(source, deep)
            for level, name in LEVELS:
                try:
                    sdk.create(small[0], small[1], large[0], large[1], deep, level)
                    raw, ms = sdk.run(packed)
                except RuntimeError as e:
                    print('  %s, %s: %s' % (path, name, e))
                    sdk.release()
                    continue
                got = luma(unpack(raw, deep, large[0], large[1]))
                rows.setdefault((kind, name), []).append((psnr(got, ref), ssim(got, ref)))
                times.setdefault((kind, name), []).append(ms)
                if args.exe and level in GLASIR:
                    theirs = through_exe(args.exe, runtime, packed, small, large, deep, GLASIR[level], work)
                    ours = bytes(raw)[:len(theirs)]
                    a = luma(unpack(ours, deep, large[0], large[1]))
                    b = luma(unpack(theirs, deep, large[0], large[1]))
                    same.setdefault((kind, name), []).append(99.0 if ours == theirs else psnr(a, b))
            print('%s: %s, done' % (path, kind), flush=True)
    sdk.release()
    print('\n%s made %s, against the true frame: luma PSNR, SSIM, a run' % (args.small, args.to)
          + (', and fg_sr.h against the SDK alone (99: the same bytes)' if args.exe else ''))
    for (kind, name), v in rows.items():
        line = '  %-7s %-20s %6.2f dB  %.4f' % (kind, name, np.mean([p for p, _ in v]), np.mean([s for _, s in v]))
        if (kind, name) in times:
            line += '  %6.2f ms' % np.mean(times[(kind, name)])
        if (kind, name) in same:
            line += '  %6.2f dB' % min(same[(kind, name)])
        print(line)


if __name__ == '__main__':
    main()

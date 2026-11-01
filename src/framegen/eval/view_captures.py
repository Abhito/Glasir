# SPDX-License-Identifier: MPL-2.0
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
# Copyright (c) 2026 Abhito
"""Viewer for the frame-generation captures (the add-on's F9 key).

Shows each capture in Captures\\ in order - f01, the frame generated
between f01 and f02, f02, ... f05 - tone-mapped for an SDR screen the way
eval\\compare.sh does (16-bit captures are HDR10/PQ). The view stays put while
stepping, so the same spot can be compared frame to frame. Started by
View-FrameGen-Captures.bat; another folder can be given:

    pythonw view_captures.py [CAPTURES_DIR]

Keys: Left/Right frame (on into the next capture), Up/Down capture, Home/End,
Space play/pause, R real frames only, wheel or +/- zoom at the cursor, drag
to pan, F fit, 1 100 %, B black bars, O show the file in Explorer, Delete
move the capture to the Recycle Bin (Shift+Delete: delete it for good), F5
look for new captures. The status bar gives the frame pixel under the cursor.
"""

import math
import os
import queue
import re
import shutil
import subprocess
import sys
import time
import tkinter as tk
from concurrent.futures import ThreadPoolExecutor, wait
from tkinter import messagebox, ttk

try:
    from PIL import Image, ImageTk
except ImportError:  # in a box: pythonw has no console
    tk.Tk().withdraw()
    messagebox.showerror("Capture viewer", "Pillow is missing. Install it with:\n\npython -m pip install pillow")
    sys.exit(1)

HERE = os.path.dirname(os.path.abspath(__file__))
# The built app (Captures are saved there); ffmpeg from PATH.
APP_DIR = os.path.join(HERE, "..", "..", "..", "out", "Glasir")
FFMPEG = shutil.which("ffmpeg") or "ffmpeg"
# eval\compare.sh's tone mapping: PQ (BT.2020) to BT.709, Hable, 203 nits as white.
PQ_TO_SDR = (
    "setparams=color_primaries=bt2020:color_trc=smpte2084:colorspace=bt2020nc,"
    "zscale=t=linear:npl=203,format=gbrpf32le,tonemap=hable:desat=0,"
    "zscale=p=bt709:t=bt709:m=bt709,format=rgb24"
)
# Playback: at 1x each frame is on screen 1/60 s (a 30 fps game doubled);
# with real frames only, each real frame stays twice as long.
SPEEDS = [("1x (real time)", 1.0), ("1/2x", 0.5), ("1/4x", 0.25), ("1/10x", 0.1), ("1/30x", 1 / 30)]
FRAME_S = 1 / 60
KEEP_CAPTURES = 3  # decoded captures kept in memory (about 130 MB each)

BG, PANEL, REAL, GEN, DIM, SELECT = "#1e1e1e", "#2d2d2d", "#7cdb8a", "#ffa94d", "#666666", "#094771"


def png_info(path):
    """Width, height and bits per channel, from the PNG header."""
    with open(path, "rb") as f:
        head = f.read(26)
    if head[:8] != b"\x89PNG\r\n\x1a\n":
        raise ValueError(f"{os.path.basename(path)} is not a PNG")
    return int.from_bytes(head[16:20], "big"), int.from_bytes(head[20:24], "big"), head[24]


def decode(path):
    """The frame as 8-bit RGB for the screen, and the box inside its black bars."""
    w, h, depth = png_info(path)
    vf = PQ_TO_SDR if depth == 16 else "format=rgb24"
    out = subprocess.run(
        [FFMPEG, "-v", "error", "-i", path, "-vf", vf, "-f", "rawvideo", "-pix_fmt", "rgb24", "-"],
        capture_output=True, check=True, creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0)).stdout
    img = Image.frombytes("RGB", (w, h), out)
    # Black bars beside (or above and below) the picture: the box of all that
    # is not black, kept only where the margins match (a dark scene's edge is
    # not a bar).
    l, t, r, b = img.convert("L").point(lambda v: 255 if v > 8 else 0).getbbox() or (0, 0, w, h)
    if abs(l - (w - r)) > 8:
        l, r = 0, w
    if abs(t - (h - b)) > 8:
        t, b = 0, h
    return img, (l, t, r, b)


def frames_of(folder):
    """[(file, kind, label)] in viewing order: each real frame, then the one generated after it."""
    try:
        names = set(os.listdir(folder))
    except OSError:
        return []
    reals = sorted((n for n in names if re.fullmatch(r"f\d+\.png", n)), key=lambda n: int(n[1:-4]))
    seq = []
    for i, r in enumerate(reals):
        seq.append((r, "real", f"real frame {r[:-4]}"))
        if i + 1 < len(reals):
            nxt = reals[i + 1][:-4]
            between = f"between_{r[:-4]}_{nxt}.png"
            if between in names:
                seq.append((between, "generated", f"generated between {r[:-4]} and {nxt}"))
    return seq


def recycle(path, hwnd=None):
    """Moves a file or folder to the Recycle Bin, as Delete in Explorer does."""
    import ctypes
    from ctypes import wintypes

    class SHFILEOPSTRUCTW(ctypes.Structure):
        _fields_ = [("hwnd", wintypes.HWND), ("wFunc", wintypes.UINT), ("pFrom", ctypes.c_void_p),
                    ("pTo", ctypes.c_void_p), ("fFlags", wintypes.WORD), ("fAnyOperationsAborted", wintypes.BOOL),
                    ("hNameMappings", ctypes.c_void_p), ("lpszProgressTitle", ctypes.c_void_p)]

    path = os.path.abspath(path)
    names = ctypes.create_unicode_buffer(path, len(path) + 2)  # a list of names, ended by two nulls
    # Undo (that is, the Recycle Bin) without the shell's "are you sure" and progress
    # boxes, but with its warning when a file would be deleted for good instead (too
    # big for the bin, or a drive without one).
    flags = 0x40 | 0x10 | 0x4 | 0x400 | 0x4000  # FOF_ALLOWUNDO NOCONFIRMATION SILENT NOERRORUI WANTNUKEWARNING
    op = SHFILEOPSTRUCTW(hwnd=hwnd, wFunc=3, pFrom=ctypes.addressof(names), fFlags=flags)  # FO_DELETE
    err = ctypes.windll.shell32.SHFileOperationW(ctypes.byref(op))
    if err and not op.fAnyOperationsAborted:
        raise OSError(f"Windows could not move it to the Recycle Bin (error {err:#x}). "
                      "Is one of its files open in another program?")


def pretty(name):
    m = re.fullmatch(r"(\d{4})(\d\d)(\d\d)-(\d\d)(\d\d)(\d\d)(?:-\d+)?", name)
    return f"{m[1]}-{m[2]}-{m[3]}  {m[4]}:{m[5]}:{m[6]}" if m else name


class Viewer:
    def __init__(self, folder):
        self.folder = folder
        self.win = tk.Tk()
        self.win.title("Frame generation captures")
        self.win.configure(bg=BG)
        self.win.geometry("1600x950")
        try:
            self.win.state("zoomed")
        except tk.TclError:
            pass
        self.pool = ThreadPoolExecutor(max_workers=4)
        self.done = queue.Queue()  # (path, future) from the decoding threads
        self.futures = {}          # path -> future still decoding or queued
        self.images = {}           # path -> (Image, box)
        self.errors = {}           # path -> message
        self.boxes = {}            # capture folder -> the box inside its black bars
        self.held = []             # capture folders with decoded frames, oldest first
        self.captures = []
        self.cap = -1
        self.seq = []
        self.pos = 0
        self.frame_size = (1, 1)
        self.zoom = None           # display pixels per frame pixel; None = fit
        self.center = None         # the frame point in the middle of the view
        self.view = None           # (x0, y0, scale) of what is drawn, for the cursor
        self.cache = {}            # path -> (PhotoImage, canvas item) at the current view
        self.cache_key = None
        self.shown = None          # the canvas item on show
        self.cursor = ""           # the frame pixel under the mouse, for the status bar
        self.note = ("", 0.0)      # (message, until) shown in the status bar instead
        self.playing = False
        self.due = 0.0
        self.drag = None
        self.slots = []
        self.pending_render = False
        self.build_ui()
        self.refresh(first=True)
        self.win.protocol("WM_DELETE_WINDOW", self.close)
        self.win.after(30, self.poll)

    # ---- layout ---------------------------------------------------------------------

    def build_ui(self):
        style = ttk.Style(self.win)
        try:
            style.theme_use("clam")
        except tk.TclError:
            pass
        style.configure("TFrame", background=BG)
        style.configure("TLabel", background=BG, foreground="#dddddd")
        style.configure("TCheckbutton", background=BG, foreground="#dddddd")
        style.map("TCheckbutton", background=[("active", BG)])
        style.configure("TButton", padding=(8, 2))

        top = ttk.Frame(self.win)
        top.pack(side="top", fill="x", padx=6, pady=4)

        def button(text, command, width=None):
            b = ttk.Button(top, text=text, command=command, takefocus=False, width=width)
            b.pack(side="left", padx=2)
            return b

        button("◀", lambda: self.step(-1), 3)
        self.play_button = button("Play", self.toggle_play, 6)
        button("▶", lambda: self.step(1), 3)
        ttk.Label(top, text="Speed").pack(side="left", padx=(12, 4))
        self.speed = ttk.Combobox(top, state="readonly", width=14, values=[s[0] for s in SPEEDS], takefocus=False)
        self.speed.current(2)
        self.speed.pack(side="left")
        self.speed.bind("<<ComboboxSelected>>", lambda e: self.canvas.focus_set())
        self.real_only = tk.BooleanVar(value=False)
        ttk.Checkbutton(top, text="Real frames only (R)", variable=self.real_only, command=self.on_real_only,
                        takefocus=False).pack(side="left", padx=(12, 4))
        self.bars = tk.BooleanVar(value=False)
        ttk.Checkbutton(top, text="Black bars (B)", variable=self.bars, command=self.reset_view,
                        takefocus=False).pack(side="left", padx=4)
        button("Fit (F)", self.fit)
        button("100% (1)", lambda: self.set_zoom(1.0))
        button("Show file (O)", self.show_file)
        button("Delete (Del)", self.delete_capture)
        self.status = ttk.Label(top, text="")
        self.status.pack(side="right", padx=6)

        self.strip = ttk.Frame(self.win)
        self.strip.pack(side="bottom", fill="x", padx=6, pady=(2, 6))

        body = ttk.Frame(self.win)
        body.pack(side="top", fill="both", expand=True)
        left = ttk.Frame(body)
        left.pack(side="left", fill="y")
        ttk.Label(left, text="Captures (F5: look again)").pack(anchor="w", padx=6)
        self.listbox = tk.Listbox(left, width=24, bg="#252526", fg="#dddddd", selectbackground=SELECT,
                                  selectforeground="#ffffff", activestyle="none", exportselection=False,
                                  highlightthickness=0, borderwidth=0, font=("Consolas", 10))
        self.listbox.pack(fill="y", expand=True, padx=6, pady=4)
        self.listbox.bind("<<ListboxSelect>>", self.on_select)
        self.canvas = tk.Canvas(body, bg="#111111", highlightthickness=0, cursor="fleur")
        self.canvas.pack(side="left", fill="both", expand=True)

        c = self.canvas
        c.bind("<Configure>", lambda e: self.schedule_render())
        self.win.bind("<MouseWheel>", self.on_wheel)
        c.bind("<ButtonPress-1>", self.on_press)
        c.bind("<B1-Motion>", self.on_drag)
        c.bind("<Double-Button-1>", self.on_double)
        c.bind("<Motion>", self.on_motion)
        keys = {
            "<Left>": lambda e: self.step(-1), "<Right>": lambda e: self.step(1),
            "<Up>": lambda e: self.step_capture(-1), "<Down>": lambda e: self.step_capture(1),
            "<Prior>": lambda e: self.step_capture(-1), "<Next>": lambda e: self.step_capture(1),
            "<Home>": lambda e: self.go(self.visible()[0]) if self.visible() else None,
            "<End>": lambda e: self.go(self.visible()[-1]) if self.visible() else None,
            "<space>": lambda e: self.toggle_play(),
            "<r>": lambda e: (self.real_only.set(not self.real_only.get()), self.on_real_only()),
            "<b>": lambda e: (self.bars.set(not self.bars.get()), self.reset_view()),
            "<f>": lambda e: self.fit(), "<Key-1>": lambda e: self.set_zoom(1.0), "<KP_1>": lambda e: self.set_zoom(1.0),
            "<plus>": lambda e: self.zoom_by(1.25), "<equal>": lambda e: self.zoom_by(1.25),
            "<KP_Add>": lambda e: self.zoom_by(1.25), "<minus>": lambda e: self.zoom_by(0.8),
            "<KP_Subtract>": lambda e: self.zoom_by(0.8),
            "<o>": lambda e: self.show_file(), "<F5>": lambda e: self.refresh(),
            "<Delete>": lambda e: self.delete_capture(), "<Shift-Delete>": lambda e: self.delete_capture(True),
        }
        for key in [k for k in keys if re.fullmatch(r"<[a-z]>", k)]:
            keys[key.upper()] = keys[key]  # Caps Lock on
        for key, handler in keys.items():
            self.win.bind(key, lambda e, h=handler: (h(e), "break")[1])
        self.win.report_callback_exception = self.on_error
        self.canvas.focus_set()

    # ---- captures and frames ----------------------------------------------------------

    def refresh(self, first=False):
        current = self.captures[self.cap] if 0 <= self.cap < len(self.captures) else None
        try:
            names = sorted(d for d in os.listdir(self.folder)
                           if os.path.isdir(os.path.join(self.folder, d)) and frames_of(os.path.join(self.folder, d)))
        except OSError:
            names = []
        self.captures = names
        self.listbox.delete(0, "end")
        for n in names:
            self.listbox.insert("end", pretty(n))
        if not names:
            self.cap, self.seq = -1, []
            self.build_strip()
            self.render()
            return
        if current in names:
            self.cap = names.index(current)
            self.select_in_list()
        else:
            self.open_capture(len(names) - 1)  # the newest
        if not first:
            self.say(f"{len(names)} captures")

    def cap_dir(self, index=None):
        index = self.cap if index is None else index
        return os.path.join(self.folder, self.captures[index])

    def path_at(self, i):
        return os.path.join(self.cap_dir(), self.seq[i][0])

    def select_in_list(self):
        self.listbox.selection_clear(0, "end")
        self.listbox.selection_set(self.cap)
        self.listbox.see(self.cap)

    def on_select(self, _event):
        sel = self.listbox.curselection()
        if sel and sel[0] != self.cap:
            self.open_capture(sel[0])
        # The list takes the focus after this event; give it back to the view,
        # or Up/Down would move both the list and the capture.
        self.win.after_idle(self.canvas.focus_set)

    def open_capture(self, index, pos=0):
        self.cap = index
        folder = self.cap_dir()
        self.seq = frames_of(folder)
        if not self.seq:
            return
        try:
            w, h, _ = png_info(os.path.join(folder, self.seq[0][0]))
            self.frame_size = (w, h)
        except (OSError, ValueError):
            self.frame_size = (1, 1)
        vis = self.visible()
        self.pos = vis[0] if pos >= 0 else vis[-1]
        for p in [p for p in self.cache if os.path.dirname(p) != folder]:  # the last capture's drawn frames
            if self.cache[p][1] == self.shown:
                self.shown = None
            self.canvas.delete(self.cache.pop(p)[1])
        self.select_in_list()
        self.win.title(f"Frame generation captures - {self.captures[index]}")
        self.load_around()
        self.build_strip()
        self.render()

    def load_around(self):
        """Decode this capture's frames, nearest first, then the next capture's; drop the rest."""
        here = [self.path_at(i) for i in sorted(range(len(self.seq)), key=lambda i: abs(i - self.pos))]
        nxt = []
        if self.cap + 1 < len(self.captures):
            d = self.cap_dir(self.cap + 1)
            nxt = [os.path.join(d, f[0]) for f in frames_of(d)]
        wanted = set(here) | set(nxt)
        for p, fut in list(self.futures.items()):
            if p not in wanted and fut.cancel():
                del self.futures[p]
        for p in here + nxt:
            if p not in self.images and p not in self.futures and p not in self.errors:
                fut = self.pool.submit(decode, p)
                self.futures[p] = fut
                fut.add_done_callback(lambda f, p=p: self.done.put((p, f)))
        # Memory: keep the captures used last.
        for d in (self.cap_dir(self.cap + 1) if nxt else None, self.cap_dir()):
            if d:
                if d in self.held:
                    self.held.remove(d)
                self.held.append(d)
        while len(self.held) > KEEP_CAPTURES:
            old = self.held.pop(0)
            for p in [p for p in self.images if os.path.dirname(p) == old]:
                del self.images[p]

    def poll(self):
        changed = False
        while True:
            try:
                p, fut = self.done.get_nowait()
            except queue.Empty:
                break
            if self.futures.get(p) is not fut:  # cancelled, or its capture deleted
                continue
            del self.futures[p]
            try:
                img, box = fut.result()
                self.images[p] = (img, box)
                d = os.path.dirname(p)
                if d not in self.boxes:
                    self.boxes[d] = box
                    if self.seq and d == self.cap_dir():
                        self.reset_view()
            except Exception as e:  # noqa: BLE001 - shown in the view instead
                self.errors[p] = f"could not read {os.path.basename(p)}: {e}"
            changed = True
        if changed and self.seq:
            self.update_strip()
            if self.path_at(self.pos) in self.images or self.path_at(self.pos) in self.errors:
                self.render()
        self.win.after(30, self.poll)

    def visible(self):
        return [i for i, f in enumerate(self.seq) if f[1] == "real" or not self.real_only.get()]

    def go(self, i):
        if not self.seq:
            return
        self.pos = i
        self.update_strip()
        self.render()

    def step(self, d):
        vis = self.visible()
        if not vis:
            return
        k = vis.index(self.pos) + d if self.pos in vis else 0
        if 0 <= k < len(vis):
            self.go(vis[k])
        elif self.playing:
            self.go(vis[k % len(vis)])
        elif d > 0 and self.cap + 1 < len(self.captures):
            self.open_capture(self.cap + 1, 0)
        elif d < 0 and self.cap > 0:
            self.open_capture(self.cap - 1, -1)

    def step_capture(self, d):
        if 0 <= self.cap + d < len(self.captures):
            self.open_capture(self.cap + d)

    def on_real_only(self):
        if self.seq and self.seq[self.pos][1] != "real":
            self.pos -= 1  # the real frame before it
        self.update_strip()
        self.render()

    # ---- the strip of frames ------------------------------------------------------------

    def build_strip(self):
        for w in self.strip.winfo_children():
            w.destroy()
        self.slots = []
        for i, (name, kind, _) in enumerate(self.seq):
            slot = tk.Label(self.strip, text=name[:-4] if kind == "real" else "gen", width=5 if kind == "real" else 4,
                            bg=PANEL, fg=DIM, padx=4, pady=3, cursor="hand2", font=("Segoe UI", 10, "bold"))
            slot.pack(side="left", padx=2)
            slot.bind("<Button-1>", lambda e, i=i: self.go(i))
            self.slots.append(slot)
        ttk.Label(self.strip, text="Left/Right frame · Up/Down capture · Space play · R real only · "
                                   "wheel zoom · drag pan · F fit · 1 100% · B bars · O file · Del delete",
                  foreground="#8a8a8a").pack(side="right", padx=6)
        self.update_strip()

    def update_strip(self):
        for i, slot in enumerate(self.slots):
            kind = self.seq[i][1]
            if self.real_only.get() and kind != "real":
                fg = "#444444"
            elif self.path_at(i) in self.images:
                fg = REAL if kind == "real" else GEN
            else:
                fg = DIM
            slot.configure(fg=fg, bg=SELECT if i == self.pos else PANEL)

    # ---- view ------------------------------------------------------------------------

    def region(self):
        w, h = self.frame_size
        if self.bars.get():
            return (0, 0, w, h)
        return self.boxes.get(self.cap_dir(), (0, 0, w, h)) if self.seq else (0, 0, w, h)

    def canvas_size(self):
        return max(1, self.canvas.winfo_width()), max(1, self.canvas.winfo_height())

    def fit_scale(self):
        cw, ch = self.canvas_size()
        l, t, r, b = self.region()
        return min(cw / max(1, r - l), ch / max(1, b - t))

    def current_view(self):
        l, t, r, b = self.region()
        if self.zoom is None or self.center is None:
            return self.fit_scale(), (l + r) / 2, (t + b) / 2
        cx = min(max(self.center[0], l), r)
        cy = min(max(self.center[1], t), b)
        return self.zoom, cx, cy

    def reset_view(self):
        if self.zoom is not None and self.center is not None:
            l, t, r, b = self.region()
            self.center = (min(max(self.center[0], l), r), min(max(self.center[1], t), b))
        self.render()

    def fit(self):
        self.zoom = None
        self.render()

    def set_zoom(self, s, mx=None, my=None):
        cw, ch = self.canvas_size()
        old, cx, cy = self.current_view()
        s = min(max(s, self.fit_scale()), 32.0)
        if mx is None:
            mx, my = cw / 2, ch / 2
        # Keep the frame point under (mx, my) where it is.
        px, py = cx + (mx - cw / 2) / old, cy + (my - ch / 2) / old
        self.center = (px - (mx - cw / 2) / s, py - (my - ch / 2) / s)
        self.zoom = None if s <= self.fit_scale() * 1.0001 else s
        self.render()

    def zoom_by(self, f, mx=None, my=None):
        self.set_zoom(self.current_view()[0] * f, mx, my)

    def on_wheel(self, e):
        x, y = e.x_root - self.canvas.winfo_rootx(), e.y_root - self.canvas.winfo_rooty()
        cw, ch = self.canvas_size()
        if 0 <= x < cw and 0 <= y < ch:
            self.zoom_by(1.25 if e.delta > 0 else 0.8, x, y)

    def on_press(self, e):
        self.canvas.focus_set()
        self.drag = (e.x, e.y)

    def on_drag(self, e):
        if self.drag is None or self.zoom is None:
            return
        s, cx, cy = self.current_view()
        self.center = (cx - (e.x - self.drag[0]) / s, cy - (e.y - self.drag[1]) / s)
        self.drag = (e.x, e.y)
        self.render()

    def on_double(self, e):
        if self.zoom is None:
            self.set_zoom(1.0, e.x, e.y)
        else:
            self.fit()

    def on_motion(self, e):
        if self.view is None:
            return
        x0, y0, s = self.view
        fx, fy = int(x0 + e.x / s), int(y0 + e.y / s)
        w, h = self.frame_size
        self.cursor = f"x {fx}  y {fy}    " if 0 <= fx < w and 0 <= fy < h else ""
        self.update_status()

    def schedule_render(self):
        if not self.pending_render:
            self.pending_render = True
            self.win.after_idle(self.render)

    def drawn(self, path, s, cx, cy):
        """The canvas item showing the visible part of a frame at this view.

        Each frame drawn at the current view keeps its own (hidden) item:
        pointing one item at another large photo makes Tk convert that photo
        for the screen again, about 16 ms, too slow for playback at 60 frames
        a second; showing and hiding items costs about 5.
        """
        cw, ch = self.canvas_size()
        key = (self.region(), cw, ch, round(s, 6), round(cx, 3), round(cy, 3))
        if key != self.cache_key:
            for _, item in self.cache.values():
                self.canvas.delete(item)
            self.cache, self.cache_key, self.shown = {}, key, None
        if path in self.cache:
            return self.cache[path][1]
        img, _ = self.images[path]
        l, t, r, b = self.region()
        x0, y0 = cx - cw / (2 * s), cy - ch / (2 * s)
        ix0, iy0 = max(l, int(math.floor(x0))), max(t, int(math.floor(y0)))
        ix1, iy1 = min(r, int(math.ceil(x0 + cw / s))), min(b, int(math.ceil(y0 + ch / s)))
        if ix1 <= ix0 or iy1 <= iy0:
            return None
        size = (max(1, round((ix1 - ix0) * s)), max(1, round((iy1 - iy0) * s)))
        photo = ImageTk.PhotoImage(img.resize(size, Image.NEAREST if s >= 2 else Image.BILINEAR,
                                              box=(ix0, iy0, ix1, iy1)))
        item = self.canvas.create_image((ix0 - x0) * s, (iy0 - y0) * s, anchor="nw", image=photo, state="hidden")
        self.canvas.tag_lower(item)  # under the label
        self.cache[path] = (photo, item)
        return item

    def show(self, item):
        if item != self.shown:
            if self.shown is not None:
                self.canvas.itemconfigure(self.shown, state="hidden")
            if item is not None:
                self.canvas.itemconfigure(item, state="normal")
            self.shown = item

    def render(self):
        # The label and message items are made once and then changed.
        self.pending_render = False
        c = self.canvas
        cw, ch = self.canvas_size()
        if not hasattr(self, "items"):
            self.items = {
                "message": c.create_text(0, 0, fill="#888888", font=("Segoe UI", 13), justify="center"),
                "box": c.create_rectangle(0, 0, 0, 0, fill="#000000", outline=""),
                "label": c.create_text(14, 10, anchor="nw", font=("Segoe UI", 12, "bold")),
            }
        items = self.items
        c.coords(items["message"], cw / 2, ch / 2)
        if not self.seq:
            self.show(None)
            c.itemconfigure(items["label"], text="")
            c.coords(items["box"], 0, 0, 0, 0)
            c.itemconfigure(items["message"],
                            text=f"No captures in\n{self.folder}\n\nPress F9 in mpv while frame generation is on.")
            self.view = None
            return
        s, cx, cy = self.current_view()
        self.view = (cx - cw / (2 * s), cy - ch / (2 * s), s)
        self.update_status()
        path = self.path_at(self.pos)
        self.show(self.drawn(path, s, cx, cy) if path in self.images else None)
        message = "" if path in self.images else self.errors.get(path, "loading…")
        if c.itemcget(items["message"], "text") != message:
            c.itemconfigure(items["message"], text=message)
        _, kind, label = self.seq[self.pos]
        vis = self.visible()
        k = vis.index(self.pos) + 1 if self.pos in vis else 0
        text = f"{pretty(self.captures[self.cap])}     {label}     {k}/{len(vis)}"
        if c.itemcget(items["label"], "text") != text:
            c.itemconfigure(items["label"], text=text, fill=REAL if kind == "real" else GEN)
            x0, y0, x1, y1 = c.bbox(items["label"])
            c.coords(items["box"], x0 - 8, y0 - 4, x1 + 8, y1 + 4)

    # ---- playback --------------------------------------------------------------------

    def toggle_play(self):
        self.playing = not self.playing
        self.play_button.configure(text="Pause" if self.playing else "Play")
        if self.playing:
            s, cx, cy = self.current_view()
            for i in self.visible():  # draw them all once, so the loop runs smoothly
                if self.path_at(i) in self.images:
                    self.drawn(self.path_at(i), s, cx, cy)
            self.due = time.perf_counter()
            self.play_tick()

    def play_tick(self):
        if not self.playing or not self.seq:
            return
        now = time.perf_counter()
        if now >= self.due:
            vis = self.visible()
            k = (vis.index(self.pos) + 1) % len(vis) if self.pos in vis else 0
            if self.path_at(vis[k]) in self.images:  # else wait for it rather than skip it
                self.go(vis[k])
                speed = SPEEDS[self.speed.current()][1]
                hold = FRAME_S / speed * (2 if self.real_only.get() else 1)
                self.due = max(self.due + hold, now)
        self.win.after(max(1, int((self.due - time.perf_counter()) * 1000)), self.play_tick)

    # ---- deleting -----------------------------------------------------------------------

    def delete_capture(self, permanent=False):
        """The capture on show goes to the Recycle Bin (or is deleted for good), and the next one opens."""
        if not self.seq:
            return
        if self.playing:
            self.toggle_play()
        index, name, folder = self.cap, self.captures[self.cap], self.cap_dir()
        if permanent and not messagebox.askyesno(
                "Capture viewer", f"Delete the capture {pretty(name)} for good?\n\n{folder}\n\n"
                                  "It does not go to the Recycle Bin.", icon="warning", default="no", parent=self.win):
            self.canvas.focus_set()
            return
        # ffmpeg keeps the file it is decoding open, and Windows will not delete an open file.
        running = []
        for p, fut in list(self.futures.items()):
            if os.path.dirname(p) == folder:
                del self.futures[p]
                if not fut.cancel():
                    running.append(fut)
        wait(running, timeout=10)
        error = None
        try:
            if permanent:
                shutil.rmtree(folder)
            else:
                recycle(folder, int(self.win.wm_frame(), 16))
        except OSError as e:
            error = e
        self.canvas.focus_set()
        if frames_of(folder):  # still there: an error, or No in the shell's warning
            if error:
                messagebox.showerror("Capture viewer", f"Could not delete {pretty(name)}:\n\n{error}", parent=self.win)
                self.canvas.focus_set()
            self.open_capture(index)
            return
        for p in [p for p in self.cache if os.path.dirname(p) == folder]:
            item = self.cache.pop(p)[1]
            if item == self.shown:
                self.shown = None
            self.canvas.delete(item)
        for d in (self.images, self.errors):
            for p in [p for p in d if os.path.dirname(p) == folder]:
                del d[p]
        self.boxes.pop(folder, None)
        if folder in self.held:
            self.held.remove(folder)
        del self.captures[index]
        self.listbox.delete(index)
        if self.captures:
            self.open_capture(min(index, len(self.captures) - 1))  # the next one, or the new last
        else:
            self.cap, self.seq = -1, []
            self.win.title("Frame generation captures")
            self.build_strip()
            self.render()
        self.say(f"{pretty(name)} deleted" if permanent else f"{pretty(name)} moved to the Recycle Bin")

    # ---- misc --------------------------------------------------------------------------

    def say(self, text, seconds=4):
        """A message in the status bar for a few seconds, instead of the cursor and zoom."""
        self.note = (text, time.perf_counter() + seconds)
        self.update_status()
        self.win.after(seconds * 1000 + 50, self.update_status)

    def update_status(self):
        text, until = self.note
        if time.perf_counter() >= until:
            text = f"{self.cursor}zoom {self.view[2] * 100:.0f}%" if self.view else ""
        self.status.configure(text=text)

    def show_file(self):
        if self.seq:
            subprocess.Popen(["explorer", "/select,", os.path.normpath(self.path_at(self.pos))])

    def on_error(self, kind, value, tb):
        import traceback
        messagebox.showerror("Capture viewer", "".join(traceback.format_exception(kind, value, tb))[-2000:])

    def close(self):
        self.playing = False
        self.pool.shutdown(wait=False, cancel_futures=True)
        self.win.destroy()

    def run(self):
        self.win.mainloop()


def main():
    folder = os.path.abspath(sys.argv[1]) if len(sys.argv) > 1 else os.path.join(APP_DIR, "Captures")
    if sys.platform == "win32":
        import ctypes
        try:
            ctypes.windll.shcore.SetProcessDpiAwareness(1)  # 100 % zoom = the frame's own pixels
        except (AttributeError, OSError):
            pass
        ctypes.windll.winmm.timeBeginPeriod(1)  # playback timing to the millisecond
    if not os.path.isfile(FFMPEG):
        tk.Tk().withdraw()
        messagebox.showerror("Capture viewer", f"ffmpeg.exe was not found at\n{FFMPEG}")
        return
    Viewer(folder).run()


if __name__ == "__main__":
    main()

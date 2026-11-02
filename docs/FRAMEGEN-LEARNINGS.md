<!-- SPDX-License-Identifier: MIT -->
# Frame generation: what was tried, measured and learned

This is the history behind Glasir's frame generation: how the method got to
where it is, how it was tested, the research rounds (including the
monocular-depth experiment), and the dead ends, so they are not walked
again. How the shipped version works, its switches and its build are in
[src/framegen/README.md](../src/framegen/README.md); this file does not
repeat that.

The work was done October 2026 in the prototype (MPV-SW-Capture with a
ReShade add-on, `zz-msc-framegen.addon64`) and ported to Glasir unchanged
(`fg_eval` scores identical, 27 shader headers byte-identical). Folders and
scripts named below that are not in this repo live on the owner's PC; the
captures (real game frames) are never committed.

## Contents

1. [Goal and constraints](#1-goal-and-constraints)
2. [How the method evolved](#2-how-the-method-evolved)
3. [How quality is measured](#3-how-quality-is-measured)
4. [Research rounds](#4-research-rounds)
5. [The monocular-depth experiment](#5-the-monocular-depth-experiment)
6. [Pacing, VRR and latency](#6-pacing-vrr-and-latency)
7. [Dedup and the PS5's noise](#7-dedup-and-the-ps5s-noise)
8. [The GPU budget](#8-the-gpu-budget)
9. [Testing: tools and gotchas](#9-testing-tools-and-gotchas)
10. [Known leftovers and open ideas](#10-known-leftovers-and-open-ideas)

---

## 1. Goal and constraints

- x2 only: 30 → 60 and 60 → 120 fps (the owner's call; x3/x4 never built).
- Two frames, nothing else: a capture card gives no motion vectors, no depth,
  no HUD mask. The add-on finds its own motion. (It uses no ReShade motion
  vectors either, which was once suspected of causing a ghost.)
- Runs inside mpv's D3D11 device as a ReShade add-on, after DLSS 5, on the
  presented picture (so it sees what DLSS 5 made, and the menu is drawn after
  it).
- Any GPU (it is plain D3D11 compute and pixel shaders, SM 5).
- Budget agreed with the owner: up to ~5 ms GPU per generated frame; a
  lighter mode for 60 → 120 is fine.
- Behaviour agreed with the owner for motion that cannot be found (a turning
  spiky hairdo): show it **solid with a lagging shape** (one real frame moved
  with the head), never see-through, never a blend.

## 2. How the method evolved

Each step below fixed something seen in the owner's captures (F9 saves five
real frames and the frames drawn between them) or in the synthetic clips.
Scores are `fg_eval` means at the time (section 3).

### 2.1 The base: motion-compensated, not blended

- A plain cross-fade scored 23.6 dB on the synthetic clips; motion
  compensation 43.0 dB (2026-10-25).
- Luma pyramids of both frames, block search coarse to fine (16 px cells),
  median filtering, then a per-pixel mix of candidate motions from the 3x3
  neighbouring cells, each scored on a 5-pixel cross.
- Handled from the start: fades (a brightness gain from motion-matched
  content), cuts (repeat frame A when more than 20% of cells match nothing),
  static overlays, very small windows (blend fallback).
- Finer 8 px cells were tried and scored worse.

### 2.2 Blockiness around characters and effects (10-25)

- Seen: blocky patches where things changed at 16 px cell edges.
- Cause: the candidate set shifting at cell edges, and a per-block one-frame
  cost.
- Fixed with a tent fade of each cell's reach, a bilinear field mismatch, and
  a wide soft mix (`soft_wide` 0.15) where no one-frame option explains the
  pixel.
- `eval/blockiness.py` (1.0 = no cell grid) on the owner's pairs: 1.18 / 1.55
  max before, 1.00 / 1.04 after; real frames score 1.04.
- Dropped: a cell-cost "doubt" fallback to cross-fade or hold (no visible
  gain).

### 2.3 A copy of the head beside a still character during camera orbits (10-25)

- Seen: a faint hair and face copy about 50 px beside the head.
- Cause: the head, a few blocks wide, was lost to the scenery on the coarse
  pyramid levels, and the fine search radius could not get it back.
- Fix: a second search around zero motion, taken when its best cost beats the
  estimate window's best by a margin (+0.19 ms).
- Dead ends:
  - comparing zero against the *unsearched* estimate punched a sky hole into
    an evenly textured pillar;
  - an absolute fit limit, or fewer pyramid levels, traded the pillar against
    the head.

### 2.4 Background showing through the top of the head (10-26)

- Seen: during camera turns, the background showed through the top of the
  head.
- Cause: turning hair spikes fit themselves worse (5-tap cost ~0.05) than the
  plain ground beside them carried by the scenery's motion (~0.01).
- Fix ("persistence", `layer_cost` 0.2): where both frames show the same
  thing at a pixel and a still cell is near, a moving option that brings a
  different colour pays a penalty.
- Dead ends:
  - global camera detectors (dominant motion, still share): they cannot tell
    static-camera combat from a camera turn;
  - a forward/backward round-trip "hidden motion" cue (the fields have a halo
    of the character's zero motion);
  - a proportional penalty;
  - an unconditional penalty, which erased details narrower than a pan's step
    (−2 to −15 dB on pans).

### 2.5 "Three tips" in fast orbits (10-28)

- Seen: at 130–140 px/frame, a faint copy of the hair tips half a scenery
  step beside the head.
- Cause: in the strip that only one frame shows, the wrong frame's sample
  lands on the character and costs the same as a real occlusion.
- Fix (`slow_cost`, NearSlowBlock): a one-frame option along the motion most
  cells share pays when its sample is near a slow block of its own frame,
  applied one way round only.
- The 25-block loop must stay `[loop]`: unrolled, it tripled the shader and
  cost ~0.5 ms.
- Dead ends:
  - a symmetric version blocked one-frame options in smoke and effects
    (patches on spear blades);
  - "still = slower than 0.1x the fastest cell" tore a colonnade and holed a
    dress.

### 2.6 The core weakness, and the character layer (10-28 to 10-30)

A thin, deforming spike over smooth floor loses to the floor's motion, which
fits almost perfectly. This is a property of the method, not a regression; it
is what the research rounds (section 4) were about. What shipped:

- **The character layer (LAYERS).** A local affine background model per
  128 px block (a global model failed on perspective floors), then the
  character found as a compact component that moves differently, its own
  motion searched, drawn hard from frame A, replacing only the pixels that
  WarpPS drew with background motion.
  - needle 15/15, needle ratio 0.40 → 0.99; everything else unchanged;
    +0.07 ms on pans, +0.75 ms on orbits.
- **Polish rounds:**
  - `layer_bgnear` 0.3: the gate counts options near the background model,
    for floors whose motion is 10–15 px off the model near the head;
  - selection gates that filter components instead of killing the layer;
  - flat cells weighted into the background model (motion-blurred floors
    have no detail);
  - a ghost rule (off-layer options landing on the layer's mask pay).
- **"pick3" (10-30), for NPCs and capes winning over the player's head:**
  merge small clear fragments into slow central cores, prefer still and
  central components, refine the edge.
  - Only active when the dominant motion is 6 px or more, so fights stay
    bit-identical.
  - hole score 4930 → 4558; everything else equal.
- **Better cut detection.** A real cut read 0.206 against a 0.2 threshold.
  The detector now also counts the unmatched share among detailed cells: the
  real cut reads 0.54, the busiest non-cut 0.34.

Dead ends in this phase:

- **Per-pixel colour ranking** (round-1 implementations):
  - minority-vector "rescue" pasted stair and pillar aliases;
  - contrast priority gave tiny gains or streaks;
  - keeping the spike vector through the median gave a ragged spike at
    −1.8 dB on the synthetic clips.
  - Lesson: per-pixel ranking cannot restore a deforming spike; it needs
    region/layer decisions.
- **Dense colour-guided motion with splatting ("guide"):** needle 15/15, but
  splat specks and tears almost everywhere; dropped after review.
- **A "near layer"** (second-search motion as extra per-pixel candidates):
  +4 ms, −1.9 dB.
- **Per-pixel stand-still detection:** noisy speckle, 13 ms.
- **Up to 3 layers ("multi3"):** +0.4–0.7 ms, hard pastes.
- **Coarse neighbours' estimates:** fixed heads slightly but spread aliased
  motion on stairs, washed a minimap corner, doubled a label (found by a
  17-agent review of all pairs).
- **A "pair" penalty:** gains only on easy pans.
- **`cap_fit` / `cap_cell` softness caps:** they wiped the faint ghost of a
  fast spear and thinned armour in combat; off by default.

## 3. How quality is measured

### 3.1 Tools (in `src/framegen/eval`)

- **`fg_eval.exe`** runs the add-on's own shaders offline.
  - Synthetic clips, made from game screenshots: pans, zoom, subtitles,
    fades, a cut, HUD, UI fade/slide, a noisy HUD, orbiting sprites. Scored as
    PSNR against the true middle frame.
  - Real captures: scored skip-one (frames 1 + 3 → 2), or `--adjacent` to
    write what the add-on would draw.
- **Metrics:**
  - `blockiness.py`: the cell grid; 1.0 = none.
  - `head_score.py`: PSNR in 22 head regions.
  - `needle_score.py`: thin hair pixels kept against both real frames.
  - `hole_score.py` (in the research folder on the owner's PC, not in this
    repo): hair dark in both real frames but not in the generated one.
  - Ghost counts.
- **Viewing:** `compare.sh` (tone-mapped crops side by side) and
  `view_captures.py` (a capture viewer).
- **`debug_view`** in the add-on colours pixels by what they show:
  - 1: which option was used;
  - 2 / 3: softness / cell cost;
  - 4: motion lengths (the quickest way to see a lost object);
  - 10: the strongest option's motion;
  - 11: depth, in the depth build.

### 3.2 Lessons about measuring

- Mean PSNR over-rewards easy clips (above 45 dB); judge by the hard clips,
  the captures and blockiness.
- Skip-one head PSNR is a poor proxy for hair, because skipping a frame
  doubles the motion. The adjacent-pair needle score replaced it.
- **Never tune to one capture.** The owner had one "dodge" capture deleted
  everywhere because work was being judged by it. Judge across the whole set:
  totals, plus how many captures got better and how many worse.
- Live `between_*.png` from F9 match `fg_eval` exactly, so offline results
  are what the player sees.
- Visual review by independent judges (Grok 4.7, several tasks, all pairs,
  before vs after) caught what scores missed: the combat regressions of
  `cap_fit`, the stair blotches, tears in effects. Every shipped change went
  through one.

### 3.3 Where it ended (final prototype build, 10-30, ported unchanged)

| measure | value |
|---|---|
| synthetic clips (mean PSNR) | 42.93 dB |
| captures, skip-one | 32.75 (28 captures) / 31.43 (40) |
| head regions | ~27.9–28.3 dB (set-dependent) |
| blockiness mean / max | 1.069 / 1.152 |
| needle pass, ratio | 15/15, 0.97–0.99 |
| hole score | 4558 |
| GPU time per generated frame | ~3–3.7 ms at 1440p, RTX 5070 Ti |

## 4. Research rounds

Five research reports were written first, then two rounds of
implementations in private copies, scored with the same tools.

### 4.1 Hardware and SDK motion, rejected

- **NVIDIA Optical Flow (NVOFA) and NvOFFRUC:**
  - a finer hardware grid does not change the tie the floor wins;
  - NvOFFRUC is over budget at 1440p, destroys HDR, its hole fill is the
    phasing bug itself, and shipping `NvFRUC.dll` is not granted by the
    licence text.
  - The only acceptable experiment would be OFA 4x4 Medium feeding the
    existing per-pixel step (3–5 ms, 30 → 60 only). Not done.
- **D3D12 Video Motion Estimation:** 8x8/16x16 encoder search through a
  second device, unknown radius; no finer than ours.
- **AMD FidelityFX optical flow and frame interpolation (MIT shaders):**
  - its optical flow is the same design as our search;
  - its interpolation keeps thin parts only by using depth.
  - FSR 4 frame generation (ML) needs camera data and Radeon 9000.
  - AMD's AFMF has no source.
- **Neural interpolation:**
  - RIFE-class networks need ~16–26 ms at 1440p, far over budget;
  - they are trained on photometric error, the same tie the shader loses.
- **NVIDIA Maxine Video Frame Generation (VFG)**, looked at again 11-01 from
  Veyra's notes:
  - RTX 40/50 only, 2–8x, Low/Medium/High;
  - CUDA, so D3D11 ↔ CUDA sharing would be needed;
  - the runtime (~435 MB) is anonymous from NVIDIA's Python index, but the C
    headers need an NGC account;
  - cost on an RTX 5070 (with Python overhead): Medium ~2.9 ms at 1080p and
    ~3.9 ms at 4K; High ~14.6 ms at 1080p;
  - users report edge "jelly" on Low/Medium in camera turns.
  - Not built. The suggested first step, if ever: score it offline with
    `fg_eval` on the same captures.

### 4.2 Literature on motion-compensated interpolation

The consensus was per-pixel candidate ranking with foreground priority. The
round-1 implementations of that failed (section 2.6). The useful outcomes:

- the critic's verdict to stamp the body region from one frame;
- the needle score;
- the layer design that shipped.

## 5. The monocular-depth experiment

From 10-29 to 10-31. The question: can a depth estimate per frame tell the
character from the floor and from NPCs, where the shader's colour matching
cannot? It was done in separate folders (`FrameGen-Depth`, later the app copy
`MPV-SW-Capture-FrameGen-Depth`); the shipped add-on was never touched.

### 5.1 Setup

- **Model:** Depth Anything V2 Small (Apache-2.0; the Base model is
  CC-BY-NC). Input is the PQ frame turned into SDR; output is 16-bit, larger =
  closer, normalised per frame between percentiles 1 and 99.
- **Shaders:** depth maps bound to every pass (t14/t15), with `depth_mode`
  and parameters in the constant buffer, `fg_eval --depth DIR`, and
  `debug_view 11`.
- **Only frame A's depth is needed.** It can be computed one frame ahead, so
  it adds no delay.
- **Speed** (fp16, ms per map; the 5070 Ti was busy with the game):

| engine | 378x672, 4070 SUPER | 518x924, 4070 SUPER | 518x924, 5070 Ti |
|---|---|---|---|
| TensorRT 11.4 | 2.5 | 4.8–5.0 | 3.0–3.5 |
| TensorRT-RTX 1.7.1 | 2.6–2.7 | 4.3–4.4 | 3.1–3.4 |
| DirectML (ONNX Runtime) | ~8 (incl. copies) | ~29 | 19.5–21 |

- **What the numbers taught:**
  - DirectML needs fixed input sizes, or shape ops fall back to the CPU
    (240 ms);
  - DirectML is ~3x slower than TensorRT;
  - fp32 is 3–5x slower than fp16;
  - TensorRT engine builds take 1–2 minutes per GPU and size, giving 60–70 MB
    plan files.

### 5.2 Rounds

- **Round 1:**
  - `depthmask`: a small gain, +0.01 ms.
  - `depthgate`: needed frame B's depth; about base; dropped.
  - `zorder` (nearer wins in WarpPS): fixed the floor showing through NPCs,
    head +0.4 dB, but +1.4–2.2 ms with hard edges and blocks.
- **Round 2:**
  - `depthselect`: depth picks the player over NPCs and capes, and builds the
    layer from depth for dodges. Frame A only, +0.1 ms; only when the dominant
    motion is 24 px or more.
    - hole score 4930 → 3372; NPC confusion fixed.
    - Review verdict: ship with fixes.
  - `zorder2`: softer, half-resolution, +0.55 ms, NPCs drawn solid.
    - Review: don't ship (false dark hair tips, holes in spears and capes,
      sky speckle).
- **Round 3:** `depthselect2`, on top of the shipped non-depth "pick3".
  - It changed only 2 of 152 frames: hole 4558 → 3696, head
    27.899 → 27.974.
  - All 15 review findings were fixed.

**Conclusion then:** depth works as a signal, but on top of the non-depth
fixes it only rescues rare dodge and spin frames.

### 5.3 The live depth app (the owner asked to try it)

- **Pipeline:**
  - after DLSS 5, the add-on copies frame A to a D3D12 readback worker;
  - shared memory (`Local\msc_fg_depth_<pid>`, 3 slots, fp16 924x518 in,
    float32 out) feeds a Python TensorRT helper on the RTX 4070 SUPER (the
    second GPU);
  - the map comes back for the next pair;
  - F7 switches it.
- **Costs:**
  - ~0.2 ms on mpv's thread;
  - ~0.2 ms of 5070 Ti time per generated frame;
  - the helper ~6 ms on the 4070;
  - no pacing cost.
- **Coverage:**
  - at 30 fps, depth arrived for 100% of pairs;
  - at 60 fps with neural rendering, almost never: the map arrives 20–24 ms
    after the frame, more than the 16.7 ms interval. Frame generation backs
    off there anyway.
- **Single GPU (5070 Ti only):**
  - 30 fps works: depth on 95–99% of pairs, 0–2 late frames per 5 s;
  - 60 fps degrades: depth on ~35%, ~22 late frames per 5 s.
- **Hardening:** code reviews found fence, stall and shutdown bugs in the
  link, and helper bugs (heartbeat while loading, log spam, event-name
  squatting). All were fixed and tested with forced failures (stalls, a
  watchdog that terminates a hung helper, retry with F7).
- **Quality on 40 captures:**
  - depth changed 8 of 160 adjacent frames and 17 of 80 skip-one frames;
  - error against the real frame favoured depth on 11 of 17;
  - but the heads it keeps had holes, flecks and square edges, and the
    visual judges disagreed.

**Outcome (10-31): shelved.** After playing with it, the owner judged it "not
bad but not a major step up" and stayed with the main implementation. The
folders are kept.

**Why depth was not worth it here:**

- it needs a second GPU or costs too much at 60 fps;
- it needs ~400 MB of TensorRT DLLs kept out of mpv, a helper process and
  cross-GPU copies;
- the non-depth character layer already fixes most cases.

**Revisit only if new captures show failures the main path cannot fix.** The
z-order idea (NPCs solid) is the other lead, if its hair and weapon artefacts
can be avoided.

## 6. Pacing, VRR and latency

- **The hold** (GPU-side, 10-30):
  - the real frame is presented at once, behind a GPU wait on a D3D12 fence
    shared into D3D11;
  - a helper thread releases it half an interval after a D3D11 fence says the
    frame in between is done;
  - it is let go at the latest 0.5 ms before the next frame's effects, so
    the next DLSS 5 pass is never held.
- **Why not sleep on mpv's thread:** mpv needs ~3 ms from arrival to present,
  so a sleep-based version could not work at 60 fps.
- **Results with VRR** (PresentMon, on-screen intervals):
  - 30 → 60: 16.67 ± 0.4 ms / 16.67 ± 1.0 ms;
  - 60 → 120: 8.34 ± 0.3 / 8.33 ± 0.5;
  - no drops.
  - The old timing (real frame half an interval after mpv's present) gave
    uneven 15.0/18.3 and 5.2/11.5 ms.
- **Latency, mpv present → screen, VRR:**

| | 30 fps | 60 fps |
|---|---|---|
| frame generation off | 6.7 ms | 5.7 ms |
| generated frame | 8.5 ms | 7.5 ms |
| real frame | 25.2 ms (+18.5) | 15.9 ms (+10.2) |

  The real frame's extra is half a source frame plus drawing. The ~7 ms from
  present to screen for the frame in between is the DLSS 5 pass, not the
  interpolation: Fast quality saves only ~0.3 ms of it.
- **Repeats when frame generation is off** ("all frames when off"): 5.7 ms
  instead of 6.8 ms at 30 fps (the GPU runs each frame ~1 ms faster at 60
  presents/s). It is the low-latency option for people who don't use frame
  generation.
- **VRR only engages for the focused window.** NVIDIA's windowed G-SYNC
  follows focus (confirmed by the owner with the G-SYNC indicator). Tests
  must click the test window or run while the owner is away; check focus.
  The screen is ~165 Hz.
- **Window layouts (PresentMon, 11-01):**
  - borderless full screen, and a boxed top-level window (Glasir's full
    screen on an ultrawide, `src/app/backdrop.cpp`), both kept independent
    flip and VRR - the boxed one only while Windows gave it an overlay
    plane: after a reboot the same test (plain mpv, `--no-config`) was
    "Composed: Flip" boxed, independent flip only at exactly the monitor's
    size (3440x1439 was composed too). Composed, presents wait a frame in
    DWM, the queue fills and frame generation gave ~40 of 60 generated
    frames ("queue full") with the GPU 19 % busy. Cause not found (not the
    second screen, taskbars, SDR/HDR, Wallpaper Engine); the boxed window
    became an option, off by default (`window.skip_black_bars`, 11-01);
  - an mpv child window (`--wid`) loses VRR.
- **NVIDIA "Prefer maximum performance" for mpv** made 60 fps work ~2 ms
  slower per frame. Leave power at the default.

## 7. Dedup and the PS5's noise

- Frame generation needs new frames only: a 30 fps game captured at 60 must
  arrive at 30.
- The PS5's signal is not bit-exact on repeats. Each 60 Hz refresh adds
  zero-mean noise:
  - NV12: ~22% of pixels ±1–3 steps;
  - P010: ~63% ±1–6.
  - A per-sample threshold let every frame through.
- **Fix:** a change is a 16x16 block whose signed mean moved more than the
  threshold, or one sample more than 8x it. A frame is new from 4 changes;
  the threshold is 3.
  - PS5 noise peaks at 1.5–1.9 per block (NV12), ~0.6–1.0 (P010);
  - new frames show ≥ ~2000 changes.
  - Live: 303 new of 603 at 30 fps.
- The Switch 2 was bit-exact.

## 8. The GPU budget

- At 60 → 120 each frame has ~8.3 ms for everything. DLSS 5 neural
  rendering costs:
  - ~9.7 ms at 3440x1440;
  - ~5.5 ms on the boxed 2560x1440 picture;
  - ~15.7 ms per frame in early tests with neural rendering on.
- **Back-off:** when mpv drops frames, frame generation pauses for 5 s,
  doubling up to 60 s (reset after 30 s clean). The menu says so ("Paused,
  back in N s"), and also when the display cannot show twice the rate.
- **Fast quality** roughly halves the interpolation's own GPU time
  (README), but the interpolation is not the big cost next to DLSS 5.
- **Boxed full screen** (11-01) was the biggest single gain: DLSS 5 and frame
  generation stopped working on the ultrawide's black bars (9.7 → 5.5 ms).
  An option, off by default, since the same day (`window.skip_black_bars`):
  it depends on an overlay plane Windows does not always give (section
  above), and without one frame generation loses about a sixth of its frames
  even with DLSS 5 off. Neither the DLSS 5 add-on
  (`NRResolutionScale` only scales the whole frame) nor the feeder
  (`work_resolution`) can be limited to the picture.
- GPU timings vary ~1 ms between sessions (clocks, background apps such as
  NVIDIA Broadcast or Wallpaper Engine). Compare A/B within one session.

## 9. Testing: tools and gotchas

- **Without the card:**
  - ffmpeg test pattern → `Glasir.exe --input-raw -` (`tools/smoke_raw.py`);
  - `GLASIR_FRAMEGEN_CAPTURE_AFTER=<s>` takes an F9 capture in a test;
  - `GLASIR_FRAMEGEN_TRACE` writes per-frame timing.
- **Pacing tests:**
  - ffmpeg `-re` jitters ±12 ms and VRR shows it directly; a steady pacer
    (`raw_pace`) fixed that;
  - use a mean, not a median, for ffmpeg's source rate.
- **Warm-up:** the first ~5 s always drop frames (DLSS 5 starting); compare
  against a frame-generation-off baseline before blaming frame generation.
- **Reading PresentMon:** VRR runs show SyncInterval 0 / AllowsTearing 1 with
  intervals off the refresh grid; fixed-refresh runs show 1/0 and on-grid
  times.
- **The owner's PC:**
  - test pipelines rewrite ReShade.ini (window size, font scale): copy it
    before and back after;
  - never run two pipeline tests at once (same pipe and link names);
  - a background process cannot take focus from the owner, and screen grabs
    show whatever they have open: capture the test window with `PrintWindow`
    instead;
  - mpv ignores keys posted from another process (ReShade's input sees them);
    use mpv's IPC `keypress`.
- **Captures:**
  - the 3440-wide captures are pillarboxed (picture at x = 440–3000), so
    "off screen" checks don't fire at the picture's edge;
  - the owner prunes captures: never put deleted ones back.

## 10. Known leftovers and open ideas

- **Leftovers:**
  - a spike that turns while the head drifts comes out soft or cut;
  - a faint wisp beside a restored spike;
  - a blocky hair fringe in one orbit;
  - NPC fragments torn over hair when an NPC slides in behind the head;
  - a whole character smears when body and hair move differently (the
    dodge).
- **Ideas not pursued:**
  - finer motion at object edges (4x4 on boundary cells only);
  - x3/x4 (needs the pacing generalised);
  - frame generation on the second GPU;
  - an offline VFG comparison;
  - a z-order rule that keeps NPCs solid without hair artefacts.

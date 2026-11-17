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
  - `hud_score.py`: how far the frames in between are from what stands
    still in all five real frames of a capture (a HUD over the scenery).
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
| synthetic clips (mean PSNR) | 42.93 dB (44.08 with 4.8's defaults) |
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
- **NVIDIA Maxine Video Frame Generation (VFG):**
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

### 4.3 A 1-3-3-1 pyramid filter, rejected (11-14)

- **The idea:** each luma pyramid level is the 2x2 mean of the one above. A
  2x2 mean lets through much of what is just too fine for the next level,
  where it shows as a coarse pattern the picture never had. DLSS5-Feeder
  2.0's own estimator switched to 1-3-3-1 on each axis for that reason
  (brickwork read as moving the wrong way), and stairs and pillars were
  recurring trouble here (section 2.6).
- **Tried** as an evaluator switch (`pyr_filter`: every level, or from
  level 1 down only), offline with `fg_eval` on the 18 clips and the 32
  captures in the prototype's `framegen-captures`. Not kept in the code.

| measure | 2x2 mean | 1-3-3-1, every level | 1-3-3-1, level 1 down |
|---|---|---|---|
| synthetic clips (mean PSNR) | 42.93 dB | 42.82 | 42.70 |
| captures, skip-one (32) | 30.35 dB | 30.41 | 30.42 |
| captures better / worse (beyond 0.03 dB) | - | 10 / 13 | 12 / 14 |
| blockiness, adjacent frames (mean) | 1.065 | not run | 1.064 |

- **The mean gain was one capture.** A staircase (20261025-124130-247)
  gained 2.2 dB skip-one: the patch where the wrong step was matched shrank
  to about a third. The other 31 moved by 0.25 dB or less and cancelled out.
- **In real use the stairs got worse.** On the adjacent pairs of that
  capture the 2x2 mean draws the stairs clean, and 1-3-3-1 added a patch on
  two of the four. The skip-one gain was the doubled motion (section 3.2).
- **Clips:** the orbits gained 0.3-0.4 dB; `pan_fast` lost 0.7, `ui_fade`
  0.3-0.5, `pan_1440` 2.2.
- **A retune did not rescue it.** `search_lambda` 0.005-0.04 and
  `guess_cost` 0.35 / 0.7 with the new filter: no setting kept clips,
  captures and stairs all at least as good as the 2x2 mean. A lower
  `search_lambda` cleaned the stairs, but it does that with the 2x2 mean as
  well (section 4.4).
- **Lesson:** the search's thresholds and the 2x2 mean were tuned together
  on these captures; a filter that is better on paper only moves which
  frames get the patches. Judge such a change on adjacent pairs, not on
  skip-one PSNR.

### 4.4 `search_lambda` 0.01, rejected (11-15)

- **The idea:** it came up as a control in 4.3. Half the default (0.02) gave
  16 of 32 captures better and 5 worse skip-one.
- **Tried** offline with `fg_eval`, same 18 clips and 32 captures.

| measure | 0.02 | 0.01 |
|---|---|---|
| synthetic clips (mean PSNR) | 42.93 dB | 42.79 (12 of 18 worse, 2 better) |
| captures, skip-one (32) | 30.35 dB | 30.41 (16 better, 5 worse) |
| head regions, skip-one (24) | 26.32 dB | 26.15 (one head −3.4 dB) |
| blockiness, adjacent frames (mean / max) | 1.065 / 1.141 | 1.064 / 1.145 |
| needle pass, ratio (3 pairs left in the set) | 3/3, 0.98 | 3/3, 0.97 |

- **On adjacent pairs it is worse.** Of the 20 regions that differ most
  between the two (at most two per capture, each beside its real frames),
  about 8 were worse with 0.01, 3 better, the rest the same. Of the 8 heads
  that differ most, none was better and 3-4 were slightly worse.
- **What it does:** hard torn pieces where 0.02 gives a soft ghost - a
  walking NPC's torso drawn twice, dark blocks in fire and sparks at the
  picture's top edge, flecks beside hair, a third copy of a lantern at the
  picture's edge. The few gains were doubled NPCs at the picture's edge
  becoming one.
- **Lesson:** the same as 4.3. The skip-one gain was the doubled motion,
  where a freer search finds more; at real distances it only breaks up what
  was soft.

### 4.5 The last pair's motion as a third search centre (11-15)

In the add-on it is a switch, off by default - the one test left of the
four (4.8): `HistorySearch=1` in `ReShade.ini`, `[GLASIR_FRAMEGEN]` (the
evaluator's is `--set hist_search 1`). The owner has run it with the card
since 11-15 (below).

- **The idea:** the search tries two centres per block, the coarser level's
  estimate and no motion. A third: what the block did over the pair before
  (the last pair's b → a field, kept per level; a's block is assumed to go on
  as it came, b's block is looked up where it was in a). It is the only
  information the two frames do not hold. What that search finds is taken by
  the rule the search around no motion already has (better by 0.5 luma SAD
  and at most half as badly: `hist_cost`, `hist_ratio`).
- **In the code:** `Interpolator::keep_history()` after a pair is drawn,
  `DrawOptions::follows` on the next pair when its frame a was that pair's
  frame b. `fg_eval` does both within a clip or capture. With the switch off
  the frames are bit-identical to before.
- **In the add-on:** the history is used only when the present before drew
  a frame in between and both pairs came at the source's pace (0.5-1.5
  intervals); a skipped frame in between, a late frame, an OSD redraw or a
  resize drops it. `ReShade.log` says `HistorySearch=1: searching around the
  last pair's motion as well` the first time it is used. Checked without a
  card on a copy of the app folder (`tools/smoke_raw.py`, 11-15): 30 real +
  29.5 generated fps with the switch off and on, no drops after start-up.
  The test pattern says nothing about quality: its frames come out the same
  either way.
- **Coverage:** in the captures only the second skip-one pair and three of
  the four adjacent pairs have a pair before them; live, every pair would.

| measure | off | on |
|---|---|---|
| synthetic clips (mean PSNR) | 42.93 dB | 43.04 (8 better, 0 worse) |
| captures, skip-one (32) | 30.35 dB | 30.40 (15 better, 0 worse) |
| head regions, skip-one (24) | 26.32 dB | 26.38 |
| blockiness, adjacent frames (mean / max) | 1.065 / 1.141 | 1.065 / 1.141 |
| needle pass, ratio (3 pairs left in the set) | 3/3, 0.98 | 3/3, 0.97 |
| skip-one pixels it changes: error to the real frame | 22.6 of 255 | 16.9 (22 captures closer, 4 further) |

- **What it does:** during camera turns, things at the picture's edges and
  NPCs walking past - trees, pillars, a banner, a person drawn twice or
  see-through before - come out single and solid. The staircase capture is
  untouched.
- **Blind review of the adjacent pairs** (a fresh Claude agent, the 40
  regions that differ most, at most two per capture, old and new in random
  order beside the real frames): new better in 19 (4 sure, 8 fairly, 7
  barely), old better in 9 (0 / 3 / 6), no difference in 12. An earlier
  look at the top 20, knowing which was which, gave about 11 / 3 / 6.
- **Where it loses:** the four captures further from the real frame are three
  fights full of effects (skip-one: a few darker, harder pieces inside an
  explosion and around a spear hand, where it was a soft smear) and one
  banner beside the minimap (broken either way). On the adjacent pairs the
  reviewer's three fairly sure losses were a red banner on a pillar leaving
  the picture (a torn dark piece instead of a faint ghost), a pedestal at
  the picture's edge, and two small HUD digits over fast dark scenery with a
  little more fuzz beside them (another game, 20261031-144655-466).
- **Tried on top:** not letting it replace what the search around no motion
  won. No frame changed visibly, so that is not how the digits got their
  fuzz; left out.
- **Thresholds:** looser ones score higher skip-one and then start to lose
  (`hist_cost` 0 / `hist_ratio` 1: 24 captures better, 4 worse) - the trap of
  4.3 and 4.4. The search around no motion's own values lose nothing, so they
  stay; they were not tuned on these captures.
- **Cost:** not measurable in the evaluator (single runs vary by 1 ms on a
  busy GPU); one more search per block where the third centre differs from
  the other two, so at most half again of the search's ~0.5 ms.
- **First captures with the card** (11-15, the owner, `HistorySearch=1`,
  five F9 captures of another game: a dark cave, running and fast camera
  turns with heavy motion blur):
  - the add-on's own frames match the evaluator's with the switch on (pairs
    2-4: 0.002-0.14 % of pixels apart, against 0.02-0.78 % from the
    evaluator with it off), so the wiring does what was measured offline;
  - those live frames had minutes of pairs behind them, the evaluator's one
    pair: a long run of history did not drift away from a short one;
  - on this game it changes little: under 0.8 % of a frame's pixels,
    skip-one +0.00 to +0.11 dB, and of the 12 regions that differ most 11
    look the same and one has a hard dark block at the picture's edge where
    both versions are broken. Nothing was clearly better here.
- **Not known:** what it costs on the GPU next to DLSS 5, and how it looks
  in play on the game the gains were measured on (camera turns with NPCs
  and scenery at the picture's edges).
- **A test clip to distrust:** `orbit_drift` scores 4 dB under `orbit`
  because ffmpeg puts the overlaid character on even pixels only: it moves 4
  px, then 2, not 3 and 3, and the "true" middle frame is 1 px off the
  half-way point. The layer finds its motion exactly (6.0 px).

### 4.6 The picture's edges, and the generator on the picture alone (11-15)

**Status:** on by default since 11-16 (4.8): `PictureOnly` and `EdgeRules`
(evaluator: `--picture X0 Y0 X1 Y1`; the edge rules are its defaults now,
`--set edge_cost -1 --set edge_explains 1` is without them - when this was
written they were off and `--set edge_cost 0.01 --set edge_explains 0` turned
them on). With both off, frames are bit-identical to before. The HUD they
cost is 4.7's matter (`StillHud`).

The outer 96 px of the picture were its weakest part: 25.6 dB skip-one on
the 32 captures against 28.6 dB for the rest. In a camera turn the strip
that only one frame shows (content leaving or coming in, half the step
wide) came out as a smear or a ghost of the other frame.

- **The guess that was wrong:** WarpPS's "off screen" test (`Inside`) knows
  the frame, not the picture, so a sample in a black bar counts as on
  screen. Testing against the picture's box instead changed nothing: edge
  strips 25.57 → 25.57 dB, the rest of the frame bit-identical. A sample in
  a bar is already ruled out by its cost.
- **What does go wrong** (seen with `debug_view` 4 on a 165 px turn):
  - *SearchCS, with bars:* a coarse block that straddles a bar is held
    still by the bar's part of it, and the finer levels under it start from
    no motion, 80 px short. Everything within a coarse block of a bar is
    pulled toward standing still.
  - *MidCS:* it compares a with b along each motion on offer. The right
    motion for the strip takes one of the two samples off the picture,
    where it reads the bar or (at the frame's own edge) the edge pixel
    repeated: a high cost, so the cell takes no motion or a wrong one.
  - *WarpPS, at the frame's own edge only:* where the other frame's sample
    is off the frame, one frame alone "explains" the pixel whatever that
    frame's motion says, so the softness stays narrow and the pixel makes a
    hard choice between whatever motions are on offer: torn pieces where a
    bar gave a soft fade.

**PictureOnly: the generator on the picture alone.** `fg::Interpolator`
takes the picture's place (`fg::Picture`): it copies that part of a and b,
runs every pass on the copies, draws into that part of the target and
copies what is around it from a. The add-on has the box from mpv
(`osd-dimensions`, as the screenshot does) and makes the generator anew
once the box has been the same for 15 presents.

- The box must be the picture exactly (x = 440–3000 here). One or two
  columns of the picture's edge spill into the bars at a fifth of its
  brightness; inside the box that column is carried along a turn as a dark
  line. Outside it, it is drawn as frame a has it.
- `cut_fraction` was set on frames with bars, whose cells always match. Of
  the picture alone the same frames leave a larger share unmatched: one
  fight frame at 0.172 of the frame is 0.231 of its picture, over the 0.2
  limit, and was shown as a repeat (−1.9 dB on that frame). The limit is
  now scaled by frame area over picture area, which keeps the decisions as
  they were.

**EdgeRules**, for the frame's own edges (the picture's, with PictureOnly):

- mid: under a motion that takes a cell's content off the frame, a pixel
  whose sample in a or b is off the frame costs a fixed `edge_cost` (0.01)
  instead of the comparison with the edge pixel, if
  - the frame further in, at the nearest place along the motion that both
    frames show, has that motion (`edge_tol`: within a quarter of its
    length + 2 px) and it carries a onto b there (`edge_fit` 0.02);
  - the pixel does not stand still (`edge_still` 0.004: a HUD at the edge
    is not carried off with the scenery).
- warp: `edge_explains` 0, so a one-frame option explains a pixel at the
  frame's edge only if that frame's own motion agrees, as everywhere else.
- Not in the switch: `edge_search` (a block the coarser estimate takes off
  the frame keeps the estimate, unless it matched within `edge_fit`). It
  adds another 0.5–1 dB on the pan clips, but in a blind review with it
  (and before `edge_still`) the two confident verdicts went against the
  rules: hard blocks across an effect scene and a broken minimap border.

| | baseline | PictureOnly | + EdgeRules |
|---|---|---|---|
| clips, mean | 42.93 dB | 42.93 (no bars) | 44.11 (`pan_slow` −0.39 at 60 dB, the rest up or equal) |
| captures, skip-one (32) | 30.35 dB | 30.87 (26 better, 3 worse) | 31.05 (27 better, 1 worse) |
| the owner's five of 11-15 | 30.71 dB | 30.94 | 31.02 |
| edge strips, 64 frames | 25.57 dB | 25.75 (32 better, 26 worse) | 26.31 (43 better, 14 worse) |
| rest of the picture | 28.62 dB | 29.07 | 29.14 |
| head regions | 26.32 dB | 26.25 | 26.25 |
| needle pass, ratio | 3/3, 0.98 | 3/3, 0.99 | 3/3, 0.99 |
| blockiness mean / max | 1.065 / 1.141 | 1.063 / 1.141 | 1.062 / 1.141 |
| evaluator GPU time per frame | 5.6 ms | 4.2 ms | 4.1 ms |

Blind reviews of adjacent pairs (40 regions each, at most two per capture,
which version is which randomised per region):

| old → new | where | new | old | same | confident (high) new / old |
|---|---|---|---|---|---|
| baseline → PictureOnly | whole picture | 29 | 9 | 2 | 7 / 2 |
| baseline → PictureOnly | within 320 px of the edges | 28 | 12 | 0 | 9 / 1 |
| PictureOnly → + EdgeRules | edges | 26 | 8 | 6 | 5 / 0 |
| baseline → both | whole picture | 27 | 10 | 3 | 12 / 1 |

- **What it loses, and why it stayed a test switch until 4.7:** the bars were
  protecting what stands still near them. In the cave game of 11-15 the HUD
  icons 30–140 px from the picture's left edge come out worse in most pairs
  of the three fast-moving captures (a faded icon, a doubled digit, blocks
  beside them); the two slow captures are unchanged. It was a confident
  loss in all three reviews against the baseline. On a 16:9 screen there are
  no bars, so that HUD is drawn this way there already. The edge rules do
  not repair it: thin or see-through HUD parts over fast scenery are not
  found still by the search, and the persistence that protects them
  (`near_still`) needs a still cell nearby, which the bar supplied.
- **Skip-one misleads at the edges:** it doubles the step, and a sharp strip
  a few pixels off scores under a smear. The frame the rules "lost" most on
  showed a clean streak against a torn patchwork. Judge edges on adjacent
  pairs.
- **Checked in the add-on** (no card: a 960x720 test pattern in a 1280x720
  window, bars of 160 px): the log's "PictureOnly=1: the picture is 960x720
  at 160,0", 30 real + 29.6–29.9 generated fps in SDR and HDR, the bars
  identical to frame a, and in HDR the add-on's own frames 0.001–0.06 % of
  pixels from the evaluator with the same switches (0.14–0.86 % from the
  evaluator with them off).
- **The switches while playing:** both (and `HistorySearch`,
  `DebugMarker`, `LogStats`) are checkboxes under "Glasir Frame Generation"
  on the Add-ons page of ReShade's overlay (Home), so one can be flipped on
  the scene in question. Tried on the test pattern: on, off and on again,
  the log's "PictureOnly=1: the picture is ..." / "PictureOnly=0: the whole
  1280x720 back buffer again" half a second after each (15 presents at
  30 fps), the marker at the picture's corner or the frame's accordingly.
  - Trying it found a fault of the first version: once made for a picture,
    the generator was never made for the whole frame again (the "box that
    failed" is all 0 when none did, which is also "the whole frame"). With
    the switch only in the file that needed the picture to grow to fill a
    back buffer of the same size; it would then have kept drawing into the
    old box.
- **With the card** (11-15 evening, the owner: 3440x1440 HDR, DLSS 5 on,
  `HistorySearch=1` as well, two games, five F9 captures with both
  switches on):
  - *In play:* "didn't seem worse". Both were flipped off and on twice from
    the panel in the first game (half a minute off in all); the second game, the
    cave game, was played with them on only. 30.0 real + 30.0 generated fps
    either way.
  - *The wiring:* the add-on's frames match the evaluator's with the
    switches on (pairs 2-4: 0.03-0.38 % of the picture's pixels apart,
    3-15 % from the evaluator with them off), the bars are frame a's.
  - *Skip-one:* 29.61 → 30.13 dB (3 captures better, 2 the same); edge
    strips 25.77 → 27.20 dB (9 of 10 frames better), the rest 28.43 →
    28.81.
  - *Blind review* of the 20 regions of the adjacent pairs that differ
    most (four per capture; by the author of the change, which is which
    read afterwards): new 7, old 9, same 4; confident 2 / 2. It splits by
    game:
    - the first (a temple; minimap, quest line and speech bubbles at the
      picture's edges): new 6, old 2, both barely. An NPC at the left edge
      whose half was a smear is whole, a speech bubble's garbled text can
      be read (skip-one there 20.6 / 26.3 → 24.6 / 32.5 dB);
    - the cave game: new 1, old 7. Four are the HUD icons at the left
      edge in all four pairs of one capture, the loss above (an icon gone
      or in pieces; skip-one in that corner -0.6 to -1.1 dB on one frame
      of each of the three captures, never better). Three are a drone in
      the middle of the picture, torn either way and within 0.1 dB
      skip-one: the moved block grid, not the switches.
  - So the loss is still there and was not seen in play. A default wants
    the HUD protected first: 4.7.
- **Not known:** the GPU time next to DLSS 5 (the generator works on 74 %
  of an ultrawide frame's pixels, plus two copies of the picture; the
  rates above show no difference), the cave game with the switches off
  and on in one session, and a window being resized (the box is followed,
  but only the test window's one change was tried).
- **Also learned:**
  - content-based bar detection does not work on these captures: the bars
    are a flat 0, 256 or 640 of 65535 depending on the session, and faint
    spill reaches 2–50 px into them (boxes found: x = 30–439);
  - cropping moves the block grid, and that alone moves single frames by
    ±1 dB all over the picture;
  - on a 16:9 screen the cut limit is the stricter one today (no bar cells
    in the count): the fight frame above would be repeated there. Not
    changed; it wants 16:9 captures of fights and of real cuts.

### 4.7 A HUD over fast scenery (11-15/16)

**Status:** on by default since 11-16 (4.8): `StillHud` (the evaluator's
default too, `--set hud_detail 0` is without it; when this was written it
was off and `--set hud_detail 0.04` turned it on). With it off the frames
are as before and WarpPS compiles to the same instructions. First run with
the card on 11-16 (4.8).

It is what 4.6 lost with the bars, and what a 16:9 screen never had: icons
and digits over the scenery in a fast turn came out faded, in pieces or
doubled.

- **What goes wrong** (the cave game, `debug_view` 1, 4 and 10): a HUD's
  cells take the scenery's motion - most of their pixels are scenery. Then
  - the rule that keeps what both frames show (WarpPS's persist) is off: it
    holds only near a cell that stands still;
  - the scenery is on offer from every cell around, also drawn from one
    frame alone at its fixed cost: it shows through an opaque icon;
  - a's and b's blocks there move too, so a one-frame option may take the
    icon's own pixels along: a faint copy of a digit half a step (70-110
    px) from the real one.
  Beside a bar none of this happened: the coarse blocks, held still by the
  bar, made every cell near it stand still - the scenery's as well, wrongly.
  With persist allowed everywhere (a quick test, not kept) the icons came
  back and the ten captures of 11-15 lost 0.18 dB: it has to be told where.
- **Static detail** (`StaticDetail` in MidCS, per cell that moves): the
  pixels with an edge (luma gradient 0.04 and up in a and in b) that is the
  same edge in both frames where it is - as bright within a quarter of the
  contrast, facing the same way within half of it. Three kinds of scenery
  pass that, and each was found the hard way:
  - *stairs in a turn:* a step's line crosses its own moved copy at a
    shallow angle and is the same edge there in both frames. The stairs
    came out in patches (20261029-185347-372: -0.61 dB). So the edge must
    lie across the cell's motion (`hud_across` 0.5); one along it says
    nothing.
  - *pillars in a pan of 130 px a frame and more:* one pillar's edge lands
    where another's was, and the motion found is a few pixels off, so "the
    same again where the motion takes it" fails by a hair. A patch of
    pillar stood still (two confident losses in the first blind review).
    The test now looks 1 px + 3 % of the motion's length further and nearer
    along it (`hud_slack`).
  - *a cart's leg, the same way:* there the cell's own motion was the
    wrong one of the two edges while a's and b's blocks had the pan. Their
    motions explain an edge as well.
- **Two designs.**
  1. *In WarpPS* (dropped): cells with static detail count for `near_still`,
     near them the one-frame options pay for bringing something else, and
     `StandsStill` is asked there whatever the option costs. It worked
     (HUD error as below, captures +0.01 dB). But the evaluator's warp stage
     went from 3.16 to 4.59 ms **with the rule off** - 3.45 after the first
     addition (nine texture loads in a branch never taken, one debug view),
     4.17 after two more arrays of ten floats. `[branch]` on everything
     changed nothing. WarpPS is at a size where anything added to it costs,
     taken or not.
  2. *HoldCS* (kept): a cell with 6 such pixels (of 64), and the cells
     right around it, get no motion - at the frame in between and as a's and
     b's blocks (the three grids coincide at level 0). WarpPS is untouched
     and treats the HUD as it treats anything that stands still. Cost: not
     measurable (4.14-4.28 ms a frame before, 4.16-4.28 with it on; the
     "motion at t" stage 0.40 → 0.48 ms).
- **Numbers** (the final code; "switches" = `PictureOnly` + `EdgeRules`):

| | switches | + StillHud | whole frame | + StillHud |
|---|---|---|---|---|
| clips, mean | 44.11 dB | 44.08 (`hud` -0.30, `noisy_hud` -0.11, `subs` -0.07, `orbit` -0.05) | 42.93 | 42.90 (the same four) |
| captures, skip-one (32) | 31.05 dB | 31.06 (2 better, 0 worse) | 30.35 | 30.36 (2 better, 0 worse) |
| the owner's ten of 11-15 | 30.57 dB | 30.60 (3 better, 0 worse) | 30.16 | 30.17 (1 better, 0 worse) |
| ... with `HistorySearch` | 30.58 dB | 30.61 (3 better, 0 worse) | | |

  HUD error (`eval\hud_score.py`: levels of 255 off the pixels that are the
  same in all five real frames, on the four adjacent pairs):

| capture | whole frame | switches | + StillHud | whole frame + StillHud |
|---|---|---|---|---|
| cave game, 20261115-021952-376 | 1.95 | 7.64 | 1.95 | 1.93 |
| 021959-650 | 2.83 | 24.14 | 0.88 | 1.09 |
| 022021-405 | 2.44 | 6.02 | 0.98 | 1.10 |
| 022029-781 | 0.77 | 0.79 | 0.79 | 0.76 |
| 022041-127 | 1.11 | 1.25 | 1.24 | 0.96 |
| 214452-990 | 4.99 | 6.80 | 2.33 | 2.76 |
| 214603-809 | 1.43 | 1.34 | 1.15 | 1.05 |
| 214613-851 | 1.65 | 21.92 | 1.14 | 1.25 |
| another game's digits, 20261031-144655-466 | 5.87 | 3.37 | 1.69 | 2.25 |

  The other 33 captures have a HUD with opaque panels of its own: 0.34 to
  0.99, every version within 0.06.
- **Blind reviews** of the adjacent pairs (a fresh Claude agent each time,
  the regions that differ most, at most two per capture, which is which
  randomised per region), switches against switches + StillHud:
  - the hold without the last two fixes: new 13, old 6, same 4 (confident
    6 / 2). All 12 HUD regions went to the new one; the two confident
    losses were the pillars above.
  - with the pillar fix: new 13, old 2, same 5 (confident 8 / 1). The
    confident loss was the cart's leg; after the fix for it those frames
    are the ones the switches alone draw. Not reviewed again after it.
- **What it costs:** the scenery in a held cell stands still with the HUD -
  a cell (16 px) around a panel's edge, a double image there. That is the
  `hud` clip's -0.30 dB (a cloak passing the panel). Narrower holds were
  tried on the ten captures, each kept more of the clip and less of the
  HUD (mean HUD error, `hud` clip): no cells around 0.83, -0.10; the cells
  around only as a's and b's blocks 0.77, -0.20; only cells around that
  are themselves mostly unchanged 0.81, -0.10; that with half the
  threshold and a looser ring 0.65, -0.85; all cells around (kept) 0.61,
  -0.39 before the three fixes above. What protects a small icon is that
  the scenery cells around it stand still, not its own inside.
- **Checked in the add-on** (no card; a copy of the app folder):
  - five frames of a cave capture played in a loop (1280x720 of it, SDR):
    with `StillHud=0` the add-on's frames are the evaluator's with the rule
    off on 95-98 % of the pixels the rule changes (3 % with it on), with
    `StillHud=1` the evaluator's with it on on 95-100 % (2 % off). HUD error
    in the add-on's own frames 17.7 → 3.6.
  - the checkbox on ReShade's Add-ons page (posted keys): on and off, the
    log's "StillHud=1 (from the panel)", the value in ReShade.ini.
  - a scrolling picture with boxes drawn over it says nothing: the search
    holds such a HUD by itself, and the rule changes no pixel.
  - `tools/smoke_raw.py`: HDR passed twice. SDR is marginal in its five
    seconds that night with either add-on (the picture stands for the first
    five while DLSS 5 starts): yesterday's passed 3 of 4, this one 2 of 4,
    and with 12 s it passes.
- **Leftovers:**
  - faint copies of a see-through icon or a digit beside the HUD in the
    fastest turns (fewer than before: 76 → 15 pixels in the worst pair
    under the first design; the hold was not counted);
  - at 130 px a frame and more near the picture's edge, with `PictureOnly`,
    a dark block of scenery beside the icons (a motion of -214 px found,
    both samples dark). The scenery's own matter; the bars hid it by
    holding everything there still.
- **Not known:** SDR games (the thresholds are on encoded luma, PQ here);
  HUDs of other games - large see-through panels, a HUD that itself moves
  or pulses. In play: 4.8.
- **Also learned:**
  - `fg_eval` is not always the same twice: on a few frames (9 of 504 in
    one run of everything) small patches on the main character differ
    between runs of the same build, the one from before this round
    included. It is in the layer passes; not looked into. Compare more than
    one run before calling a frame changed.
  - Time the stage against the build before, with the new thing off:
    `fg_eval --profile`, the builds in turn, three rounds. A single run
    moves by 1 ms with what else the GPU does.

### 4.8 The picture alone, the edge rules and the HUD hold by default (11-16)

`PictureOnly`, `EdgeRules` and `StillHud` are on unless `ReShade.ini` says
otherwise. `HistorySearch` stays off.

- **Why now:** the owner played with all four on (11-16; 3440x1440 HDR,
  DLSS 5 on, the picture 2560x1440 at 440,0 by the log): "looks good in
  use". It is the first time `StillHud` ran with the card.
- **Offline, whole frame → the three** (4.6 and 4.7 have the steps): the 32
  captures 30.35 → 31.06 dB skip-one (28 better, 2 worse), the owner's ten
  of 11-15 30.16 → 30.60 (8 better, 2 worse), the clips 42.93 → 44.08 (no
  bars in them: the edge rules and the hold). The HUD error of 4.7 is at or
  under the whole frame's but on two captures (+0.02 and +0.13 of 255).
- **A set:** `PictureOnly` without `StillHud` has 4.6's loss (the HUD near
  the picture's edge), so neither is a default without the other.
- **`Quality=fast`** (`warp_ring` 2, `warp_taps` 1, `soft_temp` 0.02) was
  measured for this, as nothing before had been:

| fast | whole frame | PictureOnly + EdgeRules | + StillHud |
|---|---|---|---|
| clips, mean | 41.19 dB | 42.67 | 42.65 (`hud` -0.26) |
| captures, skip-one (32) | 30.20 dB | 30.89 (28 better, 3 worse) | 30.90 (2 better, 1 worse by 0.02; from the whole frame 29 / 2) |
| the owner's ten | 30.00 dB | 30.40 | 30.44 (3 better, 0 worse; from the whole frame 8 / 2) |

  The HUD error with the three is at or under the whole frame's on the cave
  captures but for one (1.74 against 1.70). The hold is no slower in fast
  mode (a frame 7.1-8.0 ms with it, 7.4-9.1 without, the GPU busy with
  something else that evening).
- **`HistorySearch` not:** the owner has it on, but what was measured on
  the games played since says it does little there: +0.005 dB on the ten
  with the three on (30.602 → 30.607), nothing clearly better in 4.5's
  review of the cave game. Its gains are 4.5's, on the prototype's captures.
- **How a default is kept.** The add-on writes every setting's default into
  `ReShade.ini` at its first start and the file rules from then on, so a
  default that has shipped once cannot be changed for anyone who ran it.
  The four switches are kept another way (`switch_setting` in
  `glasir_framegen.cpp`):
  - a missing key is the default, and stays missing;
  - a key that says what the default says is taken out, at start and when
    the panel puts a switch back (ReShade removes a key given a null value,
    6.4 on; the add-on is built for 6.8's interface and loads in nothing
    older);
  - so the file holds a switch only while someone chose against the
    default, and a later version can change it for everyone else;
  - `ReShade.log` says `switches: PictureOnly=1 EdgeRules=1 StillHud=1
    HistorySearch=0` at start, as the file no longer does (bug reports).
  None of the four was in a release (0.2.0 has none), so nobody's file
  holds an old default.
- **The evaluator follows:** `fg::Interpolator` comes with the edge rules
  and the hold, so `fg_eval` without `--set` scores what ships (with
  `--picture` on captures with bars). As before them: `--set edge_cost -1
  --set edge_explains 1 --set hud_detail 0`. Checked: without flags the
  clips and the 32 captures score line for line what 4.7's flags did (44.08,
  31.06 dB), with the three off what the whole frame did (42.93, 30.35).
- **Checked in the add-on** (no card; a copy of the app folder, the 960x720
  pattern with boxes in a 1280x720 window, SDR):
  - the owner's file (all four at 1): the three keys are gone after the
    run, `HistorySearch=1` stays;
  - no switch in the file (someone coming from 0.2.0): the three on
    ("PictureOnly=1: the picture is 960x720 at 160,0", "StillHud=1: ..."),
    30 real + 29-30 generated fps, the file still without them;
  - `PictureOnly=0` and `StillHud=off` are used and left as written,
    `EdgeRules=maybe` is warned about and left, `HistorySearch=0` is taken
    out;
  - the panel (posted keys): Still HUD, Picture only and History search
    each off and on - away from the default the key is written, back at it
    the key is gone and the log says "(from the panel; the default)";
    "PictureOnly=0: the whole 1280x720 back buffer again" and back.
  - ctest 4 of 4. `tools/smoke_raw.py --format p010 --hdr` failed its first
    three runs on that copy and then passed 4 of 4, and 3 of 3 with the
    three switches off: the picture stands for the first five seconds while
    DLSS 5 starts ("mpv fell behind: 150 of 165 new frames dropped"), with
    the switches on or off, and the test measures seconds 2 to 5.
- **Not known:** SDR games and a 16:9 screen in play (the picture alone
  *is* a 16:9 frame, so the offline numbers cover what is drawn there);
  other games' HUDs (4.7 found three kinds of false HUD in one evening on
  three games); other GPU makes.
- **Also learned:** a Space posted to ReShade's overlay is lost now and
  then when the real mouse has moved since the last arrow key (the owner at
  the PC; ImGui seems to leave keyboard navigation). Post an arrow key
  first, and read the log for what was switched.

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
    "off screen" checks don't fire at the picture's edge (that alone costs
    nothing, section 4.6); score them with `--picture 440 0 3000 1440` to
    see what PictureOnly draws (the add-on's default since 4.8);
  - the owner prunes captures: never put deleted ones back.

## 10. Known leftovers and open ideas

- **Leftovers:**
  - a spike that turns while the head drifts comes out soft or cut;
  - a faint wisp beside a restored spike;
  - a blocky hair fringe in one orbit;
  - NPC fragments torn over hair when an NPC slides in behind the head;
  - a whole character smears when body and hair move differently (the
    dodge);
  - a HUD near the picture's edge over fast scenery: thin and see-through
    parts fade or double with `StillHud` off (section 4.6). On, as it comes
    (4.7, 4.8), it holds them at the price of the scenery right beside
    them; faint copies of see-through parts remain;
  - small patches on the main character that differ between two runs of
    the same build on the same frames (4.7).
- **Ideas not pursued:**
  - finer motion at object edges (4x4 on boundary cells only);
  - x3/x4 (needs the pacing generalised);
  - frame generation on the second GPU;
  - an offline VFG comparison;
  - a z-order rule that keeps NPCs solid without hair artefacts.

# Contributing to Glasir

Thanks for helping. A few rules keep the licensing clean.

## Licences

- Everything is **MIT**, except the frame generation code in
  `src/framegen/`, which is **MPL-2.0**.
- Every source file starts with its licence tag, in the file's comment style:

  ```cpp
  // SPDX-License-Identifier: MIT
  ```
  ```cpp
  // SPDX-License-Identifier: MPL-2.0
  ```

- Changes to MPL-2.0 files stay MPL-2.0. Don't move frame generation code
  into MIT files to get around that, and don't copy code from other
  projects unless its licence allows it; say where it came from.

## Sign your commits (DCO)

By contributing you certify the [Developer Certificate of Origin](https://developercertificate.org/):
that you wrote the change or have the right to submit it under the
project's licences. Add a sign-off line to every commit:

```
git commit -s -m "Describe the change"
```

which adds `Signed-off-by: Your Name <you@example.com>`.

## What doesn't go in the repo

- Third-party binaries (mpv, ffmpeg, ReShade, DLSS DLLs, add-ons). The
  app downloads them on the user's PC (`src/app/components.cpp`).
- Game footage or screenshots. Evaluation tools read captures from a local
  folder outside the repo.
- Third-party source with an unclear licence.

## Building and testing

See [README.md](README.md#building). Run `build.cmd` before sending a
change; it builds everything and runs the tests. Changes to capture,
frame generation or pacing should also be tried with a real capture card
when possible; say in the pull request what you tested on (GPU, card,
console).

## Layout

```
src/capture/    finding cards, reading frames, dedup (library)  MIT
src/app/        Glasir.exe: settings, starting mpv, frame pump  MIT
src/common/     shared with the add-ons (frame gen link)        MIT
src/audio/      WASAPI audio mpv plugin                        MIT
src/framegen/   frame generation ReShade add-on + shaders      MPL-2.0
tools/          developer scripts (fetch mpv, smoke tests)     MIT
third_party/    vendored headers with their licences           (see there)
tests/          build and unit tests                           MIT
docs/           plan, guides, troubleshooting                  MIT
```

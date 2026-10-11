+++
path = "/tools/ab-capture"
title = "A/B capture harness and visual council reviews"
kind = "playbook"
status = "experimental"
summary = """
tools/orbit_ab.py compares renderer variants on screen: it pins world, clock and camera, captures converged frames over RPC, and writes a report and contact sheet; its council command persists a rubric-scored review under docs/reviews/. Two modes: relaunch Studio per variant (any env flag), or in_process (one launch, cloud shader tunables swapped with clouds.set_defines, rounds interleaved so drift cancels). tools/build_studio.ps1 is the 15 s inner-loop build."""
keywords = ["ab test", "screenshot", "capture", "council", "visual review", "flag", "ORBIT_CLOUD_DEFINES", "noise floor", "determinism", "frame time"]
sources = [
  "tools/orbit_ab.py",
  "tools/ab_specs/refract_sun.json",
  "tools/ab_specs/cloud_march.json",
  "docs/reviews/RUBRIC.md",
  "engine/celestial_clouds/src/CloudRenderer.cpp",
]
symbols = ["ORBIT_CLOUD_DEFINES", "CLOUD_JITTER_FIX", "CLOUD_INCLOUD_STEP_SCALE", "ThinLayerStepCap"]
invariants = [
  "Every variant runs in a freshly launched Studio whose environment holds exactly that variant's ORBIT_* variables; every Orbit* process is killed first, because a hot-generation child (OrbitStudio_hot_*) survives killing Orbit and keeps the RPC port and the old environment.",
  "A difference only counts against the noise floor: the baseline is launched again as a control, and a variant is DIFFERENT if its mean difference exceeds both 3x and noise+0.05 of the control, or its changed-pixel count exceeds 3x the control's plus 500 (a mean alone hides a thinned cloud bank). Frame-time deltas are compared with the control's frame-time drift (about 1.2 to 1.4 ms on the dev machine).",
  "Captures are taken at convergence (two consecutive screenshots within converge_eps), not after a fixed sleep; the sim clock is pinned per scene.",
  "A council review is immutable (id R-<date>-<time>-<commit>), records the commit and the dirty files, the exe hash, GPU, per-scene image hashes, the measured metrics and the lenses' findings and rubric scores; subjective scores never replace measured values.",
  "ORBIT_CLOUD_DEFINES (startup) and clouds.set_defines (runtime) are developer hooks: their defaults must reproduce the shipped shader path.",
  "in_process mode alternates variant order per round (A B C, then C B A); the baseline's round-to-round frame-time spread is the drift, and a profiler pass time comes from ONE snapshot of the panel's rolling window with freeze_on_hitch off (a frozen panel returns the same snapshot for every scene, and two snapshots cannot be subtracted).",
  "tools/build_studio.ps1 builds only OrbitStudio (no configure, OrbitBuild, OrbitPlayer or packaging) and stops every Orbit* process first; editing engine source while Studio runs starts a hot handoff instead.",
]
related = ["/editor/mcp-rpc", "/rendering/clouds/raymarch", "/rendering/unified-transport"]
verify = [
  "python tools/orbit_ab.py run tools/ab_specs/refract_sun.json --project <scratch project>: the sun is DIFFERENT with ORBIT_REFRACT_SUN=1 (1.6 to 3.2/255 mean against 0.05 noise).",
  "python tools/orbit_ab.py history lists saved reviews.",
]
verified = "44e6475"

[[diagnose]]
symptom = "all variants look identical or the flag seems to do nothing"
steps = [
  "Check for a stale hot-generation process: Get-Process Orbit* (not Orbit). Editing engine source while Studio runs starts one; close Studio before editing, and the harness kills every Orbit* itself.",
  "The scratch project may start in another world (GI_Room): pass world in the spec so the harness opens it.",
]
docs = ["/tools"]
+++

## Use

Make a scratch copy of an Earth-system project (world edits and the sim clock are saved on close), then:

```
python tools/orbit_ab.py run tools/ab_specs/cloud_march.json --project <scratch dir> [--out DIR] [--resume]
python tools/orbit_ab.py council <out DIR> findings.json
python tools/orbit_ab.py history [--scene NAME]
```

Speed: a 24-capture comparison (4 shader variants x 3 scenes x 2 rounds) takes 174 s in one launch, each capture converging in about 2.4 s; the relaunch mode needs a ~25 s launch plus 7 to 13 s per scene per variant and a repeat launch for noise. Build: `powershell -File tools/build_studio.ps1` took 15 s for a one-file change (build_orbit.bat configures, builds three targets and packages).

A spec lists variants (env vars), scenes (clock, pose `surface` or `approach`, optional view layers and a pixel box) and `controls`. `--resume` reuses variants
already captured in `--out`. Outputs: `<out>/<variant>/<scene>.png`, `report.json` and `sheet.png` (top row variants, bottom row 5x difference against the baseline).

## Council

Judge `sheet.png` and zoomed crops with the lenses in `CLAUDE.md` (Shader Artist, Perceptual Realist, Vulkan/GPU/CPU engineers, Architectural Skeptic, Minimalist),
score with `docs/reviews/RUBRIC.md` (null = N/A), keep hypotheses apart from verified facts, then persist with `council`. The first review is
`docs/reviews/R-20261011-175457-44e64759`: five cloud-march sampling variants, verdict keep the baseline.

## In-process mode

`"mode": "in_process"` with `rounds` (default 2): one launch, then for each scene and round every variant's `ORBIT_CLOUD_DEFINES` is sent with `clouds.set_defines`
(about 1 s to recompile the march, shadow and volume shaders; a compile error fails the run and the previous shaders stay in place). Only
variants that differ by those defines can be compared this way; env flags read once at startup (`ORBIT_REFRACT_SUN`, `ORBIT_CLOUD_POINT_LIGHTS`, ...) need the relaunch mode.
Set `gpu_passes` (for example `["Clouds"]`) and `gpu_window_ms` for per-pass GPU time.

## Motion capture

A scene with a `motion` block (`kind` pan, pitch, ascend or move: degrees or metres `per_step`; for an `approach` pose, `alt_km` per step; `steps`;
`frames_per_step`) is run as a camera path in `in_process` mode, for judging temporal techniques that stills cannot show. For each variant:
warm up a fixed `warmup_frames` (default 150), advance the pose every `frames_per_step` rendered frames (synchronised on the profiler frame counter) and grab each frame;
then a static camera is grabbed for ten consecutive frames. Reference = the baseline shaders at every pose of the path after `reference_settle` frames,
averaged over `reference_frames` frames (a stop-at-stable-mean reference is not converged in the cloud band and shows up as an error that decays with path step).
`box_rel` ([x0, y0, x1, y1] in 0..1) limits the metrics to a region such as the cloud band. Reported per variant (mean of the rounds):

| Metric | Meaning |
| --- | --- |
| `err_mean`, `err_max` | mean absolute error of each in-motion frame against the reference, after scaling the frame to the reference's mean (auto-exposure adapts over seconds), /255 |
| `ghost_frac` | share of pixels over `ghost_threshold` (default 3/255); lag at cloud edges AND temporal noise both count |
| `flicker` | static camera, mean over pixels of the standard deviation over ten consecutive frames, /255 (temporal noise) |
| `exposure_drift` | mean absolute exposure gain applied |
| `err_spread` | the baseline's max-min of `err_mean` over rounds: the metric's own noise |

`motion_<scene>.png` plots the error by step and the flicker bars. Read the two together: more history lowers flicker and mean error (less noise) while raising the
share of pixels off by lag, so a variant is judged on both. Limits: `frames_until_grab` includes the screenshot's own latency (about 13 frames) so "one frame since the
pose change" is not guaranteed, only that every variant gets the same protocol; error mixes noise and lag; the shape along the path follows the content the camera sweeps over.
Validation (`tools/ab_specs/motion_resolve.json`, history weight 0.1 / 0.2 shipped / 0.4 / 1.0): flicker rises with the current-frame weight (0.049 / 0.059 / 0.077 / 0.124 in
the slow pan), the error of the fast pan is about twice the slow one for the shipped weight (0.20 vs 0.11), and the baseline's error spread over rounds is 0.001 to 0.006. A smoke run
(8 steps, two variants, one round) takes about a minute; the full validation about nine.

## Developer hook

`ORBIT_CLOUD_DEFINES="NAME=VALUE;NAME=VALUE"` becomes `#define` lines in every cloud shader at startup, and `clouds.set_defines` (MCP `orbit_clouds_set_defines`) does the same in the running Studio. Tunables today: `CLOUD_FAR_EARLYOUT`, `CLOUD_JITTER_FIX`, `CLOUD_INCLOUD_STEP_SCALE`, `CLOUD_MAX_STEPS`, `CLOUD_SCHEDULE_STEPS`, `CLOUD_FOOTPRINT_STEP`, `CLOUD_BASE_STEP`, `CLOUD_EMPTY_SKIP`, `CLOUD_EMPTY_COVERAGE`, `CLOUD_SUN_FAR_START`, `CLOUD_SUN_FAR_END`, `CLOUD_SUN_VOLUME`, `CLOUD_RESOLVE_WEIGHT` and the `CLOUD_PROF_*` profiling stubs. Add a tunable by guarding it with `#ifndef` and a default
that equals the shipped behaviour.

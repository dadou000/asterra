+++
path = "/tools/ab-capture"
title = "A/B capture harness and visual council reviews"
kind = "playbook"
status = "experimental"
summary = """
tools/orbit_ab.py relaunches Studio once per variant (env flags), pins world, clock and camera, captures converged frames over RPC, runs a determinism control, and writes \
a report and contact sheet; its council command persists a rubric-scored review under docs/reviews/. ORBIT_CLOUD_DEFINES lets cloud shader tunables be A/B-tested without a rebuild."""
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
  "A difference only counts against the noise floor: the baseline is launched again as a control, and a variant is DIFFERENT only if its mean difference exceeds both 3x and noise+0.05 of the control. Frame-time deltas are compared with the control's frame-time drift (about 1.2 to 1.4 ms on the dev machine).",
  "Captures are taken at convergence (two consecutive screenshots within converge_eps), not after a fixed sleep; the sim clock is pinned per scene.",
  "A council review is immutable (id R-<date>-<time>-<commit>), records the commit and the dirty files, the exe hash, GPU, per-scene image hashes, the measured metrics and the lenses' findings and rubric scores; subjective scores never replace measured values.",
  "ORBIT_CLOUD_DEFINES is a developer hook: its defaults must reproduce the shipped shader path.",
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

A spec lists variants (env vars), scenes (clock, pose `surface` or `approach`, optional view layers and a pixel box) and `controls`. `--resume` reuses variants
already captured in `--out`. Outputs: `<out>/<variant>/<scene>.png`, `report.json` and `sheet.png` (top row variants, bottom row 5x difference against the baseline).

## Council

Judge `sheet.png` and zoomed crops with the lenses in `CLAUDE.md` (Shader Artist, Perceptual Realist, Vulkan/GPU/CPU engineers, Architectural Skeptic, Minimalist),
score with `docs/reviews/RUBRIC.md` (null = N/A), keep hypotheses apart from verified facts, then persist with `council`. The first review is
`docs/reviews/R-20261011-175457-44e64759`: five cloud-march sampling variants, verdict keep the baseline.

## Developer hook

`ORBIT_CLOUD_DEFINES="NAME=VALUE;NAME=VALUE"` becomes `#define` lines in every cloud shader at startup. Tunables today: `CLOUD_JITTER_FIX` (1 clamps the sample
jitter to the thin-layer step cap) and `CLOUD_INCLOUD_STEP_SCALE` (multiplies the in-cloud step). Add a tunable by guarding it with `#ifndef` and a default
that equals the shipped behaviour.

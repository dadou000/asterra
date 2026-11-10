+++
path = "/rendering/terrain/clipmaps/lattice-and-tracking"
title = "Lattice, tracking and toroidal residency"
kind = "concept"
status = "stable"
owner_module = "OrbitTerrainView"
summary = """
ClipmapTracker snaps each level's centre on one shared spherical lattice and reports per-level cell shifts; \
a stable anchor surface frame (not rotated by ordinary shifts) lets retained toroidal samples keep the exact \
same world-space address. ToroidalResidency turns shifts into strip refreshes. The exponential-map chart is \
rebased only rarely (about 150 km of travel on a 6000 km planet)."""
keywords = ["tracker", "lattice", "snap", "toroidal", "residency", "anchor frame", "rebase", "phase", "cell shift", "coordinate spaces", "camera relative"]
sources = [
  "engine/terrain_view/include/orbit/terrain_view/ClipmapTracker.hpp",
  "engine/terrain_view/src/ClipmapTracker.cpp",
  "engine/terrain_stream/include/orbit/terrain_stream/ToroidalResidency.hpp",
]
symbols = ["ClipmapTracker", "ClipmapLevelMotion", "ClipmapMotionUpdate", "ToroidalResidency", "InvalidateSamples"]
invariants = [
  "Level centres are snapped on a single lattice shared by all levels; snapping happens in this lattice frame, before any camera-relative conversion.",
  "The anchor surface frame is stable across ordinary cell shifts, so retained toroidal samples keep an identical world-space address; only a rebase changes it.",
  "A rebase of the exponential-map chart is deliberately rare (about 150 km of travel on a 6000 km planet) versus toroidal strip updates every sample cell.",
  "InvalidateSamples() refreshes data without relocating the stable sampling lattice; Reset() forgets everything.",
  "A terrain source revision must refresh through InvalidateSamples(), never Reset(): Reset relocated every grid around the current observer and therefore changed the sampling phase even for distant levels whose spacing and field were unchanged (problem tracker entry 'Terrain changes shape on source refresh / rebase', corrected 2026-09-13 and listed as awaiting visual confirmation in /legacy/problems).",
  "fullRefresh on a level means every sample of that level must be regenerated; otherwise only the reported strips.",
]
related = ["/rendering/terrain/clipmaps/precision-and-pages", "/legacy/problems"]
depends_on = ["/rendering/terrain/clipmaps"]
verify = [
  "ctest -R Orbit.ClipmapPlanner plus the terrain stream tests (engine/terrain_stream) for residency behaviour.",
  "To rule tracking in or out when terrain looks shifted: freeze the clipmap (clipmap_freeze) and compare; then check generator parity (/rendering/terrain/clipmaps/generator-parity).",
]
verified = "b0a0de7f"

+++

## Data flow

```text
observer WorldPosition
  -> ClipmapTracker::Update   -> ClipmapMotionUpdate { levels[]: cellShiftX/Y, centerDirection,
                                  surfaceFrame, centerOffsetMeters, fullRefresh }
  -> ToroidalResidency::Apply -> ResidencyUpdate { levels[]: originX/Y, fullRefresh, refreshRegions[] }
  -> GPU generation of just those regions
```

## Why it is built this way

A level is a window of fixed size whose contents scroll as the camera moves. Re-generating the whole level on
every shift is far too expensive, so levels are toroidal: only the strip that scrolled into view is refreshed
and the rest keeps its samples. That only works if a sample's world address does not change when the window
shifts, hence the shared lattice and the stable anchor frame. Keeping the chart bounded (rebasing rarely) keeps
float shader coordinates precise.

`ClipmapTracker::Reconfigured(config)` produces a tracker for a changed config; a clipmap-only config change
must not rebuild unrelated GPU generators (`/rendering/terrain/clipmaps/rebuild-hitches`).

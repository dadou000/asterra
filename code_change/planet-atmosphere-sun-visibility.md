# Planet and atmosphere sun visibility

The existing owners are DirectLightingRenderer (surface starlight) and
AtmosphereRenderer (view-ray scattering), and CloudRenderer (cloud sources and
aerial perspective). Extend their sun-source evaluation
with one shared shader helper owned by lighting. The current body radius and
the existing view depth buffer remain authoritative inputs; no world properties,
terrain generator or independent light source is introduced.

Analytic body occlusion must cover the entire night side. Screen-space geometry
occlusion supplements it for visible mountain relief, affecting direct single
scattering and surface sunlight while leaving multiple scattering intact.
Off-screen relief is outside this depth-buffer representation's coverage.
Embedded shader/native edits use the central automatic Studio generation
handoff; candidate compile failures preserve the current generation and state.


## Stable terrain shadow correction

Replace screen-depth traversal with TerrainSunShadowField owned by lighting.
The canonical TerrainSource is sampled into a disposable body-fixed global
heightfield on a background worker, keyed only by source identity/revision,
body and radius. StudioViewportRenderer installs validated candidates and pins
one immutable buffer for surface, atmosphere and cloud graph callbacks. Camera
movement, screen bounds, render LOD and sun direction do not rebuild this field.
The RHI defers old-buffer destruction until GPU submissions complete. Failed or
superseded candidates keep the active same-body field, with analytic planet
occlusion available while the first candidate builds. No watcher is introduced;
implementation/header/CMake changes use the central generation handoff.

The inspected KSP Scatterer config uses 8192 shadow-map resolution and cascades;
its installed terrain god-ray switches are disabled. Scatterer's public docs
connect god rays to shadow-map data. Orbit's heightfield uses the same underlying
principle of world geometry determining visibility, with global body coverage
rather than a camera-depth silhouette.


## Finite source and common transport

CelestialLightingService owns the apparent angular radius from the luminous
photosphere radius and receiver distance. The same value serves terrain, air,
clouds and mesh PCSS. Replace binary visibility with finite-disc covered area:
exact disc occultation for the inscribed body and an angular terrain-horizon
approximation from the same body-fixed field. Sample the atmospheric extinction
at each ground/cloud/air point using a common shader helper, with conditional
stellar-disc centroid sampling at the curved horizon. LUT altitude mapping is
quadratic to resolve the dense lower atmosphere; change its algorithm fingerprint
alongside all CPU/GPU consumers. Increase view-ray integration precision and
remove cloud-specific HDR gain/compression; keep scattering in scene-linear units.
Finite terrain horizon and multiple-scattering closures remain approximations.

## Shared cloud transport completion

The cloud volume carries a 64-byte self-description header and is published before
surface and atmospheric lighting. A common reader validates hemisphere, lattice
tags and source/medium generation. Ground, air and clouds read the same optical
depth; unready cloud samples use eight source-disc rays with 48 spherical altitude
slabs. Atmospheric cloud god-ray subtraction skips already-integrated samples.
Sun/weather changes invalidate voxels immediately; camera windows retain matching
cells. Immutable shared buffer ownership keeps graph callbacks alive and Vulkan
defers retirement. Embedded shaders reflect via the central automatic native
generation handoff, and failed candidate startup preserves the active generation.

Release build and live HLSL activation succeeded. Documentation check: 174 nodes,
zero errors/warnings. The live Earth System scene shows a continuous limb
transition (logs/unified-physical-framed.png). Its currently saved radius is
1,000,000 m; no planet dimensions were changed. Finite field/volume resolution,
the terrain angular-horizon approximation and low-dimensional multiple-scattering
LUT remain approximations; this is not a full spectral path tracer.

## Performance correction

The original 192-step air march nested terrain tracing at every sample; cloud
aerial perspective also bypassed the ready shared cache. The corrected air
march uses at most 96 extinction steps with shared direct sun at four-step
boundaries (at most 25 queries), interpolated continuously between boundaries.
Cloud aerial transport now reads the cache first; fallback uses four source-disc
rays and 24 altitude slabs, with early termination at optical depth 12. Source
parameter buffers are cached separately for their two shader roles.

A fixed-camera 1280x720 viewport-only Perfetto capture measured atmosphere GPU
time decreasing from 15.187 ms (215 frames) to 4.863 ms (506 frames), roughly
3.12x faster. These numbers describe that static scene, not a universal speedup.
The source-disc and sunlight reconstruction are bounded approximations, retaining
continuous penumbra and the shared transport model.

The close 100,000 m radius Earth view exposed a much larger nested cost than
the distant comparison: an intermediate 720p capture measured Clouds 80.857 ms
and Atmosphere 47.096 ms. The final bounded implementation uses 32 terrain
probes, 96 extinction steps with sunlight reconstructed at eight-step boundaries
(at most 13 queries), 16 cloud aerial intervals, 32 cache-miss god-ray intervals
and four stellar-disc rays with 16 altitude slabs. Body-blocked samples skip
cloud sunlight work. Final close-view capture measured Clouds 9.101 ms and
Atmosphere 5.716 ms over 209 frames. Traces:
- baseline: capture-1791891426748.json
- final: capture-1791891717249.json
under the real Orbit/profiler directory. These are pass costs at 720p, not a
universal frame-rate guarantee. Narrow relief can be missed at the runtime
32-probe sun-ray spacing; the 512-probe CPU reference remains unchanged.

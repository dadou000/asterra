+++
path = "/rendering/clouds/temporal-animation-and-particle-shading"
title = "Temporal clouds and particle-neighborhood shading — design record"
kind = "concept"
status = "proposed"
owner_module = "OrbitCelestialClouds"
summary = "2026-10-14 design discussion: lazy time-evolving cloud clipmaps, delayed presentation, velocity-compensated interpolation, independent animated erosion, and experimental particle/voxel-neighborhood shading from the Ultimate Particle System transcript."
keywords = ["cloud animation", "presentation delay", "motion interpolation", "advection", "erosion", "IFS", "chaos game", "voxel ambient occlusion", "particle shading"]
related = ["/rendering/unified-transport/medium-and-clouds", "/rendering/clouds/unified-system-b-morphology", "/rendering/clouds/shadows-and-light-volume"]
+++

# Agreed design direction (14 October 2026)

**This is a planning record, not an assertion of existing implementation or hardware validation.**

## Goals and scene behavior

- Clouds should be cached similarly to terrain clipmaps, but their contents **continue evolving while the camera is stationary**.
- World/storm-anchored authoritative regions persist and evolve independently of viewing. Camera-relative toroidal clipmaps are strictly for expensive *visual detail residency*; moving the camera does not create or destroy clouds or invalidate their physical identity.
- Global weather, regional convection, cloud density, optical properties and lighting refresh lazily at their own temporal and spatial rates. Old valid results remain readable until coherent replacements are ready.
- Atmosphere, cloud material, terrain shadowing and lightning must consume compatible time-stamped optical state, with one unified transport authority; do not create an independent private light model solely for particles.

## Proposed cloud animation

```
Weather + storm evolution (world anchored, lazy asynchronous)
           |
       immutable physical snapshot A(t0), B(t1)
           |                         |
   advect A forward           advect B backward
           +------------+------------+
                        |
           interpolate aligned morphology
                        |
           add continuously evolving erosion noise
           (advected/sheared locally; footprint filtered)
                        |
                 displayed optical density
                        |
         shared illumination / shadows / camera march
                        |
                    linear HDR
```

**User proposal:** The *visible* cloud state lags slightly behind the simulated world, providing a pair of **completed** bounding snapshots to interpolate. Separate continuously evolving noise animates erosion of the cloud boundaries between expensive simulation updates.

**Additional technical refinement:** First advect both states toward the presentation time using regional 3D velocity, including vertical updrafts and shear, then interpolate *aligned* states. A simple density lerp of displaced clouds causes doubles, ghost silhouettes and vanishing opacity. Condensation/evaporation and cloud births/deaths require source-term-aware fades/mass correction; velocity compensation alone is insufficient.

**Presentation time and latency:** Use t_present = t_world - D with adaptive, bounded D based on snapshot completion and acceptable latency. Don't stall normal graphics waiting for a future snapshot. When compute falls behind, use bounded prediction or lower optional fidelity; don't allow unbounded growing delay. Extreme timewarp seeks absolute simulation time and cancels obsolete tasks.

**Animation bands:**
1. Slow planetary/cloud-deck coverage and translation: weather-derived coarse state.
2. Convective/storm structural evolution: regional simulation and velocity-compensated interpolation.
3. Medium-scale billows and fine wisps: continuous turbulence/noise advection in the cloud's local flow, not a uniform scrolling texture.
4. Illumination: its own dependency-driven asynchronous cadence; lighting can change without requiring the bulk cloud morphology to regenerate.

The erosion field should be deterministic in space and simulation time, smoothly evolving across snapshot boundaries, and aggressively filtered when its projected detail falls below a pixel. Fine erosion is not allowed to change global cloud identity. View and transport sample one compatible density field; coarse filtered footprints are permitted. Cloud shadows and atmospheric scattering should not visibly slide independently of the silhouettes.

**Events:** Physically simulated precipitation and lightning are referenced to compatible presentation time. A pre-generated discharge can be staged for coherent light+arc activation; unexpected lightning gets a bounded immediate direct/near-scattering fallback. Player-operated lights respond immediately *on the presented cloud*, not delayed with slow weather simulation. Distinguish simulation delay from interactive-light latency.

**GPU ownership:** Cache tiles have region, simulation time, source version, completion serial and quality/error bounds; recycled toroidal slots cannot overwrite in-flight readers. Publish completed dependency-consistent optical generations. Camera relocation and floating-origin shifts never alter the physical cloud. Disable/rebase image history on teleports and invalid motion.

## Research: Ultimate Particle System transcript (provided by user)

The transcript's central techniques:
- **IFS and chaos game:** many GPU threads independently apply weighted, contractive affine transforms to particle positions to generate complex fractal attractors. Morphing is more natural when *individual transformations* (scale, rotation, translation, shear) are interpolated before reconstructing affine matrices, rather than blending raw matrices or two full particle position buffers. Smoothed frame-rate-independent exponential convergence avoids abrupt transformation changes.
- **Compute-updated point meshes and instancing:** efficient for the showcased attractor point rendering, but benchmark outcomes are hardware- and primitive-dependent. Do not infer that 100M visible cloud particles are desirable, cheap, or necessary for Orbit.
- **Coarse voxel neighborhood shading:** voxelize particle positions into a lower-resolution occupancy/density grid, estimate occlusion from occupied neighbors, then sample that value when shading *every* high-detail particle. This was a crude **ambient occlusion proxy**, not true directional scattering, atmospheric extinction or multiple-scattering transport.
- **Postprocess:** Kuwahara-style filtering and bloom made the granular particles look soft; such stylization may obscure physically desired cloud detail and should only be a separately evaluated appearance experiment.

### Application and caveats for Orbit

**Promising experiment:** Generate microbillows or supplemental morphology using constrained/weighted 3D IFS in selected convective regions. The cloud's weather-solver structure, mass/optical properties, storm lifecycle and velocity remain authoritative. Avoid switching attractors independently of velocity or condensation. Test reproducibility and volume stability; IFS attractors are shape generators, not fluid solvers.

**Shading experiment:** Reuse the *principle* of a dense geometric representation being illuminated from a coarse shared medium. Generate a local occupancy/density neighborhood summary, optionally directional/anisotropic, and query it for per-particle shade or per-raymarch-sample microstructure response. Prefer using the existing hierarchy or sparse optical transport cache over maintaining a redundant lighting grid.

- An unsigned occupied-neighbor count estimates geometric enclosure, not how much sunlight or skylight reaches the location.
- If adding darkening to an already attenuated cloud, beware double-counted extinction/occlusion and unnaturally soot-black interiors.
- Silver linings and bright cavities depend on phase function and multiple scattering. Keep an explicit transport source and energy-conserving/visually bounded contribution; do not multiply by an arbitrary darkening factor and call it physical.
- For dynamic clouds, voxel occupancy refresh is a GPU memory traffic/atomics cost; update only dirty bricks and benchmark against simply sampling local density and optical depth in the existing march. Avoid particle-to-voxel rebuilds each frame if density fields already exist.
- Point-primitive pixel cost, overdraw, blending and memory access must be profiled on target hardware. A voxel-accelerated **raymarched** cloud may reproduce the useful shading without displaying any particles.

### Experiment acceptance tests

1. Stationary-camera cumulonimbus grows continuously between 2 slow snapshots; no visible snap/ghost.
2. Crosswind + vertical shear, cloud birth/dissipation and jump between cloud LODs.
3. Orbit approach preserves identity, optical depth and silhouette without unresolved erosion shimmer.
4. Cloud, sun-shadow, atmosphere and ground agree in a backlit mountain scene.
5. Internal lightning injects visibly coherent lighting without holding the render thread.
6. Particle-neighborhood shading on/off compared at equal shape, lighting and frame budget: track GPU ms, VRAM, visible darkening artifacts, silver-lining preservation, and error against expensive directional volumetric lighting.
7. No queued backlog at large timewarp; cache generation and resource lifetime validated under Vulkan synchronization validation.

## Decision status

**Accepted direction for planning:** world-anchored evolving state; delayed two-snapshot visual interpolation; velocity compensation; separate advected high-frequency erosion; shared optical authority and lazy transport.

**Research candidate, not yet selected for main renderer:** weighted IFS microstructure/transform interpolation and coarse voxel-neighborhood shading. Requires controlled GPU and visual experiments before adoption.

**Not selected:** brute-force 100M+ rendered particles as the primary planetary cloud representation; standalone voxel AO as a substitute for optical transport; independent particle lighting that breaks terrain/cloud/atmosphere unity.

# Orbit V0.0.7 — Universal Volumetrics Research Baseline

Status: **Architecture baseline for V0.0.7 volumetric milestones M30-M39**

## 1. Scope

Orbit needs one authoring/runtime framework for interactive volumetric and field-driven effects without assuming every effect shares one physical solver.

Target effect families include:

- smoke;
- fog/mist;
- clouds;
- fire;
- dust/sand;
- snow/spindrift;
- steam;
- generic emissive gas/plasma VFX;
- surface water flow/wakes/splashes;
- generic scalar/vector field effects.

The shared abstraction is:

> domain + typed fields + sources/effectors + solver policy + rendering policy + cache/LOD policy

Presets compose these pieces. They are not special runtime subclasses.

## 2. FluidNinja workflow lessons

FluidNinja LIVE-2 is a useful workflow reference because its public feature description emphasizes:

- generic use across water, fog, smoke, clouds, fire, sand and snow;
- compact 2D simulation driving 3D visualization;
- surface-aligned liquid/gas interaction;
- heterogeneous inputs such as bitmaps, runtime virtual textures, distance fields, landscapes, splines, particles, destructibles and bones;
- mapping simulation density to surfaces/volumes;
- using simulation velocity to drive flow detail and particles;
- a near interactive simulation attached to the player with passive far-background patterns;
- scalable/simple/passive modes and presets.

Fluid Ninja VFX Tools also demonstrates the value of baking compact density/velocity products to flipbooks/flowmaps and driving volumetric/particle presentation from those assets instead of requiring large dense volume caches for every effect.

Orbit should adopt these **architectural lessons**, not Unreal-specific APIs or implementation details.

## 3. Why 2D/2.5D matters

Asterra is a planetary world.

Many visually important effects are dominated by motion along a surface:

- dust moving over terrain;
- ground fog;
- snow drifting;
- tire/vehicle trails;
- shallow surface flow;
- wakes;
- fire spreading over a surface;
- sand movement.

A compact surface-aligned solver can provide responsive interaction over much larger areas than an equally dense 3D volume.

The 2D/2.5D result can drive:

- 3D density extrusion;
- vertical procedural detail;
- particles;
- surface masks;
- flowmap distortion;
- local 3D secondary domains where true vertical motion becomes important.

Therefore the surface solver is a production representation, not a temporary approximation intended to be deleted.

## 4. Why local 3D still exists

Some effects need actual volumetric structure:

- smoke rising through a room;
- a dense explosion plume;
- fire and hot gas around complex obstacles;
- localized cloud volumes;
- confined steam;
- volumetric interactions around vehicles/structures.

Orbit therefore keeps a local 3D domain mode under the same semantic authoring model.

A user should be able to change representation/solver policy without rebuilding the entire effect as a different object type.

## 5. Typed fields

Do not freeze one giant voxel record.

Different solvers require different channels.

Common vocabulary:

- density;
- velocity;
- temperature;
- pressure;
- fuel;
- combustion progress;
- humidity/condensate;
- occupancy/SDF;
- surface depth/height;
- foam;
- sediment/particulate;
- color;
- emission.

Plugins may register additional channels.

This reduces memory for effects that only need density + velocity, while allowing richer fire/cloud solvers when required.

## 6. Source/effectors

The common source contract should separate **what is injected/modified** from **how its shape is defined**.

Source shape/provider examples:

- point/sphere/box;
- spline;
- painted surface mask;
- texture/procedural field;
- mesh/SDF/collision volume;
- particle cloud;
- skeletal/bone attachment;
- destructible event;
- vehicle wake/velocity footprint.

Operation examples:

- inject density;
- add velocity;
- inject heat/fuel/moisture;
- remove/sink density;
- obstacle;
- vortex/wind;
- deposition/wetness.

This allows the same spline to be a smoke source, a wind effector or a water-flow guide without introducing one-off editor tools.

## 7. Large-world residency

A dense 3D volume around an entire planet is prohibited.

The intended hierarchy is:

- near interactive live domain;
- lower-frequency/coarser medium representation;
- passive/baked/procedural far representation;
- broad weather/climate fields as boundary conditions.

Domains use stable world/body coordinates.

A camera/player-following domain changes residency, not semantic world identity.

Like terrain clipmaps and GI clipmaps, scrolling should reuse valid state instead of resetting everything.

## 8. Rendering and lighting

Volumes are first-class participants in the V0.0.7 lighting system.

Required render concepts:

- extinction;
- transmittance;
- scattering;
- phase response;
- shadowing;
- emissive radiance;
- temporal integration;
- depth composition;
- surface-volume boundary behavior.

A volume receives the same stellar/local-light authority as surfaces.

An emissive volume can contribute to GI under the same budgeted lighting architecture.

Do not implement:

- a separate fake sun in the volume shader;
- RT-only volume lighting;
- fire whose only lighting effect is bloom.

## 9. Simulation vs rendering budgets

Keep separate:

- simulation update budget;
- volume rendering budget;
- volume-lighting budget.

Auto quality may independently reduce:

- solver resolution;
- update frequency;
- pressure iterations;
- advection quality;
- raymarch steps;
- shadow steps;
- temporal refresh;
- particle sampling.

A faster ray-query backend must not automatically cause the solver itself to become heavier.

## 10. Baked/passive representation

When interactivity is not needed, the system should be able to stop simulating.

Derived outputs may include:

- Orbit compact field cache;
- flipbook;
- flowmap;
- volume texture sequence;
- particle-driver cache;
- passive procedural parameters.

Bakes retain fingerprints of authoritative domain/source/solver settings.

A stale bake must be diagnosed.

## 11. UI model

The Studio workflow should prioritize direct manipulation.

Common path:

1. Add Volume.
2. Pick preset or Empty.
3. Place/scale domain.
4. Add source.
5. Simulate.
6. Inspect fields.
7. Tune rendering/lighting.
8. choose Live/Baked/Auto.
9. Bake if needed.

The viewport should provide:

- bounds;
- source handles;
- spline editing;
- paint tools;
- slices;
- vectors;
- density/temperature/emission overlays;
- residency/LOD visualization.

The Inspector remains the exact control surface.

UI, plugins and MCP call the same services.

## 12. Integration with existing Orbit systems

Volumetrics should reuse:

- FrameGraph/world/body coordinates;
- RenderGraph;
- RHI compute/storage resources;
- terrain surface addressing;
- lighting/GI;
- material system;
- Studio schema/Inspector;
- CommandService/undo;
- project assets/DDC;
- GPU timestamp/performance infrastructure.

Do not build a separate editor scene graph or transform system for volumes.

## 13. Validation

Numerical/reference tests:

- advection transport;
- dissipation;
- stable field addressing during domain movement;
- source accumulation;
- obstacle handling;
- cache fingerprints;
- live/baked equivalence within tolerance.

Interactive scenes:

- smoke around obstacle;
- dust blowing over terrain;
- snow/spindrift trail;
- emissive fire lighting a room;
- surface water flow/wake;
- camera-following near domain;
- near-live -> far-passive transition.

## 14. Explicit limits

V0.0.7 universal volumetrics does not promise:

- one physically exact multiphase CFD solver;
- planet-wide dense 3D Navier-Stokes;
- replacing Asterra climate/weather authority;
- replacing the dedicated water/ocean physical simulation;
- exact combustion chemistry;
- exact granular mechanics.

Those systems may provide boundary conditions or specialized solver plugins while sharing the same domain/render/UI framework.

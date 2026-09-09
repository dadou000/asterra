# Asterra 0.1.0 — Planetary Water & Hydrology Implementation Plan

**Branch:** `water/0.1.0`  
**Base:** `weather/0.0.5`  
**Status:** implementation blueprint  
**Primary visual reference:** KWS2 Dynamic Water System  
**Primary simulation reference:** Jeschke & Wojtan, *Generalizing Shallow Water Simulations with Dispersive Surface Waves*, SIGGRAPH 2023

Reference links:

- KWS2: https://assetstore.unity.com/packages/tools/particles-effects/kws2-dynamic-water-system-323662
- KWS2 docs: https://kripto289.gitbook.io/kripto289-docs
- NVIDIA research: https://research.nvidia.com/labs/prl/shallow-water-simulation/

---

## 1. Goal

0.1.0 turns Asterra's current ocean renderer and procedural drainage data into one coherent **planetary surface-water system**.

The target is not to run a full-resolution fluid solver over the planet. The target is to preserve physical continuity while using the cheapest valid representation for each region:

1. **Collapsed analytical domain** — calm lake, deep ocean reservoir, pond, reservoir.
2. **1D hydraulic network** — stable/dynamic river and stream discharge.
3. **Coarse 2D shallow water** — regional flooding, active flood fronts, disturbed water far from the player.
4. **Fine 2D shallow water** — local inundation, river overflow, shore interaction, dams, nearby dynamic water.
5. **Dispersive surface-wave layer** — physically meaningful wakes and wave dispersion where required.
6. **Toroidal render clipmap** — the visible high-detail surface coat, independent of the underlying physical LOD.
7. **FFT/spectral ocean** — wind waves and swell that do not need to exist in the bulk hydrology grid.

The result must support the following end-to-end chain without scripting individual flood volumes:

```text
weather / glacier / source
          |
          v
      soil / snow
          |
    infiltration
          |
       runoff
          |
    stream network
          |
       rivers
          |
  bank overflow / lake
          |
      2D flood
          |
       estuary
          |
        ocean
```

At the same time, a large vessel must be able to generate a dynamic wake whose long wavelengths propagate through the active water hierarchy, interact with shallow bathymetry, and produce coastal run-up when physically large enough.

---

## 2. Non-goals

0.1.0 must **not** attempt any of the following as a prerequisite:

- full 3D Navier-Stokes water over the world;
- global dynamic simulation of every cubic metre of ocean;
- centimeter-scale physics for visual ripples;
- permanent 2D SWE grids for normal rivers;
- permanent grids for calm lakes;
- exact global sea-level change from river inflow or local coastal flooding;
- GPU rigid-body replacement for Jolt/gameplay physics;
- full volumetric groundwater aquifer simulation;
- exact simulation of waterfalls as 3D free-surface volumes.

Deep ocean is an analytical reservoir/boundary. Rivers, lakes and land use dynamic physical grids only when their cheap representation is insufficient.

---

## 3. Existing 0.0.5 code to preserve

0.1.0 is an evolution of the current implementation, not a rewrite from zero.

### 3.1 Keep and generalize

| Existing path | 0.1.0 role |
|---|---|
| `scripts/terrain/ocean_geometry_clipmap.gd` | Keep as the basis of the visible `SurfaceWaterClipmap`; progressively generalize ocean-only assumptions. |
| `shaders/ocean_geometry_clipmap.gdshader` | Keep as the primary ocean surface renderer while the renderer is modularized. |
| `shaders/ocean_waves.gdshaderinc` | Preserve the world-space-invariant phase model. This is a core invariant for all visual LODs. |
| `scripts/terrain/ocean_gpu_physics.gd` | Keep its asynchronous query API concept, but split query service from authoritative hydrology simulation. |
| `shaders/ocean_buoyancy.glsl` | Reuse the shared surface-sampling logic during the migration to hydrology-aware queries. |
| `scripts/gen/pass_hydrology.gd` | Treat as deterministic baseline hydrology/topology generation. Runtime hydrology inherits from it. |
| `scripts/gen/flow_router.gd` | Reuse/generated drainage topology and accumulation as the initial river graph source. |
| `scripts/weather/surface_energy_bridge.gd` | Add precipitation/soil/runoff exchange here or through a dedicated bridge. Do not duplicate global terrain sampling. |
| `scripts/terrain/terrain_deltas.gd` | Hydrology invalidation source when player terrain edits change drainage/bathymetry. |
| `scripts/persist/save_game.gd` | Extend to sparse hydrology persistence; never serialize the deterministic planet wholesale. |

### 3.2 Existing invariants that must not regress

`ocean_waves.gdshaderinc` already establishes the correct visual principle: physical wave phase is independent of clipmap centre, ring and tessellation, while render LOD only decides whether a wavelength can be represented. Keep this invariant.

The existing ocean GPU physics query path also already uses persistent asynchronous slots rather than synchronizing every physics tick. Preserve that non-blocking behavior.

### 3.3 Important RenderingDevice change

The current `OceanGPUPhysics` creates a **local RenderingDevice**. That is fine for isolated asynchronous query computation, but the authoritative hydrology textures/buffers need to be sampled directly by the visible water renderer.

Therefore:

- authoritative hydrology resources must be allocated on the **main RenderingDevice** (`RenderingServer.get_rendering_device()` / native equivalent);
- local devices may remain for isolated tests or fallback jobs that do not need resource sharing;
- do not design the production water solver around CPU copies between a local compute device and the main rendering device.

This migration is Phase 1, before large solver work begins.

---

## 4. Core architecture

```text
                         PLANETARY WATER STATE
                                  |
         +------------------------+-----------------------+
         |                        |                       |
   HydroQuadtree             RiverGraph            HydraulicDomains
 sparse persistent state    1D channels           lakes/reservoirs/ocean
         |                        |                       |
         +------------------------+-----------------------+
                                  |
                         ActiveHydroScheduler
                                  |
                      sparse GPU active tile atlas
                                  |
           +----------------------+----------------------+
           |                      |                      |
       Bulk SWE             Dispersive field        Interactions
      h, hu, hv              wake residuals       sources/hulls/terrain
           |                      |                      |
           +----------------------+----------------------+
                                  |
                         SurfaceWaterClipmap
                        visible toroidal "coat"
                                  |
                   +--------------+--------------+
                   |              |              |
                 FFT         dynamic surface    shading
             wind/swell       reconstruction   foam/refraction/etc.
```

The persistent world state and the visible clipmap are deliberately different systems.

### Authority rules

- **HydroQuadtree / RiverGraph / HydraulicDomain state** owns persistent world-scale hydrology.
- **GPU active tiles** own the detailed state while a region is actively simulated.
- **SurfaceWaterClipmap** owns neither mass nor persistence; it reconstructs and renders the surface.
- **FFT** contributes surface displacement/normal/velocity but does not own bulk ocean mass.
- **Jolt/gameplay physics** remains authoritative for ships and rigid bodies.

---

## 5. Proposed source layout

Do not rename existing working files immediately. Add the new subsystem beside them and migrate incrementally.

```text
scripts/water/
    water_system.gd
    hydro_domain.gd
    hydro_scheduler.gd
    hydro_quadtree.gd
    hydro_tile_key.gd
    river_graph.gd
    river_segment.gd
    lake_domain.gd
    water_source.gd
    water_sink.gd
    soil_hydrology.gd
    hydrology_persistence.gd
    hydrology_debug.gd
    hydrology_query_service.gd
    hydrology_weather_bridge.gd

shaders/water/
    hydro_init.glsl
    hydro_classify_tiles.glsl
    hydro_flux.glsl
    hydro_integrate.glsl
    hydro_sources.glsl
    hydro_wet_dry.glsl
    hydro_friction.glsl
    hydro_restrict.glsl
    hydro_prolong.glsl
    hydro_reflux.glsl
    hydro_reduce.glsl
    hydro_activity.glsl
    hydro_hull_interaction.glsl
    hydro_queries.glsl
    hydro_debug.glsl
    water_surface_common.gdshaderinc
    water_surface_reconstruct.gdshaderinc

native/hydrology/                 # production backend after GPU prototype
    src/
    tests/
    CMakeLists.txt / build glue matching repo conventions

tests/water/
    test_lake_at_rest.gd
    test_dam_break.gd
    test_uniform_channel.gd
    test_rainfall_runoff.gd
    test_lod_conservation.gd
    test_tile_wake_sleep.gd
    test_lake_collapse_expand.gd
    test_save_roundtrip.gd
    test_wave_phase_invariance.gd
```

After the architecture is stable, `ocean_geometry_clipmap.gd` can be renamed/generalized to `surface_water_clipmap.gd` in one controlled migration.

---

## 6. Mathematical state

### 6.1 Active 2D cells

Use conservative shallow-water variables:

```text
h   water depth
hu  x/tangent momentum
hv  y/tangent momentum
zb  bed elevation
```

The first implementation should use **FP32** for `h`, `hu`, `hv` and critical reductions. Do not start by packing the core solver into FP16. Rendering fields may be FP16 once stability is established.

Additional per-cell data can be lower precision where appropriate:

```text
soil saturation       UNORM16 / FP16
infiltration state    FP16/FP32 depending model
roughness/material    R8/R16 index
foam potential        R8/R16
wet/dry/activity      bit flags
```

### 6.2 Numerical method requirements

The 2D solver must be designed for terrain and wet/dry fronts, not adapted from a flat decorative ripple solver.

Required properties:

- finite-volume conservative update;
- robust approximate Riemann flux (start with HLL/Rusanov-class robustness; optimize later);
- hydrostatic/well-balanced bed-slope treatment;
- positivity preservation (`h >= 0`);
- wet/dry hysteresis;
- semi-implicit bottom friction (Manning-style is sufficient initially);
- rainfall/source/sink terms;
- infiltration source term;
- CFL-controlled timestep/substeps;
- deterministic-enough reductions and explicit error diagnostics;
- no unexplained water creation at LOD/tile boundaries.

A **lake-at-rest** test over non-flat bathymetry is a release blocker. A solver that generates velocity from a stationary lake is not acceptable.

### 6.3 Surface reconstruction

Rendering samples a continuous conceptual function:

```text
H_render(x,t)
    = H_equilibrium
    + H_bulk_dynamic
    + H_dispersive
    + H_environmental_FFT
```

Normals and physically meaningful large-scale shading must be derived from the same fields/phases:

```text
N_render = normal(dH_render/dx, dH_render/dy) + micro_normal_detail
```

The clipmap mesh is only a sampler of this function.

---

## 7. Dynamic hydraulic representations

Every water body/region can change mathematical representation.

```text
COLLAPSED
   |
   v
NETWORK_1D
   |
   v
COARSE_2D
   |
   v
FINE_2D
```

Not every body uses every state.

### 7.1 Suggested enum

```text
SLEEPING_DRY
COLLAPSED_DOMAIN
NETWORK_1D
ALLOCATING
WARMING
COARSE_2D
FINE_2D
SETTLING
FROZEN_SNAPSHOT
```

### 7.2 Promotion triggers

Examples:

- outgoing boundary flux approaches a sleeping tile;
- river stage exceeds bank height;
- lake receives a high-energy disturbance;
- player/important vehicle enters refinement radius;
- terrain edit invalidates the cached hydraulic solution;
- dam/levee/obstacle changes;
- water source exceeds analytical capacity;
- local runoff accumulates above surface-water threshold;
- physical wake enters a region that must retain it.

### 7.3 Demotion triggers

Require hysteresis and a settling interval:

- velocity below low threshold;
- dynamic surface deviation below threshold;
- fluxes approximately steady;
- no pending disturbance/source change;
- no nearby important interaction;
- mass can be represented by the lower-level model within error tolerance.

Never promote/demote every frame at a distance threshold.

---

## 8. Physical LOD system

Physical LOD is independent of render clipmap LOD.

Suggested initial ladder (tune by testing, not by appearance):

| Hydro LOD | Representation | Typical role |
|---|---|---|
| H6+ | collapsed / graph only | unloaded world, normal lake/ocean/river |
| H5 | analytical + sparse metadata | approaching/important distant region |
| H4 | allocate/reconstruct/warm | hidden transition layer |
| H3 | coarse 2D active | visible regional dynamics |
| H2 | normal 2D | local flood/river/coast |
| H1/H0 | fine interaction | boats, structures, detailed shore |

### 8.1 Power-of-two spatial ratios

Use 2:1 cell ratios wherever possible:

```text
0.5 m -> 1 m -> 2 m -> 4 m -> 8 m -> 16 m ...
```

### 8.2 Conservative restriction

Fine -> coarse must preserve integrated mass and momentum, not visually average water height.

For a 2x2 restriction:

```text
M_coarse = sum(M_fine)
P_coarse = sum(P_fine)
```

### 8.3 Prolongation

Coarse -> fine reconstruction must preserve parent volume exactly and avoid introducing oscillatory momentum.

Use slope-limited reconstruction; initialize unresolved high-frequency content to zero, then allow the fine solver/visual wave layers to populate it.

### 8.4 Refluxing

At coarse/fine interfaces, store face-integrated fluxes and correct the coarse update with the summed fine fluxes. This is mandatory for long-lived mass stability.

### 8.5 Timesteps

Use CFL-aware subcycling:

```text
coarse: 1 step
next:   2 steps
next:   4 steps
fine:   8 steps
```

Synchronize at coarse step boundaries, restrict state, reflux, then continue.

### 8.6 Spectral filtering before downsampling

Do not alias short dynamic waves into coarse physical LODs. Low-pass the dynamic residual before restriction. Long ship-wake modes may propagate outward; unresolved turbulent/high-frequency content should dissipate/remain local.

---

## 9. Toroidal clipmap: keep it as the visible coat

The existing ocean clipmap remains the primary visible geometry mechanism.

### 9.1 Responsibilities

The clipmap handles:

- high-density visible geometry near the observer;
- horizon coverage;
- FFT displacement;
- reconstructed bulk/dynamic water displacement;
- world-space wave normals;
- foam/whitecaps;
- refraction/reflection/absorption;
- local high-frequency detail;
- transition from local to orbit ocean.

It does **not** own persistent water volume.

### 9.2 Phase invariant

Never evaluate wave phase from ring-local UV or clipmap-local coordinates.

The same world/planet-space coordinate must produce the same wave sample regardless of:

- render ring;
- grid spacing;
- clipmap recenter;
- toroidal wrap;
- graphics quality setting.

LOD may remove an unrepresentable wavelength, but must not change its phase/frequency/direction before removal.

### 9.3 Physics-frozen / rendering-alive

A distant lake/river/ocean may have no 2D physical grid while still rendering animated FFT/ripples/flow normals. This is intentional.

At H4, reconstruct/allocate the physical grid behind the still-visible coat and run hidden warm-up steps. Expose the dynamic physical result only after it is stable.

---

## 10. Ocean model

### 10.1 Deep ocean

Treat deep ocean as an analytical reservoir:

```text
mean sea level
+ tide field
+ storm-surge offset
+ large-scale current field
```

Rendering adds the FFT spectrum.

Do not globally lower/raise the ocean because a local river or flood transfers a negligible volume.

### 10.2 Active ocean patches

Create dynamic domains only for:

- large ship interaction;
- near-coastal dynamics;
- estuaries;
- major disturbances;
- locally meaningful long wake modes;
- extraordinary events.

### 10.3 Coast boundary

At the offshore edge of an active coastal domain, impose analytical ocean forcing:

```text
stage = mean sea level + tide + surge
incoming spectrum / dynamic long wave
current
```

Water draining back into deep ocean can be absorbed by the reservoir boundary after its local effect is no longer relevant.

---

## 11. Rivers

Normal rivers must not be permanent 2D grids.

### 11.1 Baseline graph

Build `RiverGraph` from the deterministic drainage data generated by `PassHydrology`/`FlowRouter`.

A segment stores at least:

```text
segment ID
upstream/downstream connections
length/slope
channel width/depth
bank elevation
roughness
baseline Q
current Q
stage
bankfull Q/capacity
subgrid cross-section descriptor
```

### 11.2 Runtime modes

**Equilibrium:** cached stage/velocity/render flow, effectively no solver cost.

**1D dynamic:** propagate changing discharge caused by rain, glacier melt, tributaries, controls, dams, etc.

**2D overflow:** when stage/bankfull or an obstacle requires floodplain flow, activate adjacent 2D tiles.

### 11.3 Subgrid channels

At coarse world LOD, do not allow a 10 m river to disappear inside a 250 m cell. Preserve effective channel geometry/discharge separately from averaged terrain.

### 11.4 Active frontier

A river overflow should wake only reachable neighboring tiles. Use boundary flux plus terrain reachability/minimum boundary elevation, not a simple circular radius.

---

## 12. Lakes, ponds and reservoirs

### 12.1 Collapsed state

A calm lake should usually be one `HydraulicDomain`:

```text
volume V
surface elevation H
surface area A(H)
inflow
outflow
evaporation
optional mean momentum/temperature/turbidity
```

Precompute a basin storage curve:

```text
V(H)
A(H)
```

Changing lake level during slow rain/inflow does not require SWE.

### 12.2 Promotion

Promote to local/coarse 2D when:

- a large boat/physical disturbance requires it;
- a landslide/dam failure injects strong momentum;
- inflow is strongly non-equilibrium;
- shore run-up matters locally;
- the player enters the high-detail interaction region.

A local boat patch does not automatically require the entire lake to become fine 2D.

### 12.3 Collapse

When dynamics settle:

```text
V = sum(h_i * area_i)
```

reduce the grid to domain volume/level and delete expensive state.

Volume error across expand -> simulate -> collapse -> expand is a release metric.

---

## 13. Rain, soil, glacier and runoff coupling

### 13.1 Weather input

Use the existing weather system as the precipitation source. Avoid resampling the same terrain/weather fields independently in multiple subsystems.

The bridge needs to expose spatial fields such as:

```text
rain rate
snowfall / snowmelt contribution
temperature for melt/evaporation
optional evapotranspiration forcing
```

### 13.2 Soil model

Start with a cheap vertical column per hydrology cell/tile:

```text
soil moisture
saturation
infiltration capacity
cumulative infiltration
groundwater proxy
```

Use terrain/biome/material IDs to look up mostly-static properties:

```text
porosity
hydraulic conductivity
soil depth
roughness
```

Do not duplicate large static property arrays if the procedural terrain can regenerate them.

### 13.3 Runoff

As long as rainfall is absorbed or can be routed analytically, no 2D water grid is required.

Activate surface water when:

```text
rain/snowmelt > infiltration + cheap drainage capacity
```

or when an incoming flood/source makes a dry tile wet.

### 13.4 Glacier/source API

Implement generic sources/sinks:

```text
WaterSource
    rate_m3_s
    temperature (future)
    sediment (future)
    direction/momentum optional

WaterSink
    capacity_m3_s
```

Glacier melt, springs, pumps, drains and scripted infrastructure should all use the same source interface.

---

## 14. Sparse GPU active-tile scheduler

### 14.1 Principle

GPU cost must scale with **active hydrological complexity**, not planet area.

### 14.2 Tile state buffer

Each resident tile needs compact metadata:

```text
tile key
physical LOD
atlas slot
state flags
max depth
max velocity
max delta-height
max outgoing flux
disturbance energy
last-active timestamp
neighbor references
```

### 14.3 GPU-generated work lists

Compute passes classify tiles and append them to work queues:

```text
coarse active
fine active
wet/dry update
LOD transfer
settling candidates
query candidates
```

Use indirect dispatch where practical so the GPU does not require a CPU round trip to decide the next workload.

### 14.4 Neighbor activation

Wake a sleeping neighbor if a physically reachable outgoing flux is expected to cross the boundary.

Avoid activating land merely because it is adjacent to water if the boundary elevation makes inundation impossible.

---

## 15. GPU resource design

Start simple and measurable.

### 15.1 Active atlas

Use array layers or tiled atlases for dynamic fields rather than one texture per tile.

Initial fields:

```text
state A: h, hu, hv
state B: h, hu, hv
bed elevation
face flux scratch
source/infiltration
activity/flags
```

Do not prematurely compress solver state.

### 15.2 Planning VRAM budget

Use **~32 B/active cell as an initial engineering planning number for core+auxiliary state**, then measure actual ping-pong/scratch usage. FP32 conservative state may move this upward depending on flux scratch design.

Target total water/hydrology allocations on High roughly within **0.5–1.5 GB**, with quality settings scaling active-cell budget and render effects independently.

### 15.3 No full-grid readback

Never read active hydrology atlases back every frame.

Gameplay obtains only compact query results:

```text
water height/depth at point
surface normal/velocity
flood status
hull force + torque
structure exposure
```

---

## 16. Boat and object interaction

### 16.1 Rigid-body ownership

Keep vessel rigid bodies on CPU/Jolt.

### 16.2 GPU hull interaction

Upload compact hull probe/raster data and compute:

```text
buoyancy
relative-flow drag
slamming
wave force
planing/lift proxy where appropriate
```

Reduce hundreds/thousands of probe forces on GPU to one force + torque per rigid body.

Only the reduced result crosses GPU -> CPU.

### 16.3 Wake injection

The vessel injects displacement/momentum into the local dynamic water field. Do not make the primary wake a decal or purely visual trail.

Short turbulent detail can remain visual/particle-based; long physically meaningful wake modes enter the dynamic/dispersive field.

---

## 17. Dispersive waves (NVIDIA-inspired stage)

The SIGGRAPH 2023 reference separates water motion into a bulk component suited to SWE and a dispersive surface-wave component suited to Airy-wave behavior, solves them separately, then recombines them.

0.1.0 should adopt this **architecture**, not blindly port research code.

### 17.1 First requirement

Ship a stable conservative bulk SWE implementation before adding dispersive coupling.

### 17.2 Then add

```text
total dynamic motion
        |
        +--> bulk / mass transport -> SWE
        |
        +--> dispersive residual -> surface-wave solver
        |
        +--> recombination
```

This layer is most valuable for:

- ship wakes;
- wave interference;
- deep-to-shallow wake transition;
- physically plausible phase/group propagation;
- avoiding the nondispersive appearance of plain SWE.

### 17.3 FFT relationship

Keep environmental ocean FFT separate from the dynamic dispersive residual.

```text
FFT = background wind/swell spectrum
Dispersive residual = interactions/disturbances that must remain physically coupled
SWE = bulk mass/momentum/flooding
```

---

## 18. KWS2-quality rendering target

KWS2 is the visual target, not a code dependency.

### 18.1 Multi-scale surface

Render distinct frequency scales:

```text
large swell / long dynamic wave
medium chop
small ripples
micro-normal roughness
```

### 18.2 Phase coherence

Geometry displacement, large-scale normals, velocity-driven foam and caustic inputs must derive from coherent world-space wave fields.

### 18.3 Required rendering features

Implement incrementally:

- physically-based Fresnel response;
- depth-dependent absorption;
- scattering/SSS approximation;
- shallow seabed contribution;
- refraction with correct water IOR;
- reflection stack appropriate to Godot (sky/probes + SSR/other available path + optional planar where justified);
- anisotropic/crest highlights where beneficial;
- dynamic foam from breaking/compression/turbulence/shore interaction;
- foam advection/decay;
- spray/splash GPU particles;
- caustics from coherent wave data;
- underwater absorption/scattering;
- Snell-window style underwater surface response;
- sun shafts/volumetric integration where the weather renderer supports it;
- wet shoreline/terrain response;
- waterline transition without clipping/pop.

### 18.4 Resolution deception is intentional

The physical grid only resolves motion that must move mass/energy correctly. FFT, reconstructed displacement and normals provide smaller visible scales. Do not increase SWE resolution to solve a shading problem.

---

## 19. Planetary persistence: sparse cube-sphere quadtree

Use a **quadtree per cube-sphere face** for surface hydrology persistence. A full octree is unnecessary until true volumetric subsurface water is required.

### 19.1 Tile key

Use a stable key such as:

```text
face + level + Morton/Z-order code
```

### 19.2 Node contents

A node may store:

```text
flags/state
water-volume anomaly
mean/stage level
soil moisture/saturation summary
groundwater proxy
disturbance energy
optional coarse momentum
child mask
references to exceptional snapshots
```

### 19.3 Compression rule

If four children are representable by their parent within configured tolerances and no active frontier crosses them, merge them.

Hydrological save size should shrink again after floods settle.

### 19.4 Separate river graph

Do not encode every stable narrow river as quadtree water cells. Serialize river segment state separately.

### 19.5 Exceptional dynamic snapshots

If the player saves during a major dam break/flood that cannot safely collapse, store a **coarse dynamic snapshot** for the exceptional active tiles. Do not serialize the full visible clipmap.

---

## 20. Save-game integration

Bump `SaveGame.VERSION` only when the new schema is actually wired and migration behavior is decided.

Proposed payload:

```text
hydrology:
    schema_version
    quadtree_nodes
    river_state
    hydraulic_domains
    exceptional_dynamic_tiles
    hydrology_time
```

Persist only state that cannot be regenerated from seed + deterministic baseline.

### Load sequence

```text
regenerate planet baseline
        |
restore sparse hydrology state
        |
restore river/lake domains
        |
restore exceptional dynamic tiles if any
        |
allocate H4 warm-up around relevant visible regions
        |
show dynamic result only after stable
```

---

## 21. Terrain edits and structures

Player construction/terrain modification must invalidate hydrology selectively.

### 21.1 Terrain deltas

On `Deltas.region_changed`:

- identify affected river/quadtree tiles;
- invalidate cached basin/outlet metadata locally;
- re-evaluate reachability/drainage;
- wake the smallest required hydrology region;
- never rebake the whole planet because one ditch was dug.

### 21.2 Structures

Represent hydrologically relevant structures with a compact obstacle/porosity/elevation interface:

- dams;
- levees;
- culverts;
- bridge piers;
- walls/buildings;
- drainage openings.

Do not require the water solver to understand arbitrary high-poly meshes directly at planetary scale. Rasterize relevant geometry into the active bed/obstacle field.

---

## 22. Stability rules (release blockers)

The following are non-negotiable:

1. **Power-of-two physical LOD ratios.**
2. **Conservative mass and momentum transfer.**
3. **Flux refluxing at coarse/fine interfaces.**
4. **CFL-aware substeps.**
5. **Well-balanced lake-at-rest behavior.**
6. **Positivity-preserving wet/dry handling.**
7. **Hysteresis for wet/dry and activate/sleep state changes.**
8. **No full-grid resampling when the visual toroidal clipmap moves.**
9. **Simulation ownership must be separate from visual LOD blending.**
10. **Low-pass/filter unresolved dynamic waves before coarse restriction.**
11. **Continuous global/regional mass-error diagnostics.**
12. **Long-duration soak tests measured in hours, not seconds.**

---

## 23. Debug tooling

Create a `HydrologyDebug` panel/overlay with:

```text
active tile count by HydroLOD
allocated atlas slots
VRAM estimate
GPU timings per pass
substeps / CFL max
wet cell count
tile promotions/demotions per second
river Q/stage/bankfull
lake volume/level
soil saturation
outgoing tile flux
mass added by rain/sources
mass removed by infiltration/evap/sinks
mass error / unexplained delta
query latency
```

World overlays:

- tile boundaries colored by state;
- physical LOD;
- flow velocity vectors;
- depth heatmap;
- soil saturation;
- drainage graph;
- river bankfull margin;
- wake/dispersive energy;
- coarse/fine interface flux error;
- frozen vs active domains.

---

## 24. Automated tests

### Solver correctness

- lake at rest over uneven bed;
- flat-water zero-motion test;
- 1D dam-break reference;
- uniform channel steady flow;
- wet/dry advancing front;
- rainfall-on-slope mass balance;
- source/sink exact-volume test;
- Manning/friction decay sanity test.

### LOD

- constant flow crossing H1/H2/H3 boundaries;
- standing lake crossing a physical LOD interface;
- flood front crossing an interface;
- fine -> coarse -> fine volume round trip;
- refluxing error over 100k+ steps;
- unresolved wave filtering/alias prevention;
- physical LOD activation hysteresis.

### Dynamic domains

- lake expand/collapse/expand preserves volume;
- calm river remains network-only;
- bankfull transition activates 2D floodplain;
- flood recedes and tiles collapse;
- distant disturbance degrades resolution rather than disappearing.

### Rendering

- clipmap reanchor does not move wave phase;
- two adjacent render LODs sample the same world-space wave;
- graphics quality change does not change authoritative buoyancy phase;
- FFT disabled leaves physical water intact;
- physics frozen/rendering alive mode remains visually stable.

### Persistence

- save/load exact domain volume;
- river Q/stage round trip;
- quadtree merge/split round trip;
- dynamic exceptional snapshot restores without large impulse;
- deterministic baseline + sparse delta reconstructs same hydrology state.

---

## 25. Performance targets and quality scaling

Do not tie simulation quality directly to visible mesh density.

### Scalable knobs

```text
max active 2D cells
max fine HydroLOD radius
coarse tile update frequency
number of solver substeps cap
number of dispersive modes/domain
FFT resolution/cascade count
foam/spray particle budget
reflection quality
caustics resolution
underwater volumetric quality
```

### Suggested profiles (starting targets only)

| Setting | Hydrology strategy |
|---|---|
| Low | smaller fine radius, fewer active cells, coarse distant dynamics, reduced dispersive layer |
| Medium | full gameplay hydrology, moderate local refinement |
| High | larger fine/coarse active budgets, full dispersive wakes, high visual water |
| Ultra | larger active frontier and visual effects; **not** different conservation/gameplay outcomes |

Gameplay-critical flood extent/river state should not fundamentally change because the player changed graphics quality.

---

## 26. Implementation milestones

### Phase 0 — 0.1.0 foundation

**Tasks**

- create `water/0.1.0` branch;
- add this architecture plan;
- add hydrology debug category and feature flags;
- document current ocean/water invariants;
- establish test scenes and GPU timing infrastructure.

**Done when**

- existing 0.0.5 ocean/weather behavior is unchanged;
- new work can be enabled/disabled independently.

---

### Phase 1 — Separate visible water from physical authority

**Tasks**

- introduce `WaterSystem` service;
- introduce `HydrologyQueryService`;
- expose current ocean clipmap as render consumer rather than physical authority;
- migrate shared future hydrology buffers to the main RenderingDevice;
- keep existing async buoyancy query behavior working during migration;
- define `H_render = equilibrium + dynamic + FFT` shader interface.

**Done when**

- visual ocean is unchanged;
- buoyancy still works;
- a synthetic dynamic-height texture can deform the clipmap without CPU readback.

---

### Phase 2 — Fixed-domain conservative SWE prototype

**Tasks**

- implement FP32 `h/hu/hv` GPU solver;
- bed elevation input from existing terrain sampling;
- well-balanced bed slope;
- wet/dry handling;
- friction;
- source/sink pass;
- CFL reduction/substeps;
- debug readback/reductions only.

**Test scene**

A small basin with rain/source, dry land, outlet and terrain slope.

**Done when**

- lake-at-rest is stable;
- dam-break and wet/dry tests pass;
- mass error stays within defined tolerance over long runs.

---

### Phase 3 — Sparse active tiles and frontier activation

**Tasks**

- tile atlas allocator;
- GPU activity classifier;
- outgoing flux/reachability activation;
- settling and sleep hysteresis;
- indirect work lists;
- tile resource recycling.

**Done when**

- a flood can move across a large test terrain while only a narrow active frontier is simulated;
- dry unreachable neighboring tiles never allocate unnecessarily.

---

### Phase 4 — Conservative physical LOD

**Tasks**

- 2:1 HydroLOD hierarchy;
- restriction/prolongation;
- face-flux bookkeeping;
- refluxing;
- subcycling;
- hidden H4 allocation/warm-up;
- activation/deactivation hysteresis;
- unresolved dynamic-wave filter.

**Done when**

- river/flood flow can cross multiple HydroLODs without visible reflection or measurable systematic mass drift;
- LOD boundaries remain stable for hours.

---

### Phase 5 — Hydraulic domains, lakes and river graph

**Tasks**

- build runtime `RiverGraph` from generated hydrology;
- stable and dynamic 1D river modes;
- bankfull/stage model;
- lake/reservoir `V(H)` storage curves;
- lake collapse/expand;
- river -> 2D overflow promotion;
- 2D -> river/domain demotion.

**Done when**

- a calm planet can contain many rivers/lakes with almost no 2D solver allocation;
- increasing upstream discharge automatically floods a bank when capacity is exceeded.

---

### Phase 6 — Weather, soil, glacier and runoff

**Tasks**

- weather precipitation bridge;
- soil saturation/infiltration;
- runoff routing into drainage graph;
- generic sources/sinks;
- snow/glacier melt source;
- optional coarse groundwater proxy.

**Vertical slice**

```text
glacier -> stream -> river -> lake -> outlet -> river -> ocean
```

and

```text
heavy rain -> soil saturation -> runoff -> river rise -> floodplain
```

**Done when**

- normal dry land consumes no 2D water compute;
- surface tiles wake only when real water reaches/accumulates in them.

---

### Phase 7 — Ocean/coast integration

**Tasks**

- analytical ocean boundary/state;
- tide/surge/current hooks;
- coastal active domains;
- estuary coupling;
- ocean reservoir sink/source behavior;
- physical coastal run-up using the same wet/dry solver.

**Done when**

- river flood reaches the sea without requiring global ocean simulation;
- storm-surge boundary can push water inland through real topography.

---

### Phase 8 — Dynamic dispersive wave layer

**Tasks**

- introduce bulk/dispersive decomposition interface;
- implement dynamic dispersive field for interactive waves;
- recombine with bulk SWE;
- retain background FFT separately;
- define LOD spectral transfer rules.

**Done when**

- a vessel wake shows wavelength-dependent propagation/interference;
- long wake energy crosses HydroLODs and can shoal/run up at a coast;
- short unresolved modes do not alias into coarse water.

---

### Phase 9 — Vessel hydrodynamics

**Tasks**

- hydrology-aware height/normal/velocity queries;
- GPU hull probe/raster pipeline;
- GPU force/torque reduction;
- momentum/displacement feedback into dynamic water;
- planing/slamming/drag refinement;
- wake/foam coupling.

**Done when**

- a large ship produces a physical wake from hull interaction rather than a scripted trail;
- Jolt receives compact stable forces without per-frame large readbacks.

---

### Phase 10 — KWS2-class renderer

**Tasks**

- multi-cascade spectral ocean refinement;
- coherent displacement/slopes/normals;
- foam generation/advection;
- PBR absorption/scattering/refraction;
- reflection stack;
- caustics;
- underwater response;
- spray/splash/wetness;
- quality presets independent of authoritative hydrology.

**Done when**

- the physical grid resolution is no longer visually obvious;
- render-LOD transitions do not change wave phase;
- ocean, lake, river and floodwater share a coherent visual material family.

---

### Phase 11 — Planetary persistence

**Tasks**

- cube-sphere sparse hydrology quadtree;
- Morton tile keys;
- sparse soil/water anomalies;
- river/domain serialization;
- dynamic snapshot fallback;
- save schema integration;
- load reconstruction/warm-up.

**Done when**

- a flood can occur far away, be stored in coarse state, and refine correctly when the player arrives;
- calm lakes/rivers serialize as compact domains/network state rather than grids;
- save size follows disturbed complexity rather than planet surface area.

---

### Phase 12 — Terrain/build-system coupling

**Tasks**

- selective drainage invalidation from terrain edits;
- dams/levees/culverts/obstacle rasterization;
- player-built water sources/drains/pumps;
- flood exposure queries for structures;
- erosion/sediment hooks reserved but not required for 0.1.0 completion.

**Done when**

- digging/blocking a channel reroutes water locally without rebuilding the planet;
- a player-built obstruction can cause physically consistent upstream level rise/overflow.

---

### Phase 13 — Optimization and soak validation

**Tasks**

- GPU profiler pass-by-pass budgets;
- VRAM allocator instrumentation;
- async compute opportunities where Godot/Vulkan integration allows;
- atlas compaction/reuse;
- quality profiles;
- 1 h / 6 h / overnight numerical soak tests;
- worst-case storm/flood stress scene;
- multiple simultaneous vessel/river/flood domains.

**Done when**

- no systematic mass drift is observed beyond defined tolerance;
- no repeated LOD wake/sleep oscillation;
- no visible clipmap phase pop;
- active-cell count and VRAM remain bounded under stress.

---

## 27. 0.1.0 definition of done

0.1.0 should be considered complete when there is at least one reproducible world-scale vertical slice demonstrating all architectural layers:

1. procedural glacier/water source;
2. source feeds a generated stream/river graph;
3. normal river stays cheap/1D;
4. heavy source/rain raises discharge;
5. bank overflow automatically activates sparse 2D flood tiles;
6. flood crosses physical LODs conservatively;
7. lake can exist collapsed, expand when disturbed and collapse again;
8. river/flood reaches analytical ocean boundary;
9. local ocean/coast uses the existing toroidal visible coat;
10. a large vessel generates a physical dynamic wake;
11. dynamic long waves remain phase-stable across render/physical LODs;
12. water can run onto a coast when wave/surge energy is sufficient;
13. far water is physics-frozen but visually alive;
14. save/load stores sparse hydraulic/quadtree state rather than the visible grid;
15. debug tools demonstrate bounded VRAM, bounded active tiles and measured mass error.

---

## 28. First implementation sequence

The first code changes after this document should be made in this order:

1. Add `scripts/water/water_system.gd` and register it as the hydrology coordinator.
2. Add a shared main-RenderingDevice resource owner for water compute buffers.
3. Refactor `OceanGPUPhysics` behind `HydrologyQueryService` without changing current behavior.
4. Add a synthetic dynamic-height/velocity field and sample it from `ocean_geometry_clipmap.gdshader`.
5. Create the fixed-domain SWE test harness and numeric debug counters.
6. Pass lake-at-rest and wet/dry tests before any sparse/planetary work.
7. Add sparse tiles.
8. Add conservative LOD/refluxing.
9. Only then connect generated rivers/lakes/weather.
10. Add dispersive interactive waves after the bulk solver is stable.
11. Finish the KWS2-quality rendering pass after the surface interface is stable enough not to be repeatedly rewritten.

This order minimizes the risk of building planetary streaming around an unstable numerical core or building expensive rendering around an interface that is still changing.

---

## 29. Architectural decisions to keep fixed unless tests disprove them

- Keep the toroidal clipmap.
- Make it a visible reconstruction/cache layer, not world persistence.
- Keep wave phase in world/planet space.
- Separate physical LOD from render LOD.
- Use a sparse cube-sphere quadtree for persistent surface hydrology.
- Use a separate river graph for narrow/stable channels.
- Collapse calm lakes to hydraulic domains.
- Treat deep ocean as analytical reservoir + FFT.
- Simulate disturbances, not the entire water inventory.
- Keep gameplay rigid bodies on CPU/Jolt and water numerics on GPU.
- Keep active hydrology buffers on the main Vulkan/RenderingDevice path so rendering can consume them without CPU round trips.
- Prioritize conservative stability over half-precision memory savings.
- Never hide numerical mass errors with visual blending.

---

## 30. Success criterion

The system succeeds when the player can no longer tell which mathematical representation currently owns a body of water.

A river may be a cached graph segment 20 km away, a coarse 2D flood 3 km away, and a fine GPU domain beside the player; a lake may collapse to one volume value after settling; the ocean may be analytical beneath an FFT surface until a ship or coast requires dynamics. The visible clipmap should make those representation changes visually continuous, while the persistent hydrology layer makes them physically continuous where conservation matters.

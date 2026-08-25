# Asterra — Phase 1: Procedural Asterra

Godot **4.7.1 stable** with a C++20 AVX2/FMA weather extension.

This implements **Phase 1 of the Asterra Development Roadmap**: the whole planet
generator plus the streaming, editing and persistence needed to stand on it.

> **Phase 1 milestone.** *Generate Asterra from a fixed seed, fly from orbit to a
> believable watershed, walk on layered ground, excavate it, move the material,
> and save/reload the modification.*
>
> All six clauses are implemented and verified by the headless test suite.

---

## Running it

Open the folder in Godot 4.7.1 and press **F5**, or:

```
godot --path .                               # play
godot --headless --path . res://tests/Tests.tscn        # 71-check verification suite
godot --headless --path . res://tests/Preview.tscn      # export all 15 map layers as PNG
godot --headless --path . res://tests/WorldReport.tscn  # hypsometry / lake / ice statistics
godot --headless --path . res://tests/ClimateReport.tscn # energy balance vs Earth, then Asterra
godot --headless --path . res://tests/Stream.tscn       # streaming + triangle-budget probe
```

On Windows, build the native weather backend once with
`tools/build_weather_avx2_windows.ps1`, then restart Godot. See
`native/weather/README.md` for requirements and direct CMake commands.

The first run bakes the planet (~50 s single-threaded for the default 221k-cell
macro grid) and caches it under `user://asterra/worlds/`. Every later run loads
it in about a second. Changing anything in `world.tres` — or bumping
`GenConfig.PIPELINE_VERSION` after editing a pass — invalidates the cache
automatically.

### Controls

| | |
|---|---|
| `WASD`, `Space` / `Shift+C` | move (fly speed scales with altitude, so orbit→ground is one continuous motion) |
| `Shift` | sprint |
| `F` | toggle walk / fly |
| `Esc` | release the mouse |
| `LMB` / `RMB` | dig / fill at the aim point |
| `G` | grade the ground to the aimed height |
| `Q` / `E` | drop a heap of carried material / pick one back up |
| `[` `]` | brush radius |
| `M`, `,` `.` | planet map overlay, previous/next layer |
| `é` | live weather map; adjust display influence and weather speed from paused 0× to rapid spin-up at 128× |
| `P` | while the weather map is open, tune six physical processes and all six vertical contribution weights live |
| `T` | teleport to the best transport-corridor site on the planet |
| `F5` / `F9` | save / load |

The HUD reports the actual field values under your feet — bedrock family, ore and
hydrocarbon content, discharge and Strahler order, climate, soil texture and
horizons, biome, buildability. That readout *is* the proof that the systems are
coupled.

---

## What Phase 1 asked for, and where it lives

| Roadmap | Implementation |
|---|---|
| **1.1** cube-sphere planetary quadtree, 4 representation levels, deterministic from seed, streaming | `core/cube_sphere.gd`, `core/planet_grid.gd`, `terrain/planet_terrain.gd`, `terrain/chunk_builder.gd` |
| **1.2** continents, islands, mountain belts, plateaus, valleys, basins, coastlines, shelves, icy poles | `gen/pass_macro.gd` |
| **1.3** bedrock families and strata, faults and folds, sedimentary basins, ore veins, petroleum and gas, high-purity quartz, groundwater layers | `gen/pass_geology.gd` |
| **1.4** erosion, deterministic watershed network, floodplains, wetlands, groundwater tendencies | `gen/pass_erosion.gd`, `gen/flow_router.gd`, `gen/pass_hydrology.gd` |
| **1.5** climate fields from a solved energy balance, derived soil with vertical horizons, derived biomes | `gen/climate_ebm.gd`, `gen/pass_climate.gd`, `gen/pass_soil.gd`, `gen/pass_biome.gd` |
| **1.6** natural transport corridors computed before any city | `gen/pass_suitability.gd` |
| **1.7** dig/cut/grade/fill, excavated volume becomes physical material, transport and redeposit, sparse deltas, save/load | `terrain/terrain_editor.gd`, `terrain/terrain_deltas.gd`, `terrain/material_stock.gd`, `terrain/loose_pile.gd`, `persist/save_game.gd` |

Phase 0 pieces that Phase 1 could not exist without are also here: hierarchical
reference frames with a floating origin (`core/frames.gd`, `core/vec3d.gd`), the
world/spatial database (`gen/planet_fields.gd`), the material database
(`gen/material_db.gd`), and the persistence layer.

See `docs/PHASE1.md` for the design rationale of each system.

---

## Architecture in one page

**Canonical state is data, not nodes.** The planet is ~40 flat arrays over a
cube-sphere cell grid (`PlanetFields`). Nothing about a mountain, an ore body or a
river requires a `Node3D` to exist. The scene graph holds only what is currently
being drawn.

**Two scales, one function.** Terrain height anywhere is

```
macro (baked, eroded, ~8 km grid)  +  sub-grid detail (procedural)  +  player deltas
```

The macro fields are baked once per seed and cached. The detail is a pure
function of position and seed, so it costs no storage. Only the deltas — the
ground the player actually moved — are ever written to a save file.

**Everything is double precision until the last moment.** `Vec3D` keeps world
positions in 64-bit; the Godot scene graph only sees a local frame whose origin
follows the player (`Frames`). This is why you can fly from 900 km up to standing
on a 0.75 m editable surface with no vertex jitter — and it had to be built
before any terrain code, not retrofitted.

**Generation order is the roadmap's dependency order.** Macro geography →
geology → provisional climate → erosion → hydrology → final climate → soil →
biomes → suitability. Climate runs twice on purpose: erosion needs to know where
it rains, and the final climate needs the eroded mountains to cast their rain
shadows.

---

## Verification

`tests/Tests.tscn` runs 71 property checks headless, including:

* two bakes of the same seed are bit-identical; a different seed is a different planet
* the cube-sphere inverse is exact and macro fields interpolate across face seams
* 1 cm offsets survive at 1.6 × 10⁶ m from the origin
* no flow runs uphill, the drainage network is acyclic, 100 % of land is exorheic
* deep ocean floor is mafic crust; ore, petroleum and quartz provinces exist
* poles are colder than the equator; continental interiors are drier than coasts
* insolation integrates to exactly S0/4; the energy balance converges to a habitable climate
* cooling the planet grows the ice caps on its own, through the albedo feedback
* continental interiors have harsher seasons than coasts; wet ground has a flatter year than arid
* soil texture fractions sum to 1; ≥ 8 biomes emerge from climate alone
* excavated loose volume equals the in-place volume times the material's bulking factor
* filling consumes real stock and cannot invent matter
* deltas, loose heaps and player state survive a save/load round trip exactly
* the spherical GPU clipmap stays bounded, while the local collision bubble converges, follows movement, and rebuilds edits

The visual terrain uses a fixed 15-level spherical clipmap. In the headless
movement probe, the local physics bubble remained bounded at 46-48 collision
tiles while retiring and replacing coverage around the observer.

---

## Known limits of this pass

* **Editing is a height-field delta layer,** not a volumetric one. Digging,
  grading, filling and material accounting are real, but you cannot yet cut an
  overhang or a tunnel. The delta store is addressed on a cube-sphere lattice, so
  swapping in a sparse voxel layer later does not change any caller.
* **Edits within ~1 lattice cell of a cube-face seam are clipped.** The seams are
  four great circles; a wrapped brush is a small, contained fix.
* **Ground contact is resolved against the height field,** not the streamed
  collision meshes, so the player cannot fall through a chunk that has not
  finished meshing. Collision meshes are built and are ready for Phase 2 bodies.
* **The bake is CPU-side.** It is a one-off cached cost, but the erosion and noise
  passes are the obvious first candidates for compute shaders or a C++ core.
* **Loose material heaps are geometric, not simulated.** They carry real volume,
  density, bulking factor and angle of repose — the numbers Phase 3 needs — but
  they do not yet slump or flow.
* Weather has a live AVX2 global model and player-centered local nest; vegetation
  is still a density field, not instanced plants.

## Where this hands off to Phase 2

`Planet.sample_info()` and `Planet.column_material()` already return everything an
assembly needs to know about the ground it sits on: bearing material, soil
texture and depth, water table tendency, flood risk, buildability. `MaterialStock`
is the same object the screwdriver's inventory will consume, and `TerrainEditor`
already refuses to create matter — so "keep the excess material" in the Unc
prototype is a property of the terrain system rather than mission script.

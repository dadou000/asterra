+++
path = "/rendering/terrain/bake"
title = "Planet bake (baked tectonics and river graph)"
kind = "subsystem"
status = "stable"
summary = "The baked planet structure terrain generation consumes instead of computing it: per-face cube rasters of the tectonic plate fields and a global river graph, stored in a section-based .orbitbake file, baked in the background when the recipe changes and swapped in when ready. Terrain sampling and the GPU generator read the bake; none evaluate plates or drainage."
owner_module = "OrbitTerrainBake"
keywords = ["bake", "baked", "tectonic bake", "planet bake", "cube raster", "orbitbake", "rebake", "stale", "recipe hash", "bakes", "precomputed", "no runtime generation"]
sources = [
  "engine/terrain/include/orbit/terrain/BakedTectonics.hpp",
  "engine/terrain/include/orbit/terrain/AnalyticTerrainSource.hpp",
  "engine/terrain_bake/include/orbit/terrain_bake/TectonicBaker.hpp",
  "engine/terrain_bake/include/orbit/terrain_bake/PlanetBakeFile.hpp",
  "engine/terrain_bake/include/orbit/terrain_bake/RiverBaker.hpp",
  "engine/terrain/include/orbit/terrain/BakedRivers.hpp",
  "engine/terrain_bake/include/orbit/terrain_bake/TerrainBakeService.hpp",
  "engine/studio_session/include/orbit/studio_session/StudioTerrainBakeController.hpp",
  "engine/terrain_gpu/src/FieldGenerationCompute.hpp",
  "engine/terrain_bake/CMakeLists.txt"]
symbols = ["BakedTectonicRasters", "BakeTectonics", "SavePlanetBake", "LoadPlanetBake", "TerrainBakeService", "StudioTerrainBakeController", "TectonicBakeRecipeHash", "BakeRivers", "RiverBakeRecipeHash", "BakedRiverNetwork"]
invariants = [
  "Terrain generation never evaluates the plate model once a bake exists: GlobalTerrainFields reads boundary masks, plate bias and the structural layer from GlobalTerrainFieldDesc::bakedTectonics, and the GPU field generator uploads the same raster (SampleBakedConvergenceAndBias mirrors BakedTectonicRasters::SampleConvergenceAndBias; the interleaved texel is {convergence, plate bias, structural elevation}) and takes the plate-free path. Only closed-form hotspot chains are still evaluated per sample; they are too small for the raster.",
  "The first bake of a planet is blocking and happens in the first StudioSession tick, before any terrain page is built (StudioTerrainBakeController::Tick runs right after the composition refresh), so no page is ever generated from the plate model. Later recipe edits rebake in a background thread while the previous bake keeps driving terrain.",
  "A bake is installed only when it completes and validates (finite layers, matching sizes, checksummed file). A failed, cancelled or superseded bake leaves the running bake untouched; Cancel does not auto-restart until the recipe changes or a bake is started.",
  "The recipe hash (TectonicBakeRecipeHash) covers exactly what the baked layers depend on: planet radius, the effective seeds, plate count, plate shape and motion, crust biases, boundary width, reference speeds, oceanic scale, convergence uplift and the bake algorithm version. Hotspot and rain-shadow edits do not stale a bake. A bake is stale when its recipe hash differs from the current recipe or its resolution differs from the persisted resolution.",
  "Installing a bake changes AnalyticTerrainSource::Revision (the bake's content hash is mixed in), so terrain caches and physical pages rebuild from the new bake; the composition is rebuilt through EditorWorldSession::RebuildUniverse. The content hash covers every texel of every layer plus the plate and neighbour ids (it is the cache identity, so it is never sampled).",
  "Crust type is the continuous ContinentalCrustFraction layer (0 oceanic, 1 continental), independent of plate ids: a plate carries a mean tendency and warped low-frequency noise adds continent outlines inside it. Crust thickness and age derive from it; the plate flag (PlateIsContinental) still drives the collision-class masks and the plate bias. Subduction has polarity (SubductionTrench on the descending plate's side, VolcanicArc inland on the overriding side; the more oceanic, else older, plate descends), and the structural subsidence, uplift and arc volcanism use them instead of the symmetric arc mask. StructuralElevationMeters is baked structure-only elevation (ridge swell and age-depth deepening, rift floor and shoulders, trench, arc) added to coarse elevation by the CPU and the GPU generator; the bake's PlateBiasMeters comes from the crust fraction. The bake evaluates naturalised boundaries (TectonicField::Deform, via GlobalTerrainFields::EvaluateTectonicTexel). Each plate pair's boundary is an explicit small circle (p . a == h, valid only where the pair are the top two plates) carrying a generated sideways displacement along strike (plate-scale bends, regional segmentation, local splays, step-overs) that tapers to zero at triple junctions; space is warped perpendicular to the nearest boundary by that displacement inside a 0.30 rad corridor. The shift is capped at 0.16 rad so the warp stays one-to-one (plates never split or fold). The obliquity that rotates the boundary normal is the smoothed strike change implied by the displacement slope, so bends become transpression/transtension. The plate-id raster follows the warped geometry. FractureDensity is strike-parallel plus en-echelon ridged bands in boundary coordinates, gated by boundary activity, and strike-slip zones carry fault valleys in the structural elevation. The runtime plate model and its GPU mirror stay the clean, undeformed one (obliquity 0), and the deformation strengths are fixed constants, not a recipe setting. Bake format version 5, algorithm version 6: older .orbitbake files are rejected and rebaked.",
  "Rasters are quantized to 16 bits over a per-layer range and carry a one-texel gutter evaluated on the neighbouring face's geometry, so sampling is continuous across cube-face edges. Fidelity against the plate model at 128 texels per face: convergence within 0.021, plate bias within 5.4 m, crust thickness within 0.64 km; the plate-speed layer is inherently discontinuous at plate boundaries.",
  "The .orbitbake container is section based (tag, size, FNV-1a checksum, payload); unknown sections are skipped, a corrupt, truncated or unsupported file is reported rather than thrown, and Save writes to a temporary file and renames over the target. Bakes live in <project>/Bakes and are derived: the recipe on the Terrain Surface stays the only authority and a bake can always be rebuilt.",
  "The river graph (BakeRivers) is computed on the baked terrain: it samples elevation and precipitation per cube-face cell, priority-floods from the ocean so every river reaches the sea, accumulates discharge down the flow tree, keeps cells above the discharge threshold (1000 m3/s) as nodes with width 3.5*sqrt(Q) and depth 0.4*cbrt(Q), and smooths single-thread stretches. Its recipe hash is TerrainRecipeHash (the terrain recipe without rivers, with the installed tectonic bake) plus the grid and runoff parameters, so a hotspot, climate or erosion edit rebakes only the rivers and reuses the tectonic rasters.",
  "Rivers are a lookup: BakedRiverNetwork::Sample finds the nearest reach in a per-face 64x64 bucket grid (distance to the chord projected back onto the sphere), cuts depth*smoothstep(1 - d/(2*halfWidth)) into land only and never below the sea level; the GPU generator (RiverCarveDepth, binding 6) mirrors it to under 0.1 m. Footprint filtering spreads sub-footprint rivers and drops reaches narrower than footprint/20.",
  "Resolution and auto-rebake are persisted Terrain Process Settings (kProcessBakeResolution, kProcessBakeAutoRebake), edited from the Planet Tectonics menu's Planet Bake section and terrain.bake_set; terrain.bake_status/start/cancel and the menu's Bake Now/Cancel call the same StudioTerrainBakeController."]
related = ["/rendering/terrain/rivers", "/rendering/terrain/macro-geology", "/rendering/terrain/contracts"]
diagnose = [
  {symptom = "Terrain changed after I edited the plates but nothing visible happened", steps = [
    "orbit_terrain_bake_status: state stale or baking means the old bake is still driving terrain until the rebake finishes (progress shows how far).",
    "state failed: read error; the old bake is intentionally untouched. Fix the recipe or call orbit_terrain_bake_start.",
    "auto_rebake false: edits only mark the bake stale; call orbit_terrain_bake_start."]},
  {symptom = "Studio opens slowly on a new planet", steps = [
    "The first bake of a planet blocks open; at 256 texels it is well under a second in Release. Check orbit_terrain_bake_status resolution and last_bake_seconds; lower the resolution if it is large."]}]
depends_on = ["/foundation/core", "/foundation/math", "/rendering/terrain/contracts", "/world/planet-coordinates"]
verify = [
  "ctest -R Orbit.TerrainBake",
  "ctest -R Orbit.StudioTerrainBake",
  "ctest -R Orbit.TerrainGpuField"]
verified = ""
+++

# Planet bake

Terrain should not run a simulation while it generates. The plate model that
drives continents and mountain belts is smooth, so it rasterizes well: this
module evaluates it once per cube-face texel, stores the result in a project
file and lets every consumer sample that instead.

## What is baked

`BakedTectonicLayer` lists fifteen layers: the three boundary masks, the three
collision-class convergence masks, the continental/oceanic plate bias, crust
thickness and age, geological age, uplift, subsidence, stress, arc volcanism and
plate speed, plus nearest-texel plate ids. Hotspot chains are not baked; they
are closed form and added at sample time.

## Flow

1. `StudioTerrainBakeController::Tick` (each session tick) observes every
   terrain body's recipe (`TerrainBodyServices::Recipe`).
2. `TerrainBakeService::Observe` loads `<project>/Bakes/<planet>.orbitbake` on
   first sight; a missing bake is baked on the spot (`BakeNow`).
3. A changed recipe goes stale; once it stops changing for 0.75 s, a background
   bake starts. Nothing is installed until it finishes and validates.
4. The controller installs finished bakes on the body's services and rebuilds
   the composition, which composes the terrain source from the bake.

## Rivers

The same container carries a `RIVR` section. A bake is tectonics first, then the
river graph on top of them, then one file; `TerrainBakeService` reuses the
running tectonic rasters when only the river recipe changed. Installing the
graph (`TerrainBodyServices::SetRiverBake`) rebuilds the composition, which
attaches it to `AnalyticTerrainDesc::bakedRivers`; the bake's content hash is
part of the source revision. `terrain.bake_status` reports `rivers_active`,
`river_nodes`, `river_segments`, `river_bytes` and the river recipe hashes.

Physical pages consume the graph instead of generating basin-scale drainage:
pages are independent (no cross-page drainage exchange), their drainage takes
the baked discharge inside the bankfull channel, and their M16 river network is
the baked graph clipped to the page. What stays at run time is page-local:
the D8 solve and depression fill inside one page (small streams, lakes),
stream-power incision, and the old meander/cutoff solve for pages with authored
river constraints. Streams below the bake's 1000 m3/s threshold are therefore
only in the page drainage (lakes, material), not in the river network.

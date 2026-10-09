+++
path = "/rendering/terrain/contracts"
title = "Terrain contracts, sources and global fields (M00, M01)"
kind = "subsystem"
status = "stable"
summary = "The frozen terrain vocabulary: authority domains, residency, revision domains and generation triggers (TerrainContracts), the physical page address and key, planet-space surface positions (TerrainPosition), the TerrainSource interface with TerrainSample (elevation, climate, biome weights, standing water), and AnalyticTerrainSource with global fields, procedural craters, authored chronological impact history and stress-guided ice fractures."
owner_module = "OrbitTerrain"
keywords = ["terrain contracts", "authority domain", "revision domain", "terrain source", "terrain sample", "physical page address", "analytic terrain", "tectonic", "plates", "hotspot", "seed domain", "stable hash", "canonical position"]
sources = [
  "engine/terrain/include/orbit/terrain/AnalyticTerrainSource.hpp",
  "engine/terrain/include/orbit/terrain/GlobalTerrainFields.hpp",
  "engine/terrain/include/orbit/terrain/TectonicFieldDesc.hpp",
  "engine/terrain/include/orbit/terrain/TectonicStructure.hpp",
  "engine/terrain/include/orbit/terrain/BakedTectonics.hpp",
  "engine/terrain/include/orbit/terrain/BakedGeology.hpp",
  "engine/terrain/src/BakedGeology.cpp",
  "engine/terrain/include/orbit/terrain/TerrainContracts.hpp",
  "engine/terrain/include/orbit/terrain/TerrainFields.hpp",
  "engine/terrain/include/orbit/terrain/TerrainPosition.hpp",
  "engine/terrain/include/orbit/terrain/TerrainSource.hpp",
  "engine/terrain/CMakeLists.txt"]
symbols = ["TerrainAuthorityDomain", "TerrainGenerationRevisions", "TerrainGenerationTrigger", "PhysicalTerrainPageAddress", "PlanetSurfacePosition", "TerrainSample", "AnalyticTerrainSource", "GlobalTerrainFields"]
invariants = [
  "Authored intent, canonical physical state and derived/view state stay separate so editor, renderer and cache code cannot silently become terrain simulation authorities: only TerrainAuthorityDomain::TerrainPhysical owns canonical terrain, only WaterPhysical owns canonical water, and DerivedCache and ViewInterest are derived-only.",
  "Residency (Absent, Cpu, Gpu, CpuAndGpu) says where a representation is materialised and is deliberately neither authority nor identity.",
  "Physical products may be generated for exactly three reasons: MissingPhysicalPage, AuthorityRevisionChanged or ExplicitBake. A view may reveal that a page is missing, but camera motion itself is never a generation or invalidation reason.",
  "Every domain that can change canonical generated terrain has an explicit, monotonic revision (geology, climate, authoring, biome, water, processes, hydrology boundary); view/camera/render revisions are intentionally absent, so they cannot invalidate physical products.",
  "Terrain Surface semantic properties persist the procedural tectonic recipe; SurfaceComposition reconstructs TectonicFieldDesc from those properties, and the Planet toolbar, terrain generator and Flat Map layer share that same field rather than keeping a parallel editor-only plate model.",
  "PhysicalTerrainPageAddress is a stable planet-space address with no render clipmap ring, GPU slot or frame state, so moving a camera cannot change physical page identity by construction; resolution and generation revisions invalidate cached output without changing the address.",
  "Persisted/cache identity and procedural seeds use fixed, platform-independent mixing (StableCombine64); std::hash is deliberately excluded because its representation is not an Orbit persistence contract; TerrainSeedDomain numeric values are persisted domain separators: append new domains, never renumber.",
  "AnalyticTerrainDesc impactHistory and iceFractures are immutable process authority compiled once into indexed fields; their full recipe content contributes to the terrain source revision.",
    "When TerrainBakeService installs a version-4 BakedGeologyRasters product, AnalyticTerrainSource samples persisted chronological impact/ice relief and process channels. CPU queries use the same scale-space level for age, excavation, ejecta, melt, breccia, micro-impact, resurfacing and fracture-damage metadata, so they do not evaluate the impact or fracture indexes. Legacy GEO1 versions without process channels retain the indexed-query fallback until the replacement bake activates.",
  "Without a geology bake, AnalyticTerrainSource::Sample evaluates indexed M07 impact and fracture fields and carries those same process samples into regional M08 compilation. With a bake, it reads those outputs from the GEO1 raster; either path gives the physical material-column builder excavation, ejecta, melt, breccia, micro-impact, resurfacing-age and fracture-damage channels.",
  "Terrain locations are canonical physical surface positions in planet/body space, never a cube face, clipmap or render patch (PlanetSurfacePosition, TerrainSampleFootprint).",
  "GlobalTerrainFields::SampleTectonicStructure(direction) is the planet structural layer: plate and neighbour plate, boundary type/strength, crust thickness (continental ~35-43 km thickening under collision, oceanic ~7-8.5 km thinning at spreading centres), crust age and geological age (0 new, 1 ancient), uplift, subsidence, stress and volcanism. It is derived analytically from the persisted TectonicFieldDesc, is a pure thread-safe function of direction, and never feeds back into plate placement. With a bake, the structure does shape terrain: the plate bias comes from the continuous crust fraction (not the plate flag), the orogenic belt is (0.65 + 0.35 * ridge noise * mountainWeight) * convergence so it survives zero noise and does not fade with the sample footprint (only the noise sculpting does; fading the whole belt made it ~6 km high on fine pages and 1.5-4 km on coarse ones, a hard seam wherever two levels met; Orbit.TerrainBake checks the spread across footprints), and structuralElevationMeters (ridge swell and age-depth, rift floor and shoulders, one-sided trench, arc) is added to coarse elevation on CPU and GPU. Without a bake the plate model has no structural elevation (it is zero). A baked CollisionLand mask keeps thick continental collisions above sea level and counts them as land, so a belt never drowns under the noise coastline. Hydrology, biomes and hazards should query it instead of re-deriving plate logic; it is exposed as terrain.tectonics_sample / orbit_terrain_tectonics_sample and in the Planet Tectonics menu.",
  "Plate boundary masks (convergence, divergence, transform) and the continental/oceanic bias are continuous everywhere: TectonicField::Sample evaluates every pair among the plates within one boundary width of the top (weight = min of each plate's closeness to the top and the pair's closeness to each other, which reduces to the original (d0 - d1) mask for two plates) and takes a max, instead of using only the nearest and runner-up plate, whose boundary normal and relative velocity jumped along the line where the runner-up changed and cut mountain belts with straight edges starting at triple junctions. The bias and the structural-layer crust thickness/age use symmetric g / (1 - g) weights that reproduce the original two-plate lerp. Collision-type weighting uses the continuous convergenceContinental/Mixed/Oceanic masks, not the nearest/second plate flags. The GPU generator's SampleTectonicConvergenceAndBias mirrors this (kMaxBoundaryCandidates = 8); Orbit.TectonicStructure sweeps the field and fails on any jump over 0.25.",
  "GlobalTerrainFieldDesc::bakedTectonics (shared, immutable BakedTectonicRasters) replaces plate evaluation: when set, TectonicAt, PlateElevationEstimateMeters and SampleTectonicStructure read baked rasters and only evaluate the closed-form hotspot chains; its content hash is part of AnalyticTerrainSource::Revision, so swapping a bake means composing a new source and invalidates caches. GlobalTerrainFields::EvaluateTectonicTexel is the plate-model evaluation the baker rasterizes and is not for terrain generation. See /rendering/terrain/bake.",
  "The GPU and CPU generators must render the SAME plates and hotspots: GlobalTerrainFields exports the tectonic state for the GPU to upload verbatim instead of regenerating it from a different hash, so a range on the 2D map is the range the 3D clipmap shows (/rendering/terrain/clipmaps/generator-parity).",
  "Plate convergence/divergence/transform masks are direction-only (never footprint-dependent), and nearby plate types let a caller tell a continental collision from a subduction zone or a spreading ridge from a rift."]
related = ["/rendering/terrain", "/rendering/terrain/macro-geology", "/rendering/terrain/invalidation", "/world/planet-coordinates"]
depends_on = ["/foundation/core", "/foundation/math", "/world/planet-coordinates"]
used_by = ["/celestial/appearance", "/celestial/compact-objects", "/celestial/far-render", "/celestial/giants", "/celestial/globe", "/celestial/magnetosphere", "/celestial/ocean", "/celestial/rings", "/celestial/small-bodies", "/celestial/stellar", "/editor/studio-session", "/rendering/lighting/radiance-cache", "/rendering/terrain/debug-fields", "/rendering/terrain/erosion", "/rendering/terrain/geology", "/rendering/terrain/gpu-passes", "/rendering/terrain/hydrology", "/rendering/terrain/impacts", "/rendering/terrain/macro-geology", "/rendering/terrain/material-column", "/rendering/terrain/page-cache", "/rendering/terrain/regions", "/rendering/terrain/streaming", "/world/path-routing", "/world/surface-composition", "/world/surface-registry", "/world/terrain-constraints"]
verify = [
  "ctest -R Orbit.PlanetTerrain",
  "ctest -R Orbit.TerrainMountains",
  "ctest -R Orbit.TectonicStructure",
  "ctest -R Orbit.TerrainFields"]
verified = "b0a0de7f"
+++

Everything under `/rendering/terrain` builds on this module; the physical-process authority chain is documented on `/rendering/terrain`.

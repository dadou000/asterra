# GPU-first terrain synthesis

## Contract

The CPU is responsible only for generating/loading the persistent coarse planet maps. Once the world has been adopted, terrain below the coarse-map scale is a GPU problem.

No camera-centred CPU terrain generation, CPU detail height synthesis, or CPU material placement belongs in the final runtime path.

## Pipeline

1. **Offline / load-time planet context**
   - macro elevation
   - tectonic uplift / plate context
   - bedrock family, erodibility, faults, strata, basins
   - coarse erosion sediment
   - drainage / discharge / floodplain / wetland
   - climate
   - soil depth and composition
   - biome / vegetation potential

2. **GPU geomorph synthesis**
   - reconstruct coarse elevation
   - derive continuous landform weights
   - domain-warped multi-scale noise
   - cellular/Voronoi structural ridges
   - mountain/crag synthesis
   - analytic drainage incision
   - thermal/sedimentary redistribution approximations
   - dune/badland/glacial/depositional processes
   - output final displaced terrain height

3. **GPU surface classification**
   - derive slope from final displaced geometry
   - derive curvature/exposure/relative height where available
   - combine final terrain state with coarse geology, climate, soil and biome context
   - select/blend physically plausible materials
   - enforce material stability (angle of repose / exposed bedrock)

4. **GPU material detail**
   - PBR texture/material family
   - material-specific micro-normal/parallax/displacement
   - near-field microgeometry/tessellation equivalent

5. **GPU scatter**
   - vegetation, trees, rocks, talus, debris, river stones, etc.
   - consume the exact same context and final material classification

## First implementation milestones

### M1 — coarse context textures
Build six-face GPU textures once when the baked world is adopted. They are immutable until another world is loaded.

Initial packed datasets:

- Soil: sand / silt / clay / organic
- Surface state: soil depth / soil moisture / vegetation / sediment
- Geology: rock family / erodibility / strata dip / basin
- Structure: uplift / fault / floodplain / wetland
- Climate: temperature / precipitation / humidity / seasonal range
- Hydrology: flow direction / discharge / depositional influence

These textures are the authoritative GPU inputs below the coarse-map scale.

### M2 — geomorph shader library
Implement deterministic GPU functions for:

- band-limited value/fBm/ridged noise
- domain warping
- inexpensive 3D cellular ridge structure
- context-driven mountain/arid/glacial/depositional weights
- first-pass analytic incision and deposition

### M3 — active clipmap integration
Replace the current altitude-only procedural detail heuristic in `spherical_geometry_clipmap_procedural_uv.gdshader` with the geomorph library. Preserve existing LOD band limiting and fine/coarse morph identity.

### M4 — material classifier
Replace the current grass/wet/sand/snow + slope-rock blend with a classifier driven by final displaced slope plus coarse context. Bedrock exposure must resolve to the actual geological family.

### M5 — near-field refinement
Add higher-density near geometry and/or GPU compute-generated local height fields for iterative erosion and material micro-displacement. The CPU remains uninvolved.

## Runtime invariant

For a fixed world seed and coarse map set, all untouched sub-grid terrain must be reproducible from planet position and GPU algorithms alone.

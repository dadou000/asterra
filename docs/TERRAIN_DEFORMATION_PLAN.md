# Asterra deformable terrain plan

## Goal

Make terrain a finite-strength environment rather than an infinitely rigid collider. The same solver must eventually serve wheels, tracks, landing gear, excavator tools, building foundations, falling objects and structural failures.

The authoritative surface remains:

`height = pristine procedural terrain + Deltas`

The new `TerrainDeformation` layer decides when contact stress is sufficient to change `Deltas`, and carries sparse mechanical state for already-disturbed ground.

## Implemented now

### 1. Sparse mechanical state

`TerrainDeformation` stores only touched 64x64 lattice tiles. Each touched cell currently carries:

- compaction
- plastic/shear damage

Untouched ground has no allocation. The state has serialize/deserialize entry points so it can be moved into save data later without changing the solver API.

### 2. Finite bearing strength

Contacts submit:

- surface direction
- effective footprint radius
- normal load
- penetration depth
- normal speed
- tangential speed
- cutting factor
- material preset

The solver calculates an effective yield pressure. Compaction hardens the material and damage weakens it. Support force is finite and is composed from yield strength, penetration stiffness and velocity damping.

If demanded contact pressure exceeds the effective yield pressure, the ground accumulates permanent sinkage rather than producing an arbitrarily large rigid collision impulse.

### 3. Shear/cutting failure

A tool with tangential speed and a cutting factor produces a shear demand. If it exceeds the material shear strength, extra plastic displacement is generated. This is the first common path for excavator teeth, blades and future cutting tools.

### 4. Compaction and displaced material

Plastic deformation increases compaction and damage. A configurable fraction of displaced volume is raised into a surrounding rim. The missing fraction represents pore collapse/compaction rather than deleted bulk material.

This is deliberately a heightfield approximation. A later loose-material layer will carry fully excavated material into buckets, piles and avalanching cells.

### 5. Material response presets

The experiment currently exposes four mechanical presets:

1. dry topsoil
2. wet clay
3. compacted gravel
4. competent rock

They differ in yield pressure, hardening, stiffness, damping, shear strength, plastic rate, cutting rate, compaction and restitution.

### 6. Batched visual publication

Mechanical changes are written immediately to authoritative `Deltas`, but visual dirty notifications are rate-limited. This prevents continuous contacts from forcing a 512x512 edit-window rebuild on every physics substep.

`TerrainEditDeltaGPU` now owns two R32F edit textures. It rebuilds the inactive texture and swaps it into the terrain material only after the new image has been uploaded. The renderer therefore never samples the texture currently being rebuilt.

### 7. Interactive experiment

Press `F10` while running the Main world.

The harness provides:

- a 1.25 m radius tungsten sphere (~157 t) that can be placed above the aimed ground and dropped
- a manipulable excavator-bucket proxy with five active teeth
- live bearing ratio, sink rate, bucket reaction force and deformation statistics
- runtime switching between the four mechanical material presets

Controls:

- `F10`: toggle experiment
- `R`: place the tungsten sphere above the current aim point and drop it
- `P`: pause/resume sphere
- `Backspace`: reset sphere suspended above the aim point
- `I/K`: bucket forward/back
- `J/L`: bucket left/right
- `U/O`: bucket up/down
- `Z/X`: bucket pitch
- `Shift`: fast bucket movement
- `B`: reset bucket
- `1..4`: choose material preset
- `Delete`: clear terrain edits and deformation state

## Next production steps

### Phase A - active high-resolution deformation tiles

The persistent Deltas lattice is appropriate for world-scale edits but too coarse for bucket teeth and tire lugs. Add active local deformation tiles at approximately 0.125-0.25 m spacing around interacting objects. Contacts rasterize into the active tile; inactive results are conservatively downsampled into persistent Deltas.

Targets:

- allocate only around active contacts
- GPU compute for pressure rasterization, plastic update and slope relaxation
- asynchronous downsample/commit to sparse persistent tiles
- no dependence on camera position for physics authority

### Phase B - contact aggregation from the physics solver

Replace experiment-specific contact submission with a common contact collector that consumes contact impulses and effective footprint geometry from simulated assemblies.

Required paths:

- wheels and tires: elliptical patches
- tracks: strips/polygons
- landing gear: tire patches
- arbitrary rigid bodies: projected contact manifold
- foundations: persistent polygons and distributed loads
- excavator teeth/blades: small high-pressure cutting patches

Contacts should be accumulated for one physics step, rasterized once per active tile, then solved once. Cost then scales mostly with touched terrain area rather than object count.

### Phase C - loose material and excavation

Add a second conserved height/mass field for disturbed loose material.

- failed solid ground transfers mass into loose material
- buckets can capture loose material based on swept volume and bucket occupancy
- dumped material becomes piles
- piles relax toward material angle of repose
- compaction can convert loose material back into load-bearing ground

### Phase D - foundations and long-term settlement

Foundation contacts reuse the same footprint API but add slow consolidation state.

- static bearing failure
- differential settlement
- moisture-dependent consolidation
- cyclic compaction from traffic
- foundation tilt and resulting structural loads

### Phase E - impact/cratering

High-energy impacts add an energy-limited failure mode on top of pressure yield.

- normal kinetic energy budget
- fracture/soil work budget
- radial ejecta/rim redistribution
- material-dependent damping and rebound
- optional object deformation sharing the same energy budget

The important invariant remains that impacts are not scripted crater effects. A crater is the result of the same finite-strength terrain mechanics used by a wheel or excavator.

## Performance direction

The final architecture should have three scales:

1. procedural global terrain: no mutable storage for untouched ground
2. sparse persistent deformation: meter-scale, saveable, world-scale
3. active physics tiles: decimeter-scale, short-lived, GPU-updated near contact

Physics consumes authoritative active/persistent state immediately. Rendering receives completed deformation buffers through a swap. This keeps contact response deterministic and low-latency while allowing visual updates to be asynchronous.

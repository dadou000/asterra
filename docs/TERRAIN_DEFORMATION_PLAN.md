# Asterra deformable terrain plan

## Goal

Make terrain a finite-strength environment rather than an infinitely rigid collider. The same solver must eventually serve wheels, tracks, landing gear, excavator tools, building foundations, falling objects and structural failures.

The surface is now split into three layers:

`height = pristine procedural terrain + persistent Deltas + active GPU deformation`

`TerrainDeformation` computes the small latency-sensitive contact response. `TerrainDeformationGPU` owns the expensive decimeter-scale plastic raster near active contacts.

## Implemented now

### 1. Sparse persistent mechanical state

`TerrainDeformation` stores only touched 64x64 persistent lattice tiles. Each touched cell currently carries:

- compaction
- plastic/shear damage

Untouched ground has no persistent allocation. The state has serialize/deserialize entry points so it can remain saveable while the active GPU layer evolves.

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

The CPU-side contact model calculates an effective yield pressure. Compaction hardens the material and damage weakens it. Support force is finite and is composed from yield strength, penetration stiffness and velocity damping.

If demanded contact pressure exceeds the effective yield pressure, the ground accumulates permanent sinkage rather than producing an arbitrarily large rigid collision impulse.

### 3. Shear/cutting failure

A tool with tangential speed and a cutting factor produces a shear demand. If it exceeds the material shear strength, extra plastic displacement is generated. Excavator teeth and future blades use this same path.

### 4. GPU active deformation tile

The first high-resolution GPU stage is implemented in `TerrainDeformationGPU`.

Current active tile:

- 256 x 256 cells
- 0.25 m per cell
- 64 m x 64 m coverage
- RGBA32F state
- R = active height delta
- G = compaction
- B = damage
- A = reserved for a later loose-material quantity

Contacts are accumulated during the physics frame and rasterized together by `terrain_deformation_active.glsl`. One compute invocation owns one texel, so the update requires no atomics. Up to 64 contacts are evaluated in a dispatch.

The field uses two GPU textures in ping-pong form. A compute pass reads texture A and writes texture B, then the visible texture swaps. The next pass reverses the direction. Compute therefore never modifies its own source state.

The active deformation texture is sampled directly by the terrain shader and by GPU scatter placement. Persistent `Deltas` and the active field are added together visually.

### 5. Physics sampling without GPU stalls

Gameplay never performs a synchronous readback. `TerrainDeformationGPU` keeps an asynchronous CPU mirror of the active tile for contact height/normal queries.

`TerrainContactSampler` combines:

1. pristine precise GPU terrain query
2. persistent `Deltas`
3. latest asynchronous active-GPU deformation sample

The active mirror is intentionally allowed to lag the rendered field slightly. Soft-ground contact tolerates this better than stalling the entire render/physics pipeline for a synchronous texture read.

### 6. CPU fallback remains available

The original sparse CPU raster remains in `TerrainDeformation` as a fallback for:

- unsupported rendering methods
- compute initialization/dispatch failure
- contacts outside the current active GPU tile
- temporary queue overflow

This keeps the experiment functional while the GPU system is expanded to multiple active tiles.

### 7. Compaction and displaced material

Plastic deformation increases compaction and damage. A configurable fraction of displaced volume is raised into a surrounding rim. The depression kernel and GPU rim kernel are approximately volume matched; the remaining fraction represents pore collapse/compaction.

This is still a heightfield approximation. A later loose-material layer will carry fully excavated material into buckets, piles and avalanching cells.

### 8. Material response presets

The experiment exposes four mechanical presets:

1. dry topsoil
2. wet clay
3. compacted gravel
4. competent rock

They differ in yield pressure, hardening, stiffness, damping, shear strength, plastic rate, cutting rate, compaction and restitution.

### 9. Interactive experiment

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

## Current GPU-stage limitations

The new active layer is intentionally the first tile implementation, not the finished planetary allocator.

- only one 64 m active GPU tile is allocated at present
- contacts outside it fall back to persistent CPU deformation
- the active tile is not yet downsampled/committed into persistent `Deltas` on save or recenter
- CPU contact sampling uses an asynchronous active-field mirror rather than direct synchronous GPU state
- loose excavated mass is not yet a separately conserved GPU field
- slope/angle-of-repose relaxation is not yet dispatched

These are the next implementation boundaries, not changes to the contact API.

## Next production steps

### Phase A2 - multi-tile active allocator and persistence

Turn the single proven GPU tile into a sparse active-tile pool.

Targets:

- allocate 0.125-0.25 m tiles around actual contacts rather than the camera
- LRU/idle retirement
- asynchronous conservative downsample into persistent `Deltas`
- transfer compaction/damage into persistent sparse state
- seamless tile migration without double-counting height
- save requests wait only on retiring dirty tiles, not on the whole GPU

### Phase B - contact aggregation from the physics solver

Replace experiment-specific contact submission with a common collector that consumes contact impulses and effective footprint geometry from simulated assemblies.

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

A crater remains a result of the same finite-strength terrain mechanics used by a wheel or excavator, not a scripted crater effect.

## Performance direction

The final architecture has three scales:

1. procedural global terrain: no mutable storage for untouched ground
2. sparse persistent deformation: meter-scale, saveable, world-scale
3. active physics tiles: decimeter-scale, short-lived, GPU-updated around contact

The expensive operation is now proportional to active tile area and the number of aggregated contacts, not to the size of the planet or the persistent edit lattice.

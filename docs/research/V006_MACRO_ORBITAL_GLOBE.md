# Orbit V0.0.6 — Macro-displaced Orbital Globe

Status: M15 implementation baseline.

## Purpose

M15 provides the first concrete render product selected by the M14 celestial representation ladder.

The macro globe is a disposable derived representation of the existing terrain authority. It does not own terrain, surface identity, or body shape.

## Authority

Inputs:

- authoritative Celestial Body reference shape;
- authoritative TerrainSource attached through SurfaceRegistry;
- terrain source revision;
- macro-globe tessellation configuration.

The globe never persists sampled elevation as authoring state.

If terrain authority changes, TerrainSource::Revision() changes and the derived globe fingerprint changes.

## Geometry

The CPU reference implementation builds a cube-sphere with six independently parameterized faces.

Configuration currently exposes:

- face resolution;
- terrain sampling footprint scale.

Each vertex direction is normalized from the cube face and then displaced radially by:

```
radius(direction) =
    referenceShapeRadius(direction) +
    terrain.Sample(direction, footprint).elevation
```

Ellipsoid reference shapes are supported through directional radius evaluation.

## Terrain filtering

The terrain query footprint is derived from the angular cell size of the globe:

```
angularCell ~= (pi / 2) / (faceResolution - 1)
footprint ~= referenceRadius * angularCell * footprintScale
```

This intentionally filters high-frequency terrain that the orbital globe cannot spatially represent.

The result preserves continental, mountain-chain and other macro-scale silhouette displacement without aliasing local ground detail into orbital geometry.

## Normals and seams

Cube-face edge vertices are duplicated by topology, but normals are not computed independently from face-local triangle accumulation.

Instead, the normal at each direction is derived by finite differencing the same displaced terrain function in two tangent directions.

Therefore equivalent cube-face edge directions sample the same terrain function and receive seam-stable lighting normals.

Triangle winding is validated against the body center for every quad so all faces have outward geometry and deterministic back-face culling.

## Revision fingerprint

MacroGlobeFingerprint combines:

- TerrainSource revision;
- face resolution;
- footprint scale;
- sphere radius or ellipsoid radii.

Studio can query this fingerprint without rebuilding the mesh.

CPU geometry and GPU buffers are rebuilt only when the fingerprint changes.

## GPU product

GpuMacroGlobeProduct contains normalized float vertex positions, float normals and a UInt32 index buffer.

The buffers are ordinary disposable RHI resources.

The body reference radius remains outside the vertex payload and is used to normalize body-local camera coordinates for rendering.

No GPU buffer becomes semantic or terrain authority.

## Renderer

MacroGlobeRenderer is an indexed raster path using the shared RHI and shader compiler.

The renderer currently supplies neutral orbital preview lighting only.

Material/appearance ownership deliberately remains outside M15 and moves to M16 Planetary Appearance System.

## Studio integration

The existing BodyMap/orbital inspection path uses MacroGlobe whenever the targeted body has an attached TerrainSource.

Perspective ground rendering remains on the production toroidal terrain path.

M17 will own automatic surface <-> globe transition behavior; M15 does not introduce a premature hard distance switch.

Studio caches the product by body, terrain revision and full macro-globe fingerprint.

Changing presentation away from MacroGlobe disposes the per-view cached product.

## Validation

Deterministic CPU coverage includes:

- expected cube-sphere vertex/index counts;
- positive footprint;
- terrain displacement above and below reference radius;
- unit-length normals;
- revision-driven fingerprint changes;
- cheap fingerprint equality with generated product;
- ellipsoid shape participation in fingerprint and geometry.

Studio presentation policy regression verifies terrain-backed BodyMap selects MacroGlobe.

## Intentional limits

M15 does not yet implement:

- planetary appearance textures;
- oceans/ice/cloud material integration;
- seamless production-surface overlap;
- smooth globe;
- impostor/point rendering;
- async GPU upload/budget scheduling.

Those belong to M16-M18 and M31.

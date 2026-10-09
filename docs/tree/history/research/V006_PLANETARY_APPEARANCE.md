# Orbit V0.0.6 — Planetary Appearance System

Status: M16 implementation baseline.

## Purpose

M16 derives far-view material appearance from existing terrain/climate/biome/water authority.

It does not introduce a second surface database, material authoring hierarchy, or persistent orbital texture authority.

The product is disposable and revisioned.

## Inputs

The current baseline consumes only existing TerrainSource outputs:

- elevation;
- coarse elevation;
- climate temperature;
- humidity/precipitation-derived biome classification already present in TerrainSample;
- normalized biome weights;
- standing-water depth;
- TerrainGenerationRevisions;
- TerrainSource::Revision().

The same directional sampling convention used by M15 is retained.

## Channels

Each derived appearance texel contains:

- linear albedo;
- terrain normal;
- roughness;
- ocean mask;
- ice/snow mask;
- linear emission.

### Albedo

Land albedo is blended from ordinary biome weights using stable baseline reflectance colors for:

- desert;
- grassland;
- temperate forest;
- boreal forest;
- tundra;
- alpine;
- wetland.

Standing water overrides the far-view surface with a deep-ocean albedo baseline.

Ice/snow blends toward a high-reflectance cold-surface value.

These colors are render derivations, not biome authority.

### Roughness

Roughness is derived from biome composition.

Standing water uses a low roughness baseline.

Ice increases the water/land result toward an icy-surface baseline.

M24 may replace or augment this simple far-view ocean response with a dedicated orbital ocean model.

### Ocean mask

The current mask is derived directly from canonical standing-water depth.

No separate coastline or ocean texture is authored.

### Ice/snow mask

Ocean ice is derived from climate temperature using configurable freeze/full-ice thresholds.

Cold land receives additional snow/ice coverage weighted by tundra/alpine biome authority.

M21-M24 may later replace this simple baseline with atmosphere/ocean/weather-derived physical state, without changing the appearance product contract.

### Normals

Normals are finite-difference derivatives of the same terrain source sampled at the orbital footprint.

This intentionally matches the M15 macro-displaced surface and remains seam-stable across cube-face boundaries.

### Emission

The channel is explicitly present but currently zero.

Terrain/climate/biome/water authority has no canonical city-light, lava, fire or other emissive field.

M16 therefore does not invent emission data.

Future emission authorities can populate the same channel without changing downstream render contracts.

## Sampling footprint

Appearance uses the same cube-face resolution and footprint policy as M15:

```
angularCell ~= (pi / 2) / (faceResolution - 1)
footprint ~= referenceRadius * angularCell * footprintScale
```

This prevents unresolved local biome/terrain details from aliasing into orbital material data.

## Revision fingerprint

The appearance fingerprint includes:

- complete TerrainGenerationRevisions fingerprint;
- TerrainSource::Revision();
- body reference radius;
- face resolution;
- footprint scale;
- ice threshold parameters.

A geology, climate, biome, water or process-authority revision therefore invalidates the derived far-view product.

## CPU product

PlanetaryAppearanceProduct stores six cube faces in the same face-major/x-y topology used by M15.

Matching configuration produces exactly one appearance texel per macro-globe vertex.

This is deliberate: the current renderer can consume appearance immediately without introducing a second directional lookup implementation.

## GPU product

GpuPlanetaryAppearanceProduct uploads the full appearance payload as an RHI Structured buffer in ShaderResource state.

The buffer is a disposable derived resource.

It is retained even though the current M16 macro-globe renderer also packs matching channels into its vertex stream. The structured form is the forward seam for:

- smooth globe;
- disc impostor generation;
- compute filtering;
- diagnostics;
- future texture baking.

## Macro-globe integration

GpuMacroGlobeVertex now carries:

- normalized displaced position;
- geometry normal;
- albedo;
- appearance normal;
- roughness/ocean/ice material channels;
- emission.

MacroGlobeRenderer uses the appearance normal, albedo, roughness and masks for the live BodyMap orbital view.

The current lighting is intentionally simple; M19 owns radiometric HDR lighting.

## Studio cache and diagnostics

Studio builds the M15 geometry and M16 appearance products together.

The cache invalidates if either:

- geometry fingerprint changes;
- appearance fingerprint changes;
- body identity changes;
- terrain source revision changes.

Studio retains both the packed globe GPU product and the independent GPU appearance buffer.

StudioMacroGlobeDiagnostics exposes:

- body ID;
- terrain revision;
- geometry fingerprint;
- appearance fingerprint;
- appearance texel count.

This is the diagnostic seam consumed by later M32 quality/debug UI work.

## Validation

Deterministic tests cover:

- cube-face product size;
- positive orbital sampling footprint;
- fingerprint reproducibility;
- revision-driven invalidation;
- unit normals;
- roughness bounds;
- ocean presence;
- dry-land presence;
- cold ice/snow presence;
- explicit zero emission;
- one-to-one topology alignment with the M15 macro globe.

## Intentional limits

M16 does not yet implement:

- atmospheric scattering;
- cloud coverage;
- physically resolved ocean BRDF/waves;
- city lights;
- lava/fire emissive authority;
- spectral albedo;
- seasonal snow transport;
- asynchronous appearance baking/cache budgets.

Those extend this derived appearance contract in M19-M24/M31 rather than creating another surface authority.

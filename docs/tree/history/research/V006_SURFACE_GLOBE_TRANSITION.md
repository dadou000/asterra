# Orbit V0.0.6 — Production Surface <-> Macro Globe Transition

Status: M17 implementation baseline.

## Purpose

M17 removes the hard presentation boundary between the accepted production toroidal terrain path and the M15/M16 macro-displaced orbital globe.

The handoff is driven by the M14 projected-error resolver.

It is not driven by a fixed altitude threshold.

## Shared authority

Both representations consume the same body and terrain authority:

- the production path samples the composed TerrainSource into toroidal clipmap products;
- the macro globe samples that same TerrainSource at a larger orbital footprint;
- M16 appearance derives from the same TerrainSource climate/biome/water outputs.

No transition-owned surface data is persisted.

## Transition metric

The M14 resolver computes:

```
projectedDetailErrorPx =
    projectedBodyRadiusPx *
    maximumProductionDetailMeters /
    bodyRadiusMeters
```

Studio currently derives `maximumProductionDetailMeters` conservatively from the active AnalyticTerrainSource recipe as the larger of:

- twice the configured local detail amplitude;
- configured mountain relief.

The camera inputs are the real body-centered observer distance, current vertical FOV and viewport height.

Therefore zoom/FOV and viewport resolution affect the transition naturally.

## Overlap function

M17 adds `ResolveSurfaceGlobeTransition`.

The center of the overlap is the same production-surface projected-error threshold used by M14.

The half-width is:

```
threshold * hysteresisFraction
```

Across the band, a cubic smoothstep produces complementary weights:

```
productionWeight = 1 - t
macroGlobeWeight = t
```

The weights always sum to one when both representations are available.

Outside the band:

- high projected detail error -> production surface only;
- low projected detail error -> macro globe only.

If one representation is unavailable, the other receives full weight.

## Hysteresis

Representation identity continues to use the M14 per-body `RepresentationTracker`.

The visual overlap is computed from the same threshold metric independently of temporary hysteresis retention.

This avoids a hard visual pop when the selected logical representation is held across the threshold.

## Rendering

Perspective rendering now follows three states.

### Production side

The existing production terrain renderer is unchanged.

It retains its established:

- toroidal clipmap topology;
- streaming;
- physical-page integration;
- morphing;
- terrain shader;
- depth path.

### Overlap

Production terrain renders first.

The identical cached M15/M16 macro globe then draws with standard RHI alpha blending:

```
src = macro globe
dst = production terrain
alpha = macroGlobeWeight
```

Vulkan uses:

- SRC_ALPHA;
- ONE_MINUS_SRC_ALPHA.

No custom compositor or second render backend was introduced.

### Globe side

When the production weight reaches zero, the production terrain draw pass is omitted.

The globe clears the orbital background and renders at opacity 1.

The production terrain runtime may remain resident for now so returning toward the ground does not require immediate reconstruction.

M31 owns broader residency/budget policy.

## Coordinate alignment

The two representations use different rendering coordinate forms but the same body frame.

The macro globe is rendered directly in body-centered coordinates.

The production terrain renderer expresses nearby body-surface geometry in the observer's transported surface frame for precision.

`TerrainCameraFromBodyCamera` transforms the same body-frame forward/up vectors into that local surface basis.

Therefore the two views differ only in representation and numerical coordinate form, not physical orientation or body identity.

## Macro globe reuse

Perspective and BodyMap call the same `EnsureMacroGlobePresentation` path.

That path builds/caches one M15 geometry + M16 appearance product keyed by:

- body identity;
- TerrainSource revision;
- macro-globe geometry fingerprint;
- appearance fingerprint.

The transition cannot accidentally use a separate orbital mesh from BodyMap.

## Alpha-capable globe renderer

`MacroGlobeRenderer::Draw` now accepts an opacity.

The renderer uses the existing RHI `BlendMode::Alpha`.

Opacity is passed through the vertex shader into the pixel shader explicitly.

BodyMap continues to render with opacity 1.

## Diagnostics

`StudioSurfaceGlobeTransitionDiagnostics` exposes per viewport:

- body;
- currently selected M14 representation;
- lower-fidelity neighbor;
- production surface weight;
- macro globe weight;
- projected body radius;
- projected production-detail error;
- projected macro-displacement error;
- hysteresis-held state;
- overlap-active state.

This is the M17 diagnostic seam for M32.

## Validation

Deterministic resolver tests cover:

- production-only side;
- exact 50/50 overlap midpoint;
- globe-only side;
- complementary weight conservation;
- missing-globe fallback;
- missing-production fallback;
- existing M14 hysteresis and quality behavior.

Studio still keeps its presentation-policy regression, while the actual Perspective handoff now occurs inside the production presentation path.

## Intentional limits

M17 does not yet:

- transition macro globe to smooth globe/impostor/point;
- budget retained terrain/globe resources;
- add atmospheric/cloud/ocean layers to the overlap;
- implement temporal upsampling;
- asynchronously prewarm derived products.

Those belong to M18, M21-M24 and M31.

# Orbit V0.0.6 — Far Celestial Representations

Status: M18 implementation baseline.

## Purpose

M18 completes the M14 far-distance representation ladder after the production terrain and macro-displaced globe stages implemented by M15-M17.

The remaining ladder is:

```
Macro-displaced Globe
    -> Smooth Globe
    -> Analytic / Cached Disc Impostor
    -> Point / Stellar Point Proxy
```

All products remain derived and disposable.

## Shared authority

Terrain-capable bodies continue to derive appearance from the M16 PlanetaryAppearanceProduct.

Non-terrain bodies derive their reference shape from BodyRegistry and use ordinary semantic capabilities such as RadiativeEmitter to select stellar behavior.

No far representation becomes semantic body authority.

## Smooth globe

SmoothGlobe uses the authoritative sphere/ellipsoid reference shape without macro displacement.

The renderer ray-intersects the analytic ellipsoid directly.

For terrain-capable bodies the material baseline is an aggregate of M16 appearance channels:

- mean linear albedo;
- mean roughness;
- ocean fraction;
- ice fraction;
- mean emission.

The smooth globe therefore removes unresolved geometry while retaining the body's bulk appearance.

## Analytic disc impostor

AnalyticDiscImpostor is a camera-facing projected disc.

Its radius is driven directly by M14's projected body radius.

The fragment shader reconstructs a hemisphere normal from disc coordinates and applies compact far-view lighting.

No mesh, vertex buffer or texture is required.

This is the default disc path for bodies without complex derived appearance.

## Cached disc impostor

CachedDiscImpostor is a real revisioned image product.

BuildCachedDisc projects the M16 six-face appearance field into a canonical 2D lit disc and produces an RGBA8_UNorm image.

The disc fingerprint includes:

- M16 appearance fingerprint;
- disc resolution.

The GPU product owns:

- host-visible staging buffer;
- sampled RGBA8 texture;
- one-time upload state.

The sampled texture is uploaded before any render-target binding in a frame that first needs it.

The current baseline uses a canonical body-forward cached view. View-dependent disc atlasing and cache budgeting belong to later performance/quality work rather than introducing hidden authoring state here.

## Point proxy

PointProxy uses a camera-facing analytic raster proxy when the projected body radius falls below the disc threshold.

The raster footprint has a minimum 0.5-pixel radius so the source remains representable by ordinary rasterization.

When the true projected radius is smaller than that floor, emitted intensity is multiplied by:

```
(projectedRadius / rasterRadius)^2
```

This preserves projected-area brightness continuity instead of making a sub-pixel body artificially brighter because its raster footprint was clamped.

## Stellar point proxy

StellarPointProxy uses the same projected-size and sub-pixel-area contract but selects the stellar visual branch.

In Studio, a body is treated as radiative only when an enabled RadiativeEmitter capability is present.

Disabled emitters do not select the stellar proxy.

M18 only preserves representation continuity. Physical luminosity, exposure and stellar radiometry are M19 responsibilities.

## Full-ladder overlap

M14 now exposes ResolveRepresentationBlend.

It produces adjacent representation pairs and smooth overlap weights for:

- production surface -> macro globe;
- macro globe -> smooth globe;
- smooth globe -> disc impostor;
- disc impostor -> point proxy.

The overlap band remains centered on the same M14 threshold and uses the configured hysteresis fraction.

The returned conceptual weights sum to one.

Rendering uses standard alpha-over semantics:

1. render the richer representation as the base;
2. render the lower representation using lowerWeight as overlay alpha.

This yields the intended richer*(1-t) + lower*t crossfade without requiring a separate compositor.

## Studio integration

Terrain-capable Perspective views now progress through the complete M14 ladder.

The M15/M16 cache is extended with:

- M18 AppearanceSummary;
- revisioned GpuCachedDiscProduct.

Macro globe, smooth globe, cached disc and point representations therefore share one terrain/appearance revision boundary.

Perspective views for bodies without terrain also use the M14 far ladder:

- SmoothGlobe;
- AnalyticDiscImpostor;
- PointProxy;
- StellarPointProxy when an enabled RadiativeEmitter capability exists.

BodyMap intentionally retains its explicit full-body preview behavior.

## Shader/resource model

FarBodyRenderer uses zero-vertex-buffer full-screen quad draws via SV_VertexID.

It owns two ordinary RHI pipelines:

- analytic pipeline for smooth globe, analytic disc, point and stellar point;
- sampled-texture pipeline for cached disc.

Both use standard alpha blending.

## Diagnostics

Existing Studio transition diagnostics continue to report:

- selected M14 representation;
- lower-fidelity neighbor;
- projected radius;
- overlap state;
- hysteresis state.

For terrain-capable bodies, the shared macro-globe diagnostics continue to expose appearance/geometry fingerprints used by the M18 cache.

## Validation

Deterministic coverage includes:

- M16 appearance summary derivation;
- cached-disc dimensions and alpha;
- appearance-driven cached-disc fingerprint invalidation;
- macro -> smooth 50/50 boundary;
- smooth -> cached-disc boundary;
- cached-disc -> point boundary;
- adjacent-weight conservation;
- existing M14/M17 hysteresis and fallback coverage.

## Intentional limits

M18 does not yet provide:

- physical stellar luminosity;
- HDR scene exposure;
- radiometric reflected light;
- eclipses/transits;
- view-dependent cached-disc atlas scheduling;
- far-product memory budgets.

Those belong to M19, M20 and M31.

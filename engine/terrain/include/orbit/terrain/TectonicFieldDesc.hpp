#pragma once

#include <orbit/core/Types.hpp>

namespace orbit::terrain
{
inline constexpr u32 kMaxTectonicPlates = 24;
inline constexpr u32 kMaxTectonicHotspots = 8;
inline constexpr u32 kMaxTectonicHotspotAgeSteps = 6;

// Analytic, deterministic "plate tectonics" layer: a fixed set of rigid
// spherical plates (each with its own rotation) drives where mountain
// ranges/continents cohere into linear chains instead of scattered noise,
// plus a handful of mantle-frame volcanic hotspots that leave aging island
// chains as their overlying plate drifts past. There is no simulated time
// axis -- everything here is evaluated analytically from direction alone,
// so it stays a pure, allocation-free function safe to call concurrently
// from the terrain streaming workers.
struct TectonicFieldDesc
{
    // Zero means derive from the owning GlobalTerrainFieldDesc's seed.
    u64 seed{0};

    u32 plateCount{14};
    f64 plateIrregularity{0.35};
    // Dot-product amplitude of each plate's random claim bias -- makes a
    // plain nearest-seed Voronoi diagram (which, for evenly spread seeds,
    // gives every plate close to the same size) into a
    // multiplicatively-weighted one, so plate sizes vary the way real
    // Earth's do (one Pacific-sized plate, several tiny ones) instead of
    // an evenly-spaced honeycomb. Safer than raising plateIrregularity
    // for this: that perturbs seed *positions*, and far enough it can
    // land two seeds close enough together to degenerate into one
    // abnormally huge, mostly-boundaryless cell.
    f64 plateSizeVarianceDot{0.12};
    f64 continentalPlateFraction{0.4};
    f64 continentalPlateBiasMeters{1'800.0};
    f64 oceanicPlateBiasMeters{-2'600.0};
    // How strongly plate identity (vs. pure noise) shapes continent/ocean
    // placement. 0 reproduces the old noise-only coastlines exactly.
    f64 tectonicContinentInfluence{0.7};
    // Dot-product width of the blend zone between two plates -- smaller is
    // a sharper boundary. This also sets the width of every convergence-
    // driven mountain range (GlobalTerrainFields::mountainElevation), so
    // it's load-bearing for real terrain, not just map styling: dropping
    // it as low as 0.18 already thins ranges enough that
    // TerrainMountainTests' deterministic 65k-point equal-area survey
    // stops reliably landing inside any range's now-narrower convergent
    // band and reports no near-8km peak. Left at its original value --
    // the Tectonics *map layer*'s line thickness is tuned independently,
    // downstream, via PlanetMapRenderer's own classification threshold,
    // which doesn't touch actual terrain and so can't break this.
    f64 boundaryWidthDot{0.25};
    f64 minPlateAngularSpeed{0.15};
    f64 maxPlateAngularSpeed{1.0};
    // Raised from an original 0.28: at 0.28, nearly every plate pair's
    // relative speed already saturates Smooth(convergence/this) to 1.0,
    // so boundaries all read as equally "fully active" regardless of how
    // fast the two plates actually move apart -- no dim/weak boundaries
    // next to bold/strong ones. The true per-seed peak convergence is
    // still well above this (measured: the tallest mountain barely
    // moves), so this mostly varies the weaker majority of boundaries.
    f64 convergenceReferenceSpeed{0.4};
    // Reference speed transform (lateral shear) motion is normalized
    // against, separate from convergenceReferenceSpeed -- higher than it
    // so incidental shear near a convergent/divergent point doesn't win
    // the boundary-type classification just because it saturates faster.
    f64 transformReferenceSpeed{0.7};
    // Relief scale for an oceanic-oceanic collision relative to any
    // collision involving a continental plate (island arcs are lower than
    // continental collision ranges like the Himalaya).
    f64 oceanicConvergenceScale{0.85};
    // Cheap analytic uplift bump used only by the rain-shadow probe's
    // coarse elevation estimate (no ridge noise).
    f64 convergenceUpliftMeters{2'200.0};

    u32 hotspotCount{5};
    u32 hotspotAgeSteps{4};
    f64 hotspotBaseReliefMeters{6'000.0};
    f64 hotspotAgeDecay{0.55};
    f64 hotspotChainSpacingMeters{180'000.0};
    f64 hotspotCoreRadiusMeters{45'000.0};
    f64 hotspotRadiusGrowthPerAge{0.4};

    f64 rainShadowStrength{0.75};
    u32 rainShadowSteps{3};
    f64 rainShadowStepMeters{80'000.0};
    f64 rainShadowStepGrowth{3.0};
    f64 rainShadowThresholdMeters{600.0};
    f64 rainShadowRangeMeters{2'200.0};
    f64 windBandTransitionDegrees{6.0};
};
} // namespace orbit::terrain

#pragma once

#include <orbit/core/Types.hpp>
#include <orbit/math/Vector.hpp>

#include <vector>

namespace orbit::celestial_lighting
{
struct ApparentDisc
{
    math::Double3 centerFromObserverMeters{};
    f64 radiusMeters{1.0};
};

struct OccultationResult
{
    f64 sourceAngularRadiusRadians{0.0};
    f64 occluderAngularRadiusRadians{0.0};
    f64 angularSeparationRadians{0.0};
    f64 obscuredFraction{0.0};
    f64 visibleFraction{1.0};
    bool occluderInFront{false};
    bool overlapping{false};
    bool total{false};
    bool annular{false};
};

[[nodiscard]] OccultationResult FiniteDiscOccultation(
    const ApparentDisc& source,
    const ApparentDisc& occluder);

struct MultiOccluderResult
{
    f64 visibleFraction{1.0};
    f64 obscuredFraction{0.0};
    u32 contributingOccluders{0};
};

// Conservative independent-disc composition. Exact pairwise source-disc
// overlaps are evaluated; combined visibility is multiplicative so overlapping
// occluder shadows are never double-subtracted. M20 keeps this deterministic
// and bounded; a later arbitrary-disc-union solver can replace only this seam.
[[nodiscard]] MultiOccluderResult CombinedOccultation(
    const ApparentDisc& source,
    const std::vector<ApparentDisc>& occluders);

struct DirectIrradianceResult
{
    f64 unoccludedWattsPerSquareMeter{0.0};
    f64 visibleFraction{1.0};
    f64 irradianceWattsPerSquareMeter{0.0};
};

[[nodiscard]] DirectIrradianceResult AttenuatedDirectIrradiance(
    f64 luminosityWatts,
    f64 sourceDistanceMeters,
    f64 visibleFraction);

[[nodiscard]] f64 LambertPhase(
    f64 phaseAngleRadians);

struct ReflectedLightResult
{
    f64 phaseAngleRadians{0.0};
    f64 phaseFunction{0.0};
    f64 incidentIrradianceWattsPerSquareMeter{0.0};
    // Multiply by geometric albedo to obtain the observer irradiance.
    f64 unitGeometricAlbedoIrradianceWattsPerSquareMeter{0.0};
};

[[nodiscard]] ReflectedLightResult LambertSphereReflectedLight(
    f64 incidentIrradianceWattsPerSquareMeter,
    f64 bodyRadiusMeters,
    f64 observerDistanceMeters,
    f64 phaseAngleRadians);
} // namespace orbit::celestial_lighting

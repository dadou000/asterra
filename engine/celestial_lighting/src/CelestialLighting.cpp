#include <orbit/celestial_lighting/CelestialLighting.hpp>

#include <orbit/celestial_radiometry/Radiometry.hpp>

#include <algorithm>
#include <cmath>
#include <numbers>
#include <stdexcept>

namespace orbit::celestial_lighting
{
namespace
{
void RequireFinitePositive(
    const f64 value,
    const char* message)
{
    if (!std::isfinite(value) ||
        value <= 0.0)
    {
        throw std::invalid_argument(message);
    }
}

[[nodiscard]] f64 AngularRadius(
    const ApparentDisc& disc)
{
    RequireFinitePositive(
        disc.radiusMeters,
        "Apparent-disc radius must be finite and positive.");

    const f64 distance =
        math::Length(
            disc.centerFromObserverMeters);

    RequireFinitePositive(
        distance,
        "Apparent-disc distance must be finite and positive.");

    const f64 ratio =
        std::clamp(
            disc.radiusMeters / distance,
            0.0,
            1.0);

    return std::asin(ratio);
}

[[nodiscard]] f64 CircleOverlapArea(
    const f64 r1,
    const f64 r2,
    const f64 separation)
{
    if (separation >= r1 + r2)
    {
        return 0.0;
    }

    if (separation <=
        std::abs(r1 - r2))
    {
        const f64 smaller =
            std::min(r1, r2);
        return
            std::numbers::pi_v<f64> *
            smaller *
            smaller;
    }

    const f64 d2 =
        separation * separation;
    const f64 r12 =
        r1 * r1;
    const f64 r22 =
        r2 * r2;

    const f64 c1 =
        std::clamp(
            (d2 + r12 - r22) /
                (2.0 * separation * r1),
            -1.0,
            1.0);

    const f64 c2 =
        std::clamp(
            (d2 + r22 - r12) /
                (2.0 * separation * r2),
            -1.0,
            1.0);

    const f64 triangle =
        0.5 *
        std::sqrt(
            std::max(
                (-separation + r1 + r2) *
                ( separation + r1 - r2) *
                ( separation - r1 + r2) *
                ( separation + r1 + r2),
                0.0));

    return
        r12 * std::acos(c1) +
        r22 * std::acos(c2) -
        triangle;
}
} // namespace

OccultationResult FiniteDiscOccultation(
    const ApparentDisc& source,
    const ApparentDisc& occluder)
{
    const f64 sourceDistance =
        math::Length(
            source.centerFromObserverMeters);
    const f64 occluderDistance =
        math::Length(
            occluder.centerFromObserverMeters);

    RequireFinitePositive(
        sourceDistance,
        "Source distance must be finite and positive.");
    RequireFinitePositive(
        occluderDistance,
        "Occluder distance must be finite and positive.");

    const f64 sourceAngular =
        AngularRadius(source);
    const f64 occluderAngular =
        AngularRadius(occluder);

    const math::Double3 sourceDirection =
        source.centerFromObserverMeters /
        sourceDistance;
    const math::Double3 occluderDirection =
        occluder.centerFromObserverMeters /
        occluderDistance;

    const f64 separation =
        std::acos(
            std::clamp(
                math::Dot(
                    sourceDirection,
                    occluderDirection),
                -1.0,
                1.0));

    const bool inFront =
        occluderDistance <
        sourceDistance;

    OccultationResult result{
        .sourceAngularRadiusRadians =
            sourceAngular,
        .occluderAngularRadiusRadians =
            occluderAngular,
        .angularSeparationRadians =
            separation,
        .occluderInFront =
            inFront
    };

    if (!inFront)
    {
        return result;
    }

    const f64 overlap =
        CircleOverlapArea(
            sourceAngular,
            occluderAngular,
            separation);

    const f64 sourceArea =
        std::numbers::pi_v<f64> *
        sourceAngular *
        sourceAngular;

    result.obscuredFraction =
        sourceArea > 0.0
            ? std::clamp(
                  overlap / sourceArea,
                  0.0,
                  1.0)
            : 0.0;

    result.visibleFraction =
        1.0 -
        result.obscuredFraction;

    result.overlapping =
        result.obscuredFraction >
        0.0;

    result.total =
        result.obscuredFraction >=
        1.0 - 1.0e-12;

    result.annular =
        result.overlapping &&
        !result.total &&
        separation +
            occluderAngular <=
            sourceAngular;

    return result;
}

MultiOccluderResult CombinedOccultation(
    const ApparentDisc& source,
    const std::vector<ApparentDisc>& occluders)
{
    f64 visible = 1.0;
    u32 contributing = 0U;

    for (const auto& occluder :
         occluders)
    {
        const auto result =
            FiniteDiscOccultation(
                source,
                occluder);

        if (!result.overlapping)
        {
            continue;
        }

        visible *=
            result.visibleFraction;
        ++contributing;
    }

    visible =
        std::clamp(
            visible,
            0.0,
            1.0);

    return {
        .visibleFraction = visible,
        .obscuredFraction =
            1.0 - visible,
        .contributingOccluders =
            contributing
    };
}

DirectIrradianceResult AttenuatedDirectIrradiance(
    const f64 luminosityWatts,
    const f64 sourceDistanceMeters,
    const f64 visibleFraction)
{
    if (!std::isfinite(visibleFraction) ||
        visibleFraction < 0.0 ||
        visibleFraction > 1.0)
    {
        throw std::invalid_argument(
            "Visible fraction must be finite and in [0,1].");
    }

    const f64 unoccluded =
        celestial_radiometry::
            IrradianceWattsPerSquareMeter(
                luminosityWatts,
                sourceDistanceMeters);

    return {
        .unoccludedWattsPerSquareMeter =
            unoccluded,
        .visibleFraction =
            visibleFraction,
        .irradianceWattsPerSquareMeter =
            unoccluded *
            visibleFraction
    };
}

f64 LambertPhase(
    const f64 phaseAngleRadians)
{
    if (!std::isfinite(phaseAngleRadians) ||
        phaseAngleRadians < 0.0 ||
        phaseAngleRadians >
            std::numbers::pi_v<f64>)
    {
        throw std::invalid_argument(
            "Lambert phase angle must be finite and in [0,pi].");
    }

    return
        (std::sin(phaseAngleRadians) +
         (std::numbers::pi_v<f64> -
          phaseAngleRadians) *
             std::cos(phaseAngleRadians)) /
        std::numbers::pi_v<f64>;
}

ReflectedLightResult LambertSphereReflectedLight(
    const f64 incidentIrradianceWattsPerSquareMeter,
    const f64 bodyRadiusMeters,
    const f64 observerDistanceMeters,
    const f64 phaseAngleRadians)
{
    if (!std::isfinite(
            incidentIrradianceWattsPerSquareMeter) ||
        incidentIrradianceWattsPerSquareMeter <
            0.0)
    {
        throw std::invalid_argument(
            "Incident irradiance must be finite and non-negative.");
    }

    RequireFinitePositive(
        bodyRadiusMeters,
        "Reflecting body radius must be finite and positive.");
    RequireFinitePositive(
        observerDistanceMeters,
        "Observer distance must be finite and positive.");

    const f64 phase =
        LambertPhase(
            phaseAngleRadians);

    const f64 radiusRatio =
        bodyRadiusMeters /
        observerDistanceMeters;

    return {
        .phaseAngleRadians =
            phaseAngleRadians,
        .phaseFunction =
            phase,
        .incidentIrradianceWattsPerSquareMeter =
            incidentIrradianceWattsPerSquareMeter,
        .unitGeometricAlbedoIrradianceWattsPerSquareMeter =
            incidentIrradianceWattsPerSquareMeter *
            radiusRatio *
            radiusRatio *
            phase
    };
}
} // namespace orbit::celestial_lighting

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
    if (occluders.empty())
    {
        return {};
    }

    if (occluders.size() == 1U)
    {
        const auto single =
            FiniteDiscOccultation(
                source,
                occluders.front());

        return {
            .visibleFraction =
                single.visibleFraction,
            .obscuredFraction =
                single.obscuredFraction,
            .contributingOccluders =
                single.overlapping
                    ? 1U
                    : 0U
        };
    }

    const f64 sourceDistance =
        math::Length(
            source.centerFromObserverMeters);
    const f64 sourceAngular =
        AngularRadius(source);
    const math::Double3 sourceDirection =
        source.centerFromObserverMeters /
        sourceDistance;

    const math::Double3 reference =
        std::abs(sourceDirection.y) <
                0.9
            ? math::Double3{
                  0.0, 1.0, 0.0}
            : math::Double3{
                  1.0, 0.0, 0.0};

    const math::Double3 tangentA =
        math::Normalize(
            math::Cross(
                reference,
                sourceDirection));
    const math::Double3 tangentB =
        math::Normalize(
            math::Cross(
                sourceDirection,
                tangentA));

    struct PreparedOccluder
    {
        math::Double3 direction{};
        f64 angularRadius{0.0};
        bool inFront{false};
        bool contributes{false};
    };

    std::vector<PreparedOccluder>
        prepared;
    prepared.reserve(
        occluders.size());

    u32 contributing = 0U;

    for (const auto& occluder :
         occluders)
    {
        const f64 distance =
            math::Length(
                occluder.
                    centerFromObserverMeters);

        const auto exact =
            FiniteDiscOccultation(
                source,
                occluder);

        PreparedOccluder item{
            .direction =
                occluder.
                    centerFromObserverMeters /
                distance,
            .angularRadius =
                exact.
                    occluderAngularRadiusRadians,
            .inFront =
                exact.occluderInFront,
            .contributes =
                exact.overlapping
        };

        if (item.contributes)
        {
            ++contributing;
        }

        prepared.push_back(
            item);
    }

    if (contributing == 0U)
    {
        return {};
    }

    // Deterministic equal-area sampling of the source disc. This evaluates
    // the union of all projected occluder discs, so overlapping occluders
    // are not double-counted. The single-occluder path above remains fully
    // analytic.
    constexpr u32 kSamples = 8192U;
    constexpr f64 kGoldenAngle =
        2.39996322972865332223;

    u32 covered = 0U;

    for (u32 sampleIndex = 0U;
         sampleIndex < kSamples;
         ++sampleIndex)
    {
        const f64 radialFraction =
            std::sqrt(
                (static_cast<f64>(
                     sampleIndex) +
                 0.5) /
                static_cast<f64>(
                    kSamples));

        const f64 offsetAngle =
            sourceAngular *
            radialFraction;
        const f64 azimuth =
            kGoldenAngle *
            static_cast<f64>(
                sampleIndex);

        const math::Double3 tangent =
            tangentA *
                std::cos(azimuth) +
            tangentB *
                std::sin(azimuth);

        const math::Double3 sampleDirection =
            sourceDirection *
                std::cos(offsetAngle) +
            tangent *
                std::sin(offsetAngle);

        bool occulted = false;

        for (const auto& occluder :
             prepared)
        {
            if (!occluder.inFront ||
                !occluder.contributes)
            {
                continue;
            }

            const f64 separation =
                std::acos(
                    std::clamp(
                        math::Dot(
                            sampleDirection,
                            occluder.direction),
                        -1.0,
                        1.0));

            if (separation <=
                occluder.angularRadius)
            {
                occulted = true;
                break;
            }
        }

        if (occulted)
        {
            ++covered;
        }
    }

    const f64 obscured =
        static_cast<f64>(covered) /
        static_cast<f64>(kSamples);

    return {
        .visibleFraction =
            1.0 - obscured,
        .obscuredFraction =
            obscured,
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

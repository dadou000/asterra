#include <orbit/lighting/TerrainHeightfieldVisibility.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>

namespace orbit::lighting
{
namespace
{
[[nodiscard]] math::Double3 ToDouble3(
    const math::Float3 value) noexcept
{
    return {
        static_cast<f64>(value.x),
        static_cast<f64>(value.y),
        static_cast<f64>(value.z)
    };
}

[[nodiscard]] math::Float3 ToFloat3Normalized(
    const math::Double3 value) noexcept
{
    const auto normalized =
        math::Normalize(value);

    return {
        static_cast<f32>(normalized.x),
        static_cast<f32>(normalized.y),
        static_cast<f32>(normalized.z)
    };
}

struct RayInterval
{
    f64 minimum{0.0};
    f64 maximum{0.0};
    bool valid{false};
};

[[nodiscard]] RayInterval IntersectSphereInterval(
    const math::Double3& origin,
    const math::Double3& direction,
    const f64 radius,
    const f64 requestedMinimum,
    const f64 requestedMaximum) noexcept
{
    const f64 b =
        math::Dot(
            origin,
            direction);

    const f64 c =
        math::Dot(origin, origin) -
        radius * radius;

    const f64 discriminant =
        b * b - c;

    if (!std::isfinite(discriminant) ||
        discriminant < 0.0)
    {
        return {};
    }

    const f64 root =
        std::sqrt(
            std::max(
                discriminant,
                0.0));

    const f64 entry =
        -b - root;
    const f64 exit =
        -b + root;

    const f64 minimum =
        std::max(
            requestedMinimum,
            entry);
    const f64 maximum =
        std::min(
            requestedMaximum,
            exit);

    return {
        .minimum = minimum,
        .maximum = maximum,
        .valid =
            std::isfinite(minimum) &&
            std::isfinite(maximum) &&
            maximum >= minimum
    };
}
} // namespace

TerrainHeightfieldVisibilityProvider::
TerrainHeightfieldVisibilityProvider(
    const universe::BodyId body,
    const world::PlanetDefinition planet,
    const terrain::TerrainSource& source,
    const universe::BodyRegistry& bodies,
    const frames::FrameGraph& frames,
    TerrainVisibilityConfig config,
    const time::SimulationTime atTime)
    : body_(body),
      planet_(planet),
      source_(&source),
      bodies_(&bodies),
      frames_(&frames),
      config_(config),
      atTime_(atTime),
      desc_({
          .providerId =
              0x5445525241494e48ULL,
          .name =
              "Terrain Heightfield",
          .kind =
              VisibilityBackendKind::
                  TerrainHeightfield,
          .capabilities =
              VisibilityCapability::Offscreen |
              VisibilityCapability::PlanetaryRange,
          .nominalErrorMeters =
              static_cast<f32>(
                  std::max(
                      config.minimumStepMeters,
                      0.0)),
          .priority = 40
      })
{
    config_.maximumAbsoluteElevationMeters =
        std::max(
            config_.maximumAbsoluteElevationMeters,
            0.0);

    config_.minimumStepMeters =
        std::max(
            config_.minimumStepMeters,
            0.01);

    config_.maximumStepMeters =
        std::max(
            config_.maximumStepMeters,
            config_.minimumStepMeters);

    config_.normalSampleSpacingMeters =
        std::max(
            config_.normalSampleSpacingMeters,
            0.01);

    config_.maximumMarchSteps =
        std::max(
            config_.maximumMarchSteps,
            1U);
}

void TerrainHeightfieldVisibilityProvider::SetTime(
    const time::SimulationTime atTime) noexcept
{
    atTime_ = atTime;
}

const VisibilityProviderDesc&
TerrainHeightfieldVisibilityProvider::
Description() const noexcept
{
    return desc_;
}

bool TerrainHeightfieldVisibilityProvider::
SupportsPurpose(
    const VisibilityPurpose purpose) const noexcept
{
    switch (purpose)
    {
    case VisibilityPurpose::DiffuseGi:
    case VisibilityPurpose::Reflection:
    case VisibilityPurpose::Shadow:
    case VisibilityPurpose::SkyVisibility:
    case VisibilityPurpose::ProbeUpdate:
    case VisibilityPurpose::Diagnostic:
        return true;
    }

    return false;
}

f64 TerrainHeightfieldVisibilityProvider::
SurfaceRadiusMeters(
    const math::Double3& bodyPoint,
    const f64 footprintMeters) const noexcept
{
    if (source_ == nullptr)
    {
        return planet_.radiusMeters;
    }

    const f64 length =
        math::Length(bodyPoint);

    if (!std::isfinite(length) ||
        length <= 1.0e-12)
    {
        return planet_.radiusMeters;
    }

    const auto direction =
        bodyPoint / length;

    const auto sample =
        source_->Sample({
            .unitDirection =
                direction,
            .footprintMeters =
                std::max(
                    footprintMeters,
                    0.01),
            .planet =
                planet_.id,
            .radialOffsetMeters =
                0.0
        });

    const f64 visibleElevation =
        sample.elevationMeters +
        sample.standingWaterDepthMeters;

    return
        planet_.radiusMeters +
        (std::isfinite(visibleElevation)
             ? visibleElevation
             : 0.0);
}

f64 TerrainHeightfieldVisibilityProvider::
SignedHeightDistanceMeters(
    const math::Double3& bodyPoint,
    const f64 footprintMeters) const noexcept
{
    const f64 radius =
        math::Length(bodyPoint);

    if (!std::isfinite(radius))
    {
        return
            std::numeric_limits<f64>::
                infinity();
    }

    return
        radius -
        SurfaceRadiusMeters(
            bodyPoint,
            footprintMeters);
}

math::Float3
TerrainHeightfieldVisibilityProvider::
SurfaceNormalInQueryFrame(
    const math::Double3& bodyPoint,
    const math::RigidTransformD& queryFromBody) const noexcept
{
    const f64 radius =
        math::Length(bodyPoint);

    if (!std::isfinite(radius) ||
        radius <= 1.0e-12)
    {
        return {0.0F, 1.0F, 0.0F};
    }

    const math::Double3 up =
        bodyPoint / radius;

    const auto frame =
        world::MakeSurfaceFrame(up);

    const f64 spacing =
        config_.normalSampleSpacingMeters;

    const auto samplePoint =
        [this, &frame, spacing](
            const math::Double2 offset)
        {
            const math::Double3 direction =
                world::DirectionAtSurfaceOffset(
                    planet_,
                    frame,
                    offset);

            const f64 surfaceRadius =
                SurfaceRadiusMeters(
                    direction *
                        planet_.radiusMeters,
                    spacing);

            return
                direction *
                surfaceRadius;
        };

    const math::Double3 west =
        samplePoint(
            {-spacing, 0.0});
    const math::Double3 east =
        samplePoint(
            { spacing, 0.0});
    const math::Double3 south =
        samplePoint(
            {0.0, -spacing});
    const math::Double3 north =
        samplePoint(
            {0.0,  spacing});

    math::Double3 normalBody =
        math::Normalize(
            math::Cross(
                east - west,
                north - south));

    if (math::Dot(
            normalBody,
            up) < 0.0)
    {
        normalBody =
            normalBody * -1.0;
    }

    const auto normalQuery =
        math::TransformVector(
            queryFromBody.rotation,
            normalBody);

    return
        ToFloat3Normalized(
            normalQuery);
}

VisibilityResult
TerrainHeightfieldVisibilityProvider::Trace(
    const VisibilityQuery& query)
{
    if (source_ == nullptr ||
        bodies_ == nullptr ||
        frames_ == nullptr ||
        query.body != body_)
    {
        return {};
    }

    const auto* body =
        bodies_->FindBody(body_);

    if (body == nullptr)
    {
        return {};
    }

    const auto bodyFromQuery =
        frames_->ResolveTransform(
            query.frame,
            body->frame,
            atTime_);

    if (!bodyFromQuery.has_value())
    {
        return {};
    }

    const auto queryFromBody =
        math::Inverse(
            *bodyFromQuery);

    const math::Double3 origin =
        math::TransformPoint(
            *bodyFromQuery,
            query.originInFrameMeters);

    const math::Double3 direction =
        math::Normalize(
            math::TransformVector(
                bodyFromQuery->rotation,
                ToDouble3(
                    query.direction)));

    if (math::LengthSquared(direction) <=
        1.0e-18)
    {
        return {};
    }

    const f64 minimumDistance =
        static_cast<f64>(
            query.minimumDistanceMeters);

    const f64 maximumDistance =
        static_cast<f64>(
            query.maximumDistanceMeters);

    const f64 outerRadius =
        planet_.radiusMeters +
        config_.
            maximumAbsoluteElevationMeters;

    const RayInterval interval =
        IntersectSphereInterval(
            origin,
            direction,
            outerRadius,
            minimumDistance,
            maximumDistance);

    if (!interval.valid)
    {
        return {
            .resolution =
                VisibilityResolution::Miss,
            .confidence = 1.0F,
            .terminal = false
        };
    }

    f64 previousDistance =
        interval.minimum;

    math::Double3 previousPoint =
        origin +
        direction *
            previousDistance;

    f64 previousSigned =
        SignedHeightDistanceMeters(
            previousPoint,
            config_.maximumStepMeters);

    if (previousSigned <= 0.0)
    {
        const auto hitQuery =
            math::TransformPoint(
                queryFromBody,
                previousPoint);

        const auto normal =
            SurfaceNormalInQueryFrame(
                previousPoint,
                queryFromBody);

        return {
            .resolution =
                VisibilityResolution::Hit,
            .hit = {
                .distanceMeters =
                    static_cast<f32>(
                        previousDistance),
                .positionInFrameMeters =
                    hitQuery,
                .geometricNormal =
                    normal,
                .shadingNormal =
                    normal
            },
            .confidence = 1.0F,
            .terminal = true
        };
    }

    f64 currentDistance =
        previousDistance;

    for (u32 stepIndex = 0U;
         stepIndex <
             config_.maximumMarchSteps &&
         currentDistance <
             interval.maximum;
         ++stepIndex)
    {
        const f64 adaptiveStep =
            std::clamp(
                std::abs(previousSigned) *
                    0.5,
                config_.minimumStepMeters,
                config_.maximumStepMeters);

        currentDistance =
            std::min(
                interval.maximum,
                previousDistance +
                    adaptiveStep);

        const math::Double3 currentPoint =
            origin +
            direction *
                currentDistance;

        const f64 currentSigned =
            SignedHeightDistanceMeters(
                currentPoint,
                adaptiveStep);

        if (currentSigned <= 0.0)
        {
            f64 lower =
                previousDistance;
            f64 upper =
                currentDistance;

            for (u32 refine = 0U;
                 refine <
                     config_.
                         rootRefinementIterations;
                 ++refine)
            {
                const f64 middle =
                    (lower + upper) *
                    0.5;

                const math::Double3
                    middlePoint =
                        origin +
                        direction *
                            middle;

                const f64 middleSigned =
                    SignedHeightDistanceMeters(
                        middlePoint,
                        std::max(
                            config_.
                                minimumStepMeters,
                            upper - lower));

                if (middleSigned > 0.0)
                {
                    lower = middle;
                }
                else
                {
                    upper = middle;
                }
            }

            const f64 hitDistance =
                upper;

            const math::Double3 hitBody =
                origin +
                direction *
                    hitDistance;

            const auto hitQuery =
                math::TransformPoint(
                    queryFromBody,
                    hitBody);

            const auto normal =
                SurfaceNormalInQueryFrame(
                    hitBody,
                    queryFromBody);

            const f32 confidence =
                static_cast<f32>(
                    std::clamp(
                        1.0 -
                            (upper - lower) /
                                std::max(
                                    config_.
                                        minimumStepMeters,
                                    1.0e-6),
                        0.0,
                        1.0));

            return {
                .resolution =
                    VisibilityResolution::Hit,
                .hit = {
                    .distanceMeters =
                        static_cast<f32>(
                            hitDistance),
                    .positionInFrameMeters =
                        hitQuery,
                    .geometricNormal =
                        normal,
                    .shadingNormal =
                        normal
                },
                .confidence =
                    std::max(
                        confidence,
                        0.95F),
                .terminal = true
            };
        }

        if (currentDistance >=
            interval.maximum)
        {
            break;
        }

        previousDistance =
            currentDistance;
        previousSigned =
            currentSigned;
    }

    return {
        .resolution =
            VisibilityResolution::Miss,
        .confidence = 1.0F,
        .terminal = false
    };
}
} // namespace orbit::lighting

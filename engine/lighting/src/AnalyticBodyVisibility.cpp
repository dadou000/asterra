#include <orbit/lighting/AnalyticBodyVisibility.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <type_traits>
#include <variant>

namespace orbit::lighting
{
namespace
{
[[nodiscard]] universe::EllipsoidShape AsEllipsoid(
    const universe::BodyShape& shape) noexcept
{
    return std::visit(
        [](const auto& value)
        {
            using Shape =
                std::decay_t<decltype(value)>;

            if constexpr (
                std::is_same_v<
                    Shape,
                    universe::SphereShape>)
            {
                return universe::EllipsoidShape{
                    .radiiMeters = {
                        value.radiusMeters,
                        value.radiusMeters,
                        value.radiusMeters
                    }
                };
            }
            else
            {
                return value;
            }
        },
        shape);
}

[[nodiscard]] math::Double3 ToDouble3(
    const math::Float3 value) noexcept
{
    return {
        static_cast<f64>(value.x),
        static_cast<f64>(value.y),
        static_cast<f64>(value.z)
    };
}

[[nodiscard]] math::Float3 ToFloatNormal(
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
} // namespace

AnalyticBodyVisibilityProvider::
AnalyticBodyVisibilityProvider(
    const universe::BodyRegistry& bodies,
    const frames::FrameGraph& frames,
    const time::SimulationTime atTime)
    : bodies_(&bodies),
      frames_(&frames),
      atTime_(atTime),
      desc_({
          .providerId =
              0x414e414c59544943ULL,
          .name =
              "Analytic Body",
          .kind =
              VisibilityBackendKind::Analytic,
          .capabilities =
              VisibilityCapability::Offscreen |
              VisibilityCapability::PlanetaryRange,
          .nominalErrorMeters = 0.0F,
          .priority = 20
      })
{
}

void AnalyticBodyVisibilityProvider::SetTime(
    const time::SimulationTime atTime) noexcept
{
    atTime_ = atTime;
}

const VisibilityProviderDesc&
AnalyticBodyVisibilityProvider::
Description() const noexcept
{
    return desc_;
}

bool AnalyticBodyVisibilityProvider::
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

VisibilityResult
AnalyticBodyVisibilityProvider::Trace(
    const VisibilityQuery& query)
{
    if (bodies_ == nullptr ||
        frames_ == nullptr ||
        !query.body ||
        !query.frame)
    {
        return {};
    }

    const auto* body =
        bodies_->FindBody(
            query.body);

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

    const math::Double3 originBody =
        math::TransformPoint(
            *bodyFromQuery,
            query.originInFrameMeters);

    const math::Double3 directionBodyRaw =
        math::TransformVector(
            bodyFromQuery->rotation,
            ToDouble3(
                query.direction));

    const math::Double3 directionBody =
        math::Normalize(
            directionBodyRaw);

    if (math::LengthSquared(directionBody) <=
        1.0e-18)
    {
        return {};
    }

    const auto ellipsoid =
        AsEllipsoid(
            body->shape);

    const math::Double3 radii{
        std::max(
            ellipsoid.radiiMeters.x,
            1.0e-6),
        std::max(
            ellipsoid.radiiMeters.y,
            1.0e-6),
        std::max(
            ellipsoid.radiiMeters.z,
            1.0e-6)
    };

    const math::Double3 ro{
        originBody.x / radii.x,
        originBody.y / radii.y,
        originBody.z / radii.z
    };

    const math::Double3 rd{
        directionBody.x / radii.x,
        directionBody.y / radii.y,
        directionBody.z / radii.z
    };

    const f64 a =
        math::Dot(rd, rd);
    const f64 b =
        2.0 *
        math::Dot(ro, rd);
    const f64 c =
        math::Dot(ro, ro) -
        1.0;

    const f64 discriminant =
        b * b -
        4.0 * a * c;

    if (!std::isfinite(discriminant) ||
        discriminant < 0.0 ||
        a <= 0.0)
    {
        return {
            .resolution =
                VisibilityResolution::Miss,
            .confidence = 1.0F,
            // The provider only proves a miss against this body's analytic
            // boundary, not against all scene geometry.
            .terminal = false
        };
    }

    const f64 root =
        std::sqrt(
            std::max(
                discriminant,
                0.0));

    const f64 inverseDenominator =
        0.5 / a;

    const std::array candidates{
        (-b - root) *
            inverseDenominator,
        (-b + root) *
            inverseDenominator
    };

    f64 distance =
        std::numeric_limits<f64>::infinity();

    for (const f64 candidate :
         candidates)
    {
        if (candidate >=
                static_cast<f64>(
                    query.
                        minimumDistanceMeters) &&
            candidate <=
                static_cast<f64>(
                    query.
                        maximumDistanceMeters))
        {
            distance =
                std::min(
                    distance,
                    candidate);
        }
    }

    if (!std::isfinite(distance))
    {
        return {
            .resolution =
                VisibilityResolution::Miss,
            .confidence = 1.0F,
            .terminal = false
        };
    }

    const math::Double3 hitBody =
        originBody +
        directionBody *
            distance;

    const math::Double3 normalBody =
        math::Normalize({
            hitBody.x /
                (radii.x * radii.x),
            hitBody.y /
                (radii.y * radii.y),
            hitBody.z /
                (radii.z * radii.z)
        });

    const math::Double3 hitQuery =
        math::TransformPoint(
            queryFromBody,
            hitBody);

    const math::Double3 normalQuery =
        math::TransformVector(
            queryFromBody.rotation,
            normalBody);

    return {
        .resolution =
            VisibilityResolution::Hit,
        .hit = {
            .distanceMeters =
                static_cast<f32>(
                    distance),
            .positionInFrameMeters =
                hitQuery,
            .geometricNormal =
                ToFloatNormal(
                    normalQuery),
            .shadingNormal =
                ToFloatNormal(
                    normalQuery)
        },
        .confidence = 1.0F,
        .terminal = true
    };
}
} // namespace orbit::lighting

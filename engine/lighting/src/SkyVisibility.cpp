#include <orbit/lighting/SkyVisibility.hpp>

#include <algorithm>
#include <array>
#include <cmath>

namespace orbit::lighting
{
SkyVisibilityEstimate EstimateSkyVisibility(
    const VisibilityRegistry& visibility,
    const frames::FrameId frame,
    const universe::BodyId body,
    const math::Double3& originInFrameMeters,
    math::Float3 upDirection,
    const SkyVisibilitySettings& settings)
{
    const f32 upLengthSquared =
        math::LengthSquared(upDirection);

    if (!std::isfinite(upLengthSquared) ||
        upLengthSquared <= 1.0e-10F)
    {
        upDirection =
            {0.0F, 1.0F, 0.0F};
    }
    else
    {
        upDirection =
            math::Normalize(upDirection);
    }

    math::Float3 helper =
        std::abs(upDirection.y) < 0.95F
            ? math::Float3{0.0F, 1.0F, 0.0F}
            : math::Float3{1.0F, 0.0F, 0.0F};

    const math::Float3 tangent =
        math::Normalize(
            math::Cross(
                helper,
                upDirection));

    const math::Float3 bitangent =
        math::Normalize(
            math::Cross(
                upDirection,
                tangent));

    constexpr f32 kVertical = 0.72F;
    constexpr f32 kLateral =
        0.693974733F;

    struct Sample
    {
        math::Float3 direction{};
        f32 weight{0.0F};
    };

    const std::array<Sample, 5> samples{{
        {
            .direction = upDirection,
            .weight = 0.30F
        },
        {
            .direction =
                math::Normalize(
                    upDirection * kVertical +
                    tangent * kLateral),
            .weight = 0.175F
        },
        {
            .direction =
                math::Normalize(
                    upDirection * kVertical -
                    tangent * kLateral),
            .weight = 0.175F
        },
        {
            .direction =
                math::Normalize(
                    upDirection * kVertical +
                    bitangent * kLateral),
            .weight = 0.175F
        },
        {
            .direction =
                math::Normalize(
                    upDirection * kVertical -
                    bitangent * kLateral),
            .weight = 0.175F
        }
    }};

    const f32 minimumDistance =
        std::max(
            settings.minimumDistanceMeters,
            0.001F);

    const f32 maximumDistance =
        std::max(
            settings.maximumDistanceMeters,
            minimumDistance + 0.001F);

    f32 visibleWeight = 0.0F;
    math::Float3 visibleDirection{};

    for (const auto& sample : samples)
    {
        VisibilityQuery query{
            .purpose =
                VisibilityPurpose::SkyVisibility,
            .frame = frame,
            .body = body,
            .originInFrameMeters =
                originInFrameMeters,
            .direction =
                sample.direction,
            .minimumDistanceMeters =
                minimumDistance,
            .maximumDistanceMeters =
                maximumDistance,
            .importance =
                sample.weight,
            .requirements = {
                .requireOffscreenCoverage =
                    true
            }
        };

        const auto result =
            visibility.TraceNearest(query);

        // Sky visibility asks whether any known scene representation blocks
        // this direction. An unresolved/no-hit answer is intentionally kept
        // open so missing detail does not invent false occlusion.
        if (result.resolution ==
            VisibilityResolution::Hit)
        {
            continue;
        }

        visibleWeight +=
            sample.weight;

        visibleDirection =
            visibleDirection +
            sample.direction *
                sample.weight;
    }

    SkyVisibilityEstimate estimate;
    estimate.visibleFraction =
        std::clamp(
            visibleWeight,
            0.0F,
            1.0F);

    if (math::LengthSquared(
            visibleDirection) >
        1.0e-10F)
    {
        estimate.openDirection =
            math::Normalize(
                visibleDirection);
    }

    return estimate;
}
} // namespace orbit::lighting

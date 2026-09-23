#include <orbit/volume_representation/VolumeRepresentation.hpp>

#include <algorithm>
#include <bit>
#include <cmath>
#include <tuple>

namespace orbit::volume_representation
{
namespace
{
[[nodiscard]] f32 SmoothStep(
    const f64 a,
    const f64 b,
    const f64 x) noexcept
{
    if (!(b > a))
    {
        return x >= b ? 1.0F : 0.0F;
    }

    const f64 t =
        std::clamp(
            (x - a) / (b - a),
            0.0,
            1.0);

    return static_cast<f32>(
        t * t * (3.0 - 2.0 * t));
}

[[nodiscard]] u64 Mix(
    u64 seed,
    const u64 value) noexcept
{
    seed ^= value + 0x9e3779b97f4a7c15ULL +
        (seed << 6U) + (seed >> 2U);
    return seed;
}

[[nodiscard]] ResolvedRepresentation
ForcedRepresentation(
    const world_model::VolumeRepresentationMode mode,
    const bool bakedAvailable,
    bool& bakedFallback) noexcept
{
    bakedFallback = false;

    switch (mode)
    {
    case world_model::VolumeRepresentationMode::Live:
        return ResolvedRepresentation::Live;
    case world_model::VolumeRepresentationMode::Coarse:
        return ResolvedRepresentation::Coarse;
    case world_model::VolumeRepresentationMode::Passive:
        return ResolvedRepresentation::Passive;
    case world_model::VolumeRepresentationMode::Baked:
        if (bakedAvailable)
        {
            return ResolvedRepresentation::Baked;
        }
        bakedFallback = true;
        return ResolvedRepresentation::Passive;
    case world_model::VolumeRepresentationMode::Auto:
        break;
    }

    return ResolvedRepresentation::Live;
}
} // namespace

RepresentationDecision ResolveRepresentation(
    const RepresentationInput& input,
    const ResolvedRepresentation previous) noexcept
{
    const auto half = math::Double3{
        std::abs(input.halfExtentsMeters.x),
        std::abs(input.halfExtentsMeters.y),
        std::abs(input.halfExtentsMeters.z)};

    const f64 radius =
        std::max(
            math::Length(half),
            0.01);

    const math::Double3 delta =
        input.volumeCenterInFrameMeters -
        input.observerInFrameMeters;

    const f64 centerDistance =
        std::max(
            math::Length(delta),
            0.0);
    const f64 boundsDistance =
        std::max(
            centerDistance - radius,
            0.0);

    const f64 tanHalfFov =
        std::tan(
            std::clamp<f64>(
                input.verticalFovRadians,
                0.05,
                3.05) *
            0.5);

    const f32 projected =
        centerDistance <= radius
            ? static_cast<f32>(
                  std::max(
                      input.viewportHeightPixels,
                      1U))
            : static_cast<f32>(
                  radius /
                  std::max(
                      centerDistance * tanHalfFov,
                      1.0e-6) *
                  static_cast<f64>(
                      std::max(
                          input.viewportHeightPixels,
                          1U)));

    const f64 liveDistance =
        std::max(
            input.liveDistanceMeters,
            0.01);
    const f64 passiveDistance =
        std::max(
            input.passiveDistanceMeters,
            liveDistance + 0.01);
    const f32 livePixels =
        std::max(
            input.liveProjectedPixels,
            1.0F);
    const f32 passivePixels =
        std::clamp(
            input.passiveProjectedPixels,
            0.1F,
            livePixels);
    const f32 hysteresis =
        std::clamp(
            input.hysteresisFraction,
            0.0F,
            0.45F);

    RepresentationDecision result{
        .previousRepresentation = previous,
        .centerDistanceMeters = centerDistance,
        .distanceToBoundsMeters = boundsDistance,
        .projectedDiameterPixels = projected
    };

    u64 stable = 0x4f52424954564f4cULL;
    stable = Mix(stable, input.volume.high);
    stable = Mix(stable, input.volume.low);
    stable = Mix(stable, input.stableFrame);
    stable = Mix(stable, input.stableBody);
    result.stableAddressFingerprint = stable;

    if (input.authoredMode !=
        world_model::VolumeRepresentationMode::Auto)
    {
        result.forced = true;
        result.representation =
            ForcedRepresentation(
                input.authoredMode,
                input.bakedAvailable,
                result.bakedFallback);
    }
    else
    {
        const f64 liveEnterDistance =
            liveDistance * (1.0 - hysteresis);
        const f64 liveExitDistance =
            liveDistance * (1.0 + hysteresis);
        const f64 passiveEnterDistance =
            passiveDistance * (1.0 + hysteresis);
        const f64 passiveExitDistance =
            passiveDistance * (1.0 - hysteresis);

        const f32 liveEnterPixels =
            livePixels * (1.0F + hysteresis);
        const f32 liveExitPixels =
            livePixels * (1.0F - hysteresis);
        const f32 passiveEnterPixels =
            passivePixels * (1.0F - hysteresis);
        const f32 passiveExitPixels =
            passivePixels * (1.0F + hysteresis);

        const bool liveEvidence =
            boundsDistance <= liveEnterDistance ||
            projected >= liveEnterPixels;
        const bool leaveLive =
            boundsDistance > liveExitDistance &&
            projected < liveExitPixels;
        const bool passiveEvidence =
            boundsDistance >= passiveEnterDistance &&
            projected <= passiveEnterPixels;
        const bool leavePassive =
            boundsDistance < passiveExitDistance ||
            projected > passiveExitPixels;

        switch (previous)
        {
        case ResolvedRepresentation::Live:
            result.representation =
                leaveLive
                    ? (passiveEvidence
                           ? ResolvedRepresentation::Passive
                           : ResolvedRepresentation::Coarse)
                    : ResolvedRepresentation::Live;
            break;
        case ResolvedRepresentation::Passive:
        case ResolvedRepresentation::Baked:
            result.representation =
                leavePassive
                    ? (liveEvidence
                           ? ResolvedRepresentation::Live
                           : ResolvedRepresentation::Coarse)
                    : ResolvedRepresentation::Passive;
            break;
        case ResolvedRepresentation::Coarse:
            result.representation =
                liveEvidence
                    ? ResolvedRepresentation::Live
                    : passiveEvidence
                        ? ResolvedRepresentation::Passive
                        : ResolvedRepresentation::Coarse;
            break;
        }
    }

    const f64 liveBlendHalfWidth =
        std::max(
            liveDistance *
                std::max<f64>(hysteresis, 0.03),
            0.5);
    const f64 passiveBlendHalfWidth =
        std::max(
            passiveDistance *
                std::max<f64>(hysteresis, 0.03),
            1.0);

    const f32 liveToCoarse =
        SmoothStep(
            liveDistance - liveBlendHalfWidth,
            liveDistance + liveBlendHalfWidth,
            boundsDistance);
    const f32 coarseToPassive =
        SmoothStep(
            passiveDistance - passiveBlendHalfWidth,
            passiveDistance + passiveBlendHalfWidth,
            boundsDistance);

    result.liveWeight =
        1.0F - liveToCoarse;
    result.passiveWeight =
        coarseToPassive;
    result.coarseWeight =
        std::max(
            1.0F -
                result.liveWeight -
                result.passiveWeight,
            0.0F);

    if (result.forced)
    {
        result.liveWeight = 0.0F;
        result.coarseWeight = 0.0F;
        result.passiveWeight = 0.0F;
        result.bakedWeight = 0.0F;

        switch (result.representation)
        {
        case ResolvedRepresentation::Live:
            result.liveWeight = 1.0F;
            break;
        case ResolvedRepresentation::Coarse:
            result.coarseWeight = 1.0F;
            break;
        case ResolvedRepresentation::Passive:
            result.passiveWeight = 1.0F;
            break;
        case ResolvedRepresentation::Baked:
            result.bakedWeight = 1.0F;
            break;
        }
    }

    result.denseFieldRequired =
        result.representation ==
            ResolvedRepresentation::Live ||
        result.liveWeight > 0.001F;

    result.transitionActive =
        !result.forced &&
        ((result.liveWeight > 0.001F &&
          result.liveWeight < 0.999F) ||
         (result.passiveWeight > 0.001F &&
          result.passiveWeight < 0.999F));

    return result;
}

bool VolumeRepresentationService::Key::operator<(
    const Key& other) const noexcept
{
    return std::tie(
               viewport,
               volume.high,
               volume.low) <
        std::tie(
               other.viewport,
               other.volume.high,
               other.volume.low);
}

RepresentationDecision
VolumeRepresentationService::Resolve(
    const std::string_view viewportId,
    const RepresentationInput& input)
{
    Key key{
        .viewport = std::string(viewportId),
        .volume = input.volume
    };

    const auto found =
        decisions_.find(key);

    const auto previous =
        found == decisions_.end()
            ? ResolvedRepresentation::Live
            : found->second.representation;

    auto decision =
        ResolveRepresentation(
            input,
            previous);

    decisions_.insert_or_assign(
        std::move(key),
        decision);

    return decision;
}

RepresentationDecision
VolumeRepresentationService::Diagnostics(
    const std::string_view viewportId,
    const scene::ObjectId volume) const noexcept
{
    const auto found =
        decisions_.find({
            .viewport = std::string(viewportId),
            .volume = volume
        });

    return found == decisions_.end()
        ? RepresentationDecision{}
        : found->second;
}

void VolumeRepresentationService::RemoveMissing(
    const scene::ObjectStore& objects)
{
    std::erase_if(
        decisions_,
        [&objects](const auto& item)
        {
            const auto record =
                objects.Find(
                    item.first.volume);
            return
                !record.has_value() ||
                record->type !=
                    world_model::kVolumeType;
        });
}

std::string_view ResolvedRepresentationName(
    const ResolvedRepresentation value) noexcept
{
    switch (value)
    {
    case ResolvedRepresentation::Live:
        return "Live";
    case ResolvedRepresentation::Coarse:
        return "Coarse";
    case ResolvedRepresentation::Passive:
        return "Passive";
    case ResolvedRepresentation::Baked:
        return "Baked";
    }

    return "Unknown";
}
} // namespace orbit::volume_representation

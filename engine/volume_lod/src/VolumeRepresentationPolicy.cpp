#include <orbit/volume_lod/VolumeRepresentationPolicy.hpp>

#include <algorithm>
#include <cmath>
#include <numbers>

namespace orbit::volume_lod
{
namespace
{
[[nodiscard]] f64 Length(
    const math::Double3 value) noexcept
{
    return
        std::sqrt(
            value.x * value.x +
            value.y * value.y +
            value.z * value.z);
}

[[nodiscard]] f64 DomainRadius(
    const world_model::ResolvedVolumeDomain& domain) noexcept
{
    return
        std::sqrt(
            domain.halfExtentsMeters.x *
                domain.halfExtentsMeters.x +
            domain.halfExtentsMeters.y *
                domain.halfExtentsMeters.y +
            domain.halfExtentsMeters.z *
                domain.halfExtentsMeters.z);
}

[[nodiscard]] f32 Smooth01(
    const f64 value) noexcept
{
    const f32 t =
        static_cast<f32>(
            std::clamp(
                value,
                0.0,
                1.0));

    return
        t * t *
        (3.0F - 2.0F * t);
}

[[nodiscard]] f32 ProjectedDiameterPixels(
    const f64 radiusMeters,
    const f64 distanceMeters,
    const u32 viewportHeight,
    const f32 verticalFovRadians) noexcept
{
    if (radiusMeters <= 0.0 ||
        viewportHeight == 0U ||
        !std::isfinite(
            verticalFovRadians) ||
        verticalFovRadians <= 0.0F ||
        verticalFovRadians >=
            std::numbers::pi_v<f32>)
    {
        return 0.0F;
    }

    if (distanceMeters <= radiusMeters)
    {
        return
            static_cast<f32>(
                viewportHeight);
    }

    const f64 angularRadius =
        std::asin(
            std::clamp(
                radiusMeters /
                    distanceMeters,
                0.0,
                1.0));

    const f64 pixelsPerRadian =
        static_cast<f64>(
            viewportHeight) /
        static_cast<f64>(
            verticalFovRadians);

    return
        static_cast<f32>(
            2.0 *
            angularRadius *
            pixelsPerRadian);
}

[[nodiscard]] StableVolumeAddress StableAddressFor(
    const math::Double3 center,
    const VolumeLodContext& context,
    const f64 cellSizeMeters) noexcept
{
    const f64 cell =
        std::max(
            std::abs(
                cellSizeMeters),
            0.001);

    return {
        .frame = context.frame,
        .body = context.body,
        .x = static_cast<i64>(
            std::floor(center.x / cell)),
        .y = static_cast<i64>(
            std::floor(center.y / cell)),
        .z = static_cast<i64>(
            std::floor(center.z / cell))
    };
}
} // namespace

VolumeRepresentationPolicyService::Entry&
VolumeRepresentationPolicyService::Ensure(
    const scene::ObjectId volume)
{
    const auto found =
        std::find_if(
            entries_.begin(),
            entries_.end(),
            [volume](const Entry& entry)
            {
                return
                    entry.volume ==
                    volume;
            });

    if (found != entries_.end())
    {
        return *found;
    }

    entries_.push_back({
        .volume = volume
    });

    return entries_.back();
}

VolumeLodPolicy&
VolumeRepresentationPolicyService::Policy(
    const scene::ObjectId volume)
{
    return Ensure(volume).policy;
}

const VolumeLodPolicy*
VolumeRepresentationPolicyService::FindPolicy(
    const scene::ObjectId volume) const noexcept
{
    const auto found =
        std::find_if(
            entries_.begin(),
            entries_.end(),
            [volume](const Entry& entry)
            {
                return
                    entry.volume ==
                    volume;
            });

    return
        found == entries_.end()
            ? nullptr
            : &found->policy;
}

VolumeRepresentationDecision
VolumeRepresentationPolicyService::Resolve(
    const world_model::ResolvedVolumeDomain& authored,
    const VolumeLodContext& context)
{
    auto& entry =
        Ensure(
            authored.object);

    auto& policy =
        entry.policy;

    policy.liveDistanceMeters =
        std::max(
            policy.liveDistanceMeters,
            0.0);
    policy.coarseDistanceMeters =
        std::max(
            policy.coarseDistanceMeters,
            policy.liveDistanceMeters);
    policy.liveProjectedDiameterPixels =
        std::max(
            policy.liveProjectedDiameterPixels,
            0.0F);
    policy.coarseProjectedDiameterPixels =
        std::clamp(
            policy.coarseProjectedDiameterPixels,
            0.0F,
            policy.liveProjectedDiameterPixels);
    policy.coarseResolutionScale =
        std::clamp(
            policy.coarseResolutionScale,
            0.0625F,
            1.0F);
    policy.hysteresisFraction =
        std::clamp(
            policy.hysteresisFraction,
            0.0F,
            0.45F);
    policy.stableAddressCellMeters =
        std::max(
            std::abs(
                policy.stableAddressCellMeters),
            0.001);

    VolumeRepresentationDecision result;
    result.runtimeCenterMeters =
        authored.centerMeters;

    switch (policy.followMode)
    {
    case VolumeFollowMode::AuthoredCenter:
        break;

    case VolumeFollowMode::Camera:
        result.runtimeCenterMeters =
            context.cameraPositionInFrameMeters;
        break;

    case VolumeFollowMode::Object:
        if (context.
                followObjectPositionInFrameMeters.
                has_value())
        {
            result.runtimeCenterMeters =
                *context.
                    followObjectPositionInFrameMeters;
        }
        else
        {
            result.followTargetResolved =
                false;
        }
        break;
    }

    const auto delta =
        authored.centerMeters -
        context.cameraPositionInFrameMeters;

    result.observerDistanceMeters =
        Length(delta);

    const f64 radius =
        DomainRadius(authored);

    result.projectedDiameterPixels =
        ProjectedDiameterPixels(
            radius,
            result.observerDistanceMeters,
            context.viewportHeight,
            context.verticalFovRadians);

    result.stableAddress =
        StableAddressFor(
            result.runtimeCenterMeters,
            context,
            policy.stableAddressCellMeters);

    const auto authoredMode =
        authored.representationMode;

    if (authoredMode !=
        world_model::
            VolumeRepresentationMode::Auto)
    {
        result.requested =
            RepresentationFromAuthoredMode(
                authoredMode);
        result.effective =
            result.requested;
        result.forced = true;
        result.reason = "forced authored representation";

        // M37 owns actual cache playback. Until then a forced Baked request is
        // represented by the bounded passive seam instead of allocating a live
        // dense field or silently pretending a cache exists.
        if (result.requested ==
            VolumeRuntimeRepresentation::Baked)
        {
            result.effective =
                VolumeRuntimeRepresentation::Passive;
            result.bakedFallbackToPassive =
                true;
            result.reason =
                "baked requested; M37 cache unavailable, passive fallback";
        }
    }
    else
    {
        const f32 budget =
            std::clamp(
                context.budgetScale,
                0.0F,
                1.0F);

        const f64 liveDistance =
            policy.liveDistanceMeters *
            (0.55 +
             0.45 *
                static_cast<f64>(budget));
        const f64 coarseDistance =
            policy.coarseDistanceMeters *
            (0.60 +
             0.40 *
                static_cast<f64>(budget));

        const bool liveByImportance =
            result.projectedDiameterPixels >=
            policy.liveProjectedDiameterPixels;
        const bool coarseByImportance =
            result.projectedDiameterPixels >=
            policy.coarseProjectedDiameterPixels;

        const f64 liveBand =
            std::max(
                liveDistance *
                    static_cast<f64>(
                        policy.hysteresisFraction),
                0.01);
        const f64 coarseBand =
            std::max(
                coarseDistance *
                    static_cast<f64>(
                        policy.hysteresisFraction),
                0.01);

        f64 liveThreshold =
            liveDistance;
        f64 coarseThreshold =
            coarseDistance;

        if (entry.hasPrevious)
        {
            if (entry.previous ==
                VolumeRuntimeRepresentation::Live)
            {
                liveThreshold +=
                    liveBand;
            }
            else
            {
                liveThreshold -=
                    liveBand;
            }

            if (entry.previous ==
                    VolumeRuntimeRepresentation::Live ||
                entry.previous ==
                    VolumeRuntimeRepresentation::Coarse)
            {
                coarseThreshold +=
                    coarseBand;
            }
            else
            {
                coarseThreshold -=
                    coarseBand;
            }
        }

        if (result.observerDistanceMeters <=
                liveThreshold ||
            liveByImportance)
        {
            result.requested =
                VolumeRuntimeRepresentation::Live;
            result.reason =
                liveByImportance &&
                    result.observerDistanceMeters >
                        liveThreshold
                    ? "projected importance promoted live"
                    : "inside live region";
        }
        else if (
            result.observerDistanceMeters <=
                coarseThreshold ||
            coarseByImportance)
        {
            result.requested =
                VolumeRuntimeRepresentation::Coarse;
            result.reason =
                coarseByImportance &&
                    result.observerDistanceMeters >
                        coarseThreshold
                    ? "projected importance promoted coarse"
                    : "inside coarse region";
        }
        else
        {
            result.requested =
                VolumeRuntimeRepresentation::Passive;
            result.reason =
                "outside interactive regions";
        }

        result.effective =
            result.requested;

        const f64 liveBlendStart =
            std::max(
                liveDistance -
                    liveBand,
                0.0);
        const f64 liveBlendEnd =
            liveDistance +
            liveBand;

        const f64 coarseBlendStart =
            std::max(
                coarseDistance -
                    coarseBand,
                liveBlendEnd);
        const f64 coarseBlendEnd =
            coarseDistance +
            coarseBand;

        if (result.observerDistanceMeters <=
            liveBlendStart)
        {
            result.liveWeight = 1.0F;
            result.coarseWeight = 0.0F;
            result.passiveWeight = 0.0F;
        }
        else if (
            result.observerDistanceMeters <
            liveBlendEnd)
        {
            const f32 t =
                Smooth01(
                    (result.observerDistanceMeters -
                     liveBlendStart) /
                    std::max(
                        liveBlendEnd -
                            liveBlendStart,
                        1.0e-6));

            result.liveWeight =
                1.0F - t;
            result.coarseWeight =
                t;
            result.passiveWeight =
                0.0F;
        }
        else if (
            result.observerDistanceMeters <=
            coarseBlendStart)
        {
            result.liveWeight = 0.0F;
            result.coarseWeight = 1.0F;
            result.passiveWeight = 0.0F;
        }
        else if (
            result.observerDistanceMeters <
            coarseBlendEnd)
        {
            const f32 t =
                Smooth01(
                    (result.observerDistanceMeters -
                     coarseBlendStart) /
                    std::max(
                        coarseBlendEnd -
                            coarseBlendStart,
                        1.0e-6));

            result.liveWeight = 0.0F;
            result.coarseWeight =
                1.0F - t;
            result.passiveWeight =
                t;
        }
        else
        {
            result.liveWeight = 0.0F;
            result.coarseWeight = 0.0F;
            result.passiveWeight = 1.0F;
        }
    }

    result.allocateDenseFields =
        result.effective ==
            VolumeRuntimeRepresentation::Live ||
        result.effective ==
            VolumeRuntimeRepresentation::Coarse;

    result.runLiveSolver =
        result.allocateDenseFields;

    result.runtimeResolution =
        result.effective ==
                VolumeRuntimeRepresentation::Coarse
            ? std::max<u32>(
                  8U,
                  static_cast<u32>(
                      std::lround(
                          static_cast<f64>(
                              authored.resolution) *
                          static_cast<f64>(
                              policy.
                                  coarseResolutionScale))))
            : authored.resolution;

    entry.previous =
        result.effective;
    entry.hasPrevious =
        true;

    return result;
}

world_model::ResolvedVolumeDomain
VolumeRepresentationPolicyService::RuntimeDomain(
    const world_model::ResolvedVolumeDomain& authored,
    const VolumeRepresentationDecision& decision) const noexcept
{
    auto runtime =
        authored;

    runtime.centerMeters =
        decision.runtimeCenterMeters;
    runtime.resolution =
        decision.runtimeResolution;

    return runtime;
}

void VolumeRepresentationPolicyService::RemoveMissing(
    const scene::ObjectStore& objects)
{
    std::erase_if(
        entries_,
        [&objects](const Entry& entry)
        {
            const auto record =
                objects.Find(
                    entry.volume);

            return
                !record.has_value() ||
                record->type !=
                    world_model::
                        kVolumeType;
        });
}

VolumeRuntimeRepresentation
RepresentationFromAuthoredMode(
    const world_model::VolumeRepresentationMode mode) noexcept
{
    switch (mode)
    {
    case world_model::
            VolumeRepresentationMode::Live:
        return
            VolumeRuntimeRepresentation::Live;
    case world_model::
            VolumeRepresentationMode::Coarse:
        return
            VolumeRuntimeRepresentation::Coarse;
    case world_model::
            VolumeRepresentationMode::Passive:
        return
            VolumeRuntimeRepresentation::Passive;
    case world_model::
            VolumeRepresentationMode::Baked:
        return
            VolumeRuntimeRepresentation::Baked;
    case world_model::
            VolumeRepresentationMode::Auto:
        return
            VolumeRuntimeRepresentation::Live;
    }

    return
        VolumeRuntimeRepresentation::Live;
}

std::string_view
VolumeRuntimeRepresentationName(
    const VolumeRuntimeRepresentation representation) noexcept
{
    switch (representation)
    {
    case VolumeRuntimeRepresentation::Live:
        return "Live";
    case VolumeRuntimeRepresentation::Coarse:
        return "Coarse";
    case VolumeRuntimeRepresentation::Passive:
        return "Passive";
    case VolumeRuntimeRepresentation::Baked:
        return "Baked";
    }

    return "Unknown";
}

std::string_view
VolumeFollowModeName(
    const VolumeFollowMode mode) noexcept
{
    switch (mode)
    {
    case VolumeFollowMode::AuthoredCenter:
        return "Authored Center";
    case VolumeFollowMode::Camera:
        return "Camera";
    case VolumeFollowMode::Object:
        return "Object";
    }

    return "Unknown";
}
} // namespace orbit::volume_lod

#pragma once

#include <orbit/core/Types.hpp>
#include <orbit/math/Vector.hpp>
#include <orbit/scene/ObjectStore.hpp>
#include <orbit/world_model/VolumeSchemas.hpp>

#include <algorithm>
#include <map>
#include <optional>
#include <string>
#include <string_view>

namespace orbit::volume_representation
{
enum class ResolvedRepresentation : u8
{
    Live = 0U,
    Coarse = 1U,
    Passive = 2U,
    Baked = 3U
};

enum class FollowTarget : u8
{
    AuthoredDomain = 0U,
    Camera = 1U,
    Object = 2U
};

struct VolumeRepresentationSettings
{
    FollowTarget followTarget{
        FollowTarget::AuthoredDomain};

    // Object is semantic identity; the host runtime resolves its frame-space
    // position through the appropriate actor/transform adapter. Keeping the
    // resolved position separate prevents this module from depending on any
    // game-specific transform type.
    std::optional<scene::ObjectId> followObject;
    std::optional<math::Double3>
        followObjectPositionInFrameMeters;

    f64 liveDistanceMeters{120.0};
    f64 passiveDistanceMeters{1200.0};
    f32 liveProjectedPixels{96.0F};
    f32 passiveProjectedPixels{12.0F};
    f32 hysteresisFraction{0.12F};

    u32 coarseResolution{24U};
    u32 passiveResolution{8U};
    u32 coarseRaymarchSteps{24U};
    u32 passiveRaymarchSteps{8U};
};

struct RepresentationInput
{
    scene::ObjectId volume{};
    math::Double3 volumeCenterInFrameMeters{};
    math::Double3 halfExtentsMeters{1.0,1.0,1.0};
    math::Double3 observerInFrameMeters{};

    FollowTarget followTarget{
        FollowTarget::AuthoredDomain};
    std::optional<scene::ObjectId> followObject;
    std::optional<math::Double3>
        followObjectPositionInFrameMeters;

    u64 stableFrame{0U};
    u64 stableBody{0U};

    u32 viewportHeightPixels{1080U};
    f32 verticalFovRadians{1.0F};

    world_model::VolumeRepresentationMode authoredMode{
        world_model::VolumeRepresentationMode::Auto};

    f64 liveDistanceMeters{120.0};
    f64 passiveDistanceMeters{1200.0};
    f32 liveProjectedPixels{96.0F};
    f32 passiveProjectedPixels{12.0F};
    f32 hysteresisFraction{0.12F};

    bool bakedAvailable{false};
};

struct RepresentationDecision
{
    ResolvedRepresentation representation{
        ResolvedRepresentation::Live};
    ResolvedRepresentation previousRepresentation{
        ResolvedRepresentation::Live};

    f32 liveWeight{1.0F};
    f32 coarseWeight{0.0F};
    f32 passiveWeight{0.0F};
    f32 bakedWeight{0.0F};

    math::Double3 runtimeCenterInFrameMeters{};
    f64 centerDistanceMeters{0.0};
    f64 distanceToBoundsMeters{0.0};
    f32 projectedDiameterPixels{0.0F};

    bool forced{false};
    bool denseFieldRequired{true};
    bool transitionActive{false};
    bool bakedFallback{false};
    bool followTargetResolved{true};

    u64 stableAddressFingerprint{0U};
};

struct PublishedAllocationPolicy
{
    ResolvedRepresentation representation{
        ResolvedRepresentation::Live};
    bool denseFieldRequired{true};
    u32 coarseResolution{24U};
    u32 passiveResolution{8U};
    math::Double3 runtimeCenterInFrameMeters{};
    bool hasRuntimeCenter{false};
};

namespace detail
{
[[nodiscard]] inline std::map<
    scene::ObjectId,
    PublishedAllocationPolicy>&
AllocationPolicies() noexcept
{
    static std::map<
        scene::ObjectId,
        PublishedAllocationPolicy>
        policies;
    return policies;
}
} // namespace detail

inline void PublishAllocationPolicy(
    const scene::ObjectId volume,
    const RepresentationDecision& decision,
    const VolumeRepresentationSettings& settings)
{
    detail::AllocationPolicies().insert_or_assign(
        volume,
        PublishedAllocationPolicy{
            .representation = decision.representation,
            .denseFieldRequired = decision.denseFieldRequired,
            .coarseResolution =
                std::clamp(
                    settings.coarseResolution,
                    8U,
                    128U),
            .passiveResolution =
                std::clamp(
                    settings.passiveResolution,
                    8U,
                    64U),
            .runtimeCenterInFrameMeters =
                decision.runtimeCenterInFrameMeters,
            .hasRuntimeCenter =
                decision.followTargetResolved &&
                settings.followTarget !=
                    FollowTarget::AuthoredDomain
        });
}

[[nodiscard]] inline std::optional<PublishedAllocationPolicy>
AllocationPolicy(
    const scene::ObjectId volume) noexcept
{
    const auto& policies =
        detail::AllocationPolicies();
    const auto found =
        policies.find(volume);

    return found == policies.end()
        ? std::nullopt
        : std::optional(found->second);
}

inline void ClearAllocationPolicy(
    const scene::ObjectId volume) noexcept
{
    detail::AllocationPolicies().erase(volume);
}

[[nodiscard]] RepresentationDecision
ResolveRepresentation(
    const RepresentationInput& input,
    ResolvedRepresentation previous =
        ResolvedRepresentation::Live) noexcept;

class VolumeRepresentationService
{
public:
    [[nodiscard]] VolumeRepresentationSettings& Settings(
        const scene::ObjectId volume)
    {
        return settings_[volume];
    }

    [[nodiscard]] VolumeRepresentationSettings Settings(
        const scene::ObjectId volume) const noexcept
    {
        const auto found = settings_.find(volume);
        return found == settings_.end()
            ? VolumeRepresentationSettings{}
            : found->second;
    }

    [[nodiscard]] RepresentationDecision Resolve(
        std::string_view viewportId,
        const RepresentationInput& input);

    [[nodiscard]] RepresentationDecision Diagnostics(
        std::string_view viewportId,
        scene::ObjectId volume) const noexcept;

    void RemoveMissing(
        const scene::ObjectStore& objects);

private:
    struct Key
    {
        std::string viewport;
        scene::ObjectId volume{};

        [[nodiscard]] bool operator<(
            const Key& other) const noexcept;
    };

    std::map<scene::ObjectId, VolumeRepresentationSettings> settings_;
    std::map<Key, RepresentationDecision> decisions_;
};

[[nodiscard]] std::string_view
ResolvedRepresentationName(
    ResolvedRepresentation value) noexcept;

[[nodiscard]] constexpr std::string_view
FollowTargetName(
    const FollowTarget value) noexcept
{
    switch (value)
    {
    case FollowTarget::AuthoredDomain:
        return "Authored Domain";
    case FollowTarget::Camera:
        return "Camera";
    case FollowTarget::Object:
        return "Object";
    }

    return "Unknown";
}
} // namespace orbit::volume_representation

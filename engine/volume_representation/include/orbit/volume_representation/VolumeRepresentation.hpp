#pragma once

#include <orbit/core/Types.hpp>
#include <orbit/math/Vector.hpp>
#include <orbit/scene/ObjectStore.hpp>
#include <orbit/world_model/VolumeSchemas.hpp>

#include <map>
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
    Camera = 1U
};

struct VolumeRepresentationSettings
{
    FollowTarget followTarget{
        FollowTarget::AuthoredDomain};

    f64 liveDistanceMeters{120.0};
    f64 passiveDistanceMeters{1200.0};
    f32 liveProjectedPixels{96.0F};
    f32 passiveProjectedPixels{12.0F};
    f32 hysteresisFraction{0.12F};

    // Coarse/passive are deliberately field-free procedural representations.
    // These budgets bound their screen-space ray work independent of authored
    // live-field resolution.
    u32 coarseRaymarchSteps{24U};
    u32 passiveRaymarchSteps{8U};
};

struct RepresentationInput
{
    scene::ObjectId volume{};
    math::Double3 volumeCenterInFrameMeters{};
    math::Double3 halfExtentsMeters{1.0,1.0,1.0};
    math::Double3 observerInFrameMeters{};

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

    f64 centerDistanceMeters{0.0};
    f64 distanceToBoundsMeters{0.0};
    f32 projectedDiameterPixels{0.0F};

    bool forced{false};
    bool denseFieldRequired{true};
    bool transitionActive{false};
    bool bakedFallback{false};

    u64 stableAddressFingerprint{0U};
};

[[nodiscard]] RepresentationDecision
ResolveRepresentation(
    const RepresentationInput& input,
    ResolvedRepresentation previous =
        ResolvedRepresentation::Live) noexcept;

class VolumeRepresentationService
{
public:
    [[nodiscard]] VolumeRepresentationSettings& Settings(
        scene::ObjectId volume);

    [[nodiscard]] VolumeRepresentationSettings Settings(
        scene::ObjectId volume) const noexcept;

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

[[nodiscard]] std::string_view
FollowTargetName(
    FollowTarget value) noexcept;
} // namespace orbit::volume_representation

#pragma once

#include <orbit/core/Types.hpp>
#include <orbit/frames/FrameGraph.hpp>
#include <orbit/math/Vector.hpp>
#include <orbit/scene/ObjectStore.hpp>
#include <orbit/universe/BodyRegistry.hpp>
#include <orbit/world_model/VolumeSchemas.hpp>

#include <optional>
#include <string_view>
#include <vector>

namespace orbit::volume_lod
{
enum class VolumeRuntimeRepresentation : u8
{
    Live = 0U,
    Coarse = 1U,
    Passive = 2U,
    Baked = 3U
};

enum class VolumeFollowMode : u8
{
    AuthoredCenter = 0U,
    Camera = 1U,
    Object = 2U
};

struct VolumeLodPolicy
{
    VolumeFollowMode followMode{
        VolumeFollowMode::AuthoredCenter};

    // Object mode uses an externally resolved stable object position. The
    // representation service never reaches into renderer transforms.
    std::optional<scene::ObjectId> followObject;

    f64 liveDistanceMeters{120.0};
    f64 coarseDistanceMeters{600.0};

    // Projected size keeps visually important effects promoted even when the
    // distance-only rule would otherwise demote them.
    f32 liveProjectedDiameterPixels{96.0F};
    f32 coarseProjectedDiameterPixels{12.0F};

    // Runtime coarse fields reuse the M31 field model at reduced logical
    // resolution. The authored domain remains unchanged.
    f32 coarseResolutionScale{0.25F};

    // Symmetric overlap around tier boundaries. Hysteresis is evaluated from
    // the previous stable runtime representation, not from frame-local noise.
    f32 hysteresisFraction{0.12F};

    // Stable-address cell used only for diagnostics/identity. It is explicitly
    // independent of the floating GPU presentation origin.
    f64 stableAddressCellMeters{32.0};
};

struct VolumeLodContext
{
    frames::FrameId frame{};
    universe::BodyId body{};

    math::Double3 cameraPositionInFrameMeters{};
    std::optional<math::Double3>
        followObjectPositionInFrameMeters;

    u32 viewportWidth{1U};
    u32 viewportHeight{1U};
    f32 verticalFovRadians{1.0F};

    // Runtime pressure may reduce promoted detail without changing authored
    // policy. 1 = nominal; 0 = strongest demotion pressure.
    f32 budgetScale{1.0F};
};

struct StableVolumeAddress
{
    frames::FrameId frame{};
    universe::BodyId body{};
    i64 x{0};
    i64 y{0};
    i64 z{0};

    [[nodiscard]] constexpr bool operator==(
        const StableVolumeAddress&) const noexcept = default;
};

struct VolumeRepresentationDecision
{
    VolumeRuntimeRepresentation requested{
        VolumeRuntimeRepresentation::Live};
    VolumeRuntimeRepresentation effective{
        VolumeRuntimeRepresentation::Live};

    bool forced{false};
    bool bakedFallbackToPassive{false};
    bool allocateDenseFields{true};
    bool runLiveSolver{true};
    bool followTargetResolved{true};

    math::Double3 runtimeCenterMeters{};
    u32 runtimeResolution{64U};

    f64 observerDistanceMeters{0.0};
    f32 projectedDiameterPixels{0.0F};

    // Adjacent representation overlap weights. They sum to approximately one
    // and make transition state visible/diagnosable instead of implicit.
    f32 liveWeight{1.0F};
    f32 coarseWeight{0.0F};
    f32 passiveWeight{0.0F};

    StableVolumeAddress stableAddress{};
    std::string_view reason{"live"};
};

class VolumeRepresentationPolicyService
{
public:
    [[nodiscard]] VolumeLodPolicy& Policy(
        scene::ObjectId volume);

    [[nodiscard]] const VolumeLodPolicy* FindPolicy(
        scene::ObjectId volume) const noexcept;

    [[nodiscard]] VolumeRepresentationDecision Resolve(
        const world_model::ResolvedVolumeDomain& authored,
        const VolumeLodContext& context);

    [[nodiscard]] world_model::ResolvedVolumeDomain RuntimeDomain(
        const world_model::ResolvedVolumeDomain& authored,
        const VolumeRepresentationDecision& decision) const noexcept;

    void RemoveMissing(
        const scene::ObjectStore& objects);

private:
    struct Entry
    {
        scene::ObjectId volume{};
        VolumeLodPolicy policy{};
        VolumeRuntimeRepresentation previous{
            VolumeRuntimeRepresentation::Live};
        bool hasPrevious{false};
    };

    [[nodiscard]] Entry& Ensure(
        scene::ObjectId volume);

    std::vector<Entry> entries_;
};

[[nodiscard]] VolumeRuntimeRepresentation
RepresentationFromAuthoredMode(
    world_model::VolumeRepresentationMode mode) noexcept;

[[nodiscard]] std::string_view
VolumeRuntimeRepresentationName(
    VolumeRuntimeRepresentation representation) noexcept;

[[nodiscard]] std::string_view
VolumeFollowModeName(
    VolumeFollowMode mode) noexcept;
} // namespace orbit::volume_lod

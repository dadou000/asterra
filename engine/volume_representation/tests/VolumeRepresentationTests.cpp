#include <orbit/volume_representation/VolumeRepresentation.hpp>

#include <cmath>
#include <cstdlib>
#include <iostream>
#include <source_location>

namespace
{
void Check(
    const bool condition,
    const std::source_location location =
        std::source_location::current())
{
    if (!condition)
    {
        std::cerr
            << "Volume representation test failed at "
            << location.file_name()
            << ':'
            << location.line()
            << '\n';
        std::exit(1);
    }
}
} // namespace

int main()
{
    using namespace orbit;
    using namespace orbit::volume_representation;

    const scene::ObjectId volumeId{
        .high = 1U,
        .low = 2U
    };

    RepresentationInput input{
        .volume = volumeId,
        .volumeCenterInFrameMeters = {0.0,0.0,0.0},
        .halfExtentsMeters = {10.0,10.0,10.0},
        .observerInFrameMeters = {0.0,0.0,30.0},
        .followTarget = FollowTarget::AuthoredDomain,
        .stableFrame = 17U,
        .stableBody = 31U,
        .viewportHeightPixels = 1080U,
        .verticalFovRadians = 1.0F,
        .liveDistanceMeters = 100.0,
        .passiveDistanceMeters = 1000.0,
        .liveProjectedPixels = 80.0F,
        .passiveProjectedPixels = 10.0F,
        .hysteresisFraction = 0.10F
    };

    const auto near =
        ResolveRepresentation(
            input,
            ResolvedRepresentation::Live);
    Check(near.representation ==
        ResolvedRepresentation::Live);
    Check(near.denseFieldRequired);
    Check(near.followTargetResolved);
    Check(near.runtimeCenterInFrameMeters ==
        input.volumeCenterInFrameMeters);

    input.observerInFrameMeters =
        {0.0,0.0,400.0};
    const auto medium =
        ResolveRepresentation(
            input,
            ResolvedRepresentation::Live);
    Check(medium.representation ==
        ResolvedRepresentation::Coarse);

    input.observerInFrameMeters =
        {0.0,0.0,5000.0};
    const auto far =
        ResolveRepresentation(
            input,
            ResolvedRepresentation::Coarse);
    Check(far.representation ==
        ResolvedRepresentation::Passive);
    Check(!far.denseFieldRequired);

    // Hysteresis holds Live just outside the raw live threshold.
    input.observerInFrameMeters =
        {0.0,0.0,112.0};
    const auto heldLive =
        ResolveRepresentation(
            input,
            ResolvedRepresentation::Live);
    Check(heldLive.representation ==
        ResolvedRepresentation::Live);

    // Explicit force bypasses Auto policy.
    input.authoredMode =
        world_model::VolumeRepresentationMode::Coarse;
    input.observerInFrameMeters =
        {0.0,0.0,1.0};
    const auto forced =
        ResolveRepresentation(
            input,
            ResolvedRepresentation::Live);
    Check(forced.forced);
    Check(forced.representation ==
        ResolvedRepresentation::Coarse);
    Check(!forced.denseFieldRequired);

    // Baked cannot silently claim a cache before M37 supplies one.
    input.authoredMode =
        world_model::VolumeRepresentationMode::Baked;
    input.bakedAvailable = false;
    const auto missingBake =
        ResolveRepresentation(
            input,
            ResolvedRepresentation::Live);
    Check(missingBake.bakedFallback);
    Check(missingBake.representation ==
        ResolvedRepresentation::Passive);

    // Stable identity is semantic frame/body/object + stable frame-space cell
    // state and is therefore unchanged by a presentation-origin rebase.
    input.authoredMode =
        world_model::VolumeRepresentationMode::Auto;
    input.observerInFrameMeters =
        {1000000.0,2000000.0,3000000.0};
    input.volumeCenterInFrameMeters =
        {1000100.0,2000000.0,3000000.0};
    const auto beforeRebase =
        ResolveRepresentation(
            input,
            ResolvedRepresentation::Coarse);

    // A GPU-origin rebase changes neither semantic frame-space value.
    const auto afterRebase =
        ResolveRepresentation(
            input,
            beforeRebase.representation);
    Check(beforeRebase.stableAddressFingerprint ==
        afterRebase.stableAddressFingerprint);
    Check(std::isfinite(
        afterRebase.projectedDiameterPixels));

    // Camera follow produces a runtime roaming center without mutating the
    // authored Volume center.
    input.followTarget =
        FollowTarget::Camera;
    input.observerInFrameMeters =
        {500.0,25.0,-90.0};
    const auto cameraFollow =
        ResolveRepresentation(
            input,
            ResolvedRepresentation::Live);
    Check(cameraFollow.followTargetResolved);
    Check(cameraFollow.runtimeCenterInFrameMeters ==
        input.observerInFrameMeters);
    Check(input.volumeCenterInFrameMeters ==
        math::Double3{1000100.0,2000000.0,3000000.0});

    // An unresolved actor target is explicit and keeps the authored center.
    input.followTarget =
        FollowTarget::Object;
    input.followObject =
        scene::ObjectId{.high=9U,.low=8U};
    input.followObjectPositionInFrameMeters.reset();
    const auto unresolvedActor =
        ResolveRepresentation(
            input,
            ResolvedRepresentation::Live);
    Check(!unresolvedActor.followTargetResolved);
    Check(unresolvedActor.runtimeCenterInFrameMeters ==
        input.volumeCenterInFrameMeters);

    // Once the host runtime resolves the actor, the same representation policy
    // follows it without depending on any game-specific transform class.
    input.followObjectPositionInFrameMeters =
        math::Double3{508.0,26.0,-93.0};
    const auto actorFollow =
        ResolveRepresentation(
            input,
            ResolvedRepresentation::Live);
    Check(actorFollow.followTargetResolved);
    Check(actorFollow.runtimeCenterInFrameMeters ==
        *input.followObjectPositionInFrameMeters);
    Check(actorFollow.representation ==
        ResolvedRepresentation::Live);

    // Published policy carries the roaming center to M31 allocation without
    // rewriting authored semantic state.
    VolumeRepresentationSettings settings;
    settings.followTarget = FollowTarget::Object;
    settings.followObject = input.followObject;
    settings.followObjectPositionInFrameMeters =
        input.followObjectPositionInFrameMeters;

    PublishAllocationPolicy(
        volumeId,
        actorFollow,
        settings);

    const auto published =
        AllocationPolicy(volumeId);
    Check(published.has_value());
    Check(published->hasRuntimeCenter);
    Check(published->runtimeCenterInFrameMeters ==
        actorFollow.runtimeCenterInFrameMeters);

    ClearAllocationPolicy(volumeId);
    Check(!AllocationPolicy(volumeId).has_value());

    return 0;
}

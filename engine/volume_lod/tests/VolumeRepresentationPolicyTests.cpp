#include <orbit/volume_lod/VolumeRepresentationPolicy.hpp>

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
            << "Volume LOD policy test failed at "
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
    using namespace orbit::volume_lod;

    const scene::ObjectId volumeId{
        .high = 1U,
        .low = 2U
    };

    world_model::ResolvedVolumeDomain volume{
        .object = volumeId,
        .enabled = true,
        .centerMeters = {0.0, 0.0, 0.0},
        .halfExtentsMeters = {20.0, 5.0, 20.0},
        .representationMode =
            world_model::
                VolumeRepresentationMode::Auto,
        .resolution = 128U
    };

    VolumeRepresentationPolicyService service;
    auto& policy =
        service.Policy(volumeId);

    policy.liveDistanceMeters = 100.0;
    policy.coarseDistanceMeters = 400.0;
    policy.liveProjectedDiameterPixels = 100.0F;
    policy.coarseProjectedDiameterPixels = 10.0F;
    policy.coarseResolutionScale = 0.25F;
    policy.hysteresisFraction = 0.10F;

    VolumeLodContext context{
        .frame = frames::FrameId{
            .high = 10U,
            .low = 20U},
        .body = universe::BodyId{
            .high = 30U,
            .low = 40U},
        .cameraPositionInFrameMeters =
            {0.0, 0.0, 30.0},
        .viewportWidth = 1920U,
        .viewportHeight = 1080U,
        .verticalFovRadians = 1.0F,
        .budgetScale = 1.0F
    };

    const auto live =
        service.Resolve(
            volume,
            context);

    Check(
        live.effective ==
        VolumeRuntimeRepresentation::Live);
    Check(live.allocateDenseFields);
    Check(live.runLiveSolver);
    Check(live.runtimeResolution == 128U);

    context.cameraPositionInFrameMeters =
        {0.0, 0.0, 250.0};

    const auto coarse =
        service.Resolve(
            volume,
            context);

    Check(
        coarse.effective ==
        VolumeRuntimeRepresentation::Coarse);
    Check(coarse.allocateDenseFields);
    Check(coarse.runtimeResolution == 32U);

    context.cameraPositionInFrameMeters =
        {0.0, 0.0, 2000.0};

    const auto passive =
        service.Resolve(
            volume,
            context);

    Check(
        passive.effective ==
        VolumeRuntimeRepresentation::Passive);
    Check(!passive.allocateDenseFields);
    Check(!passive.runLiveSolver);
    Check(passive.passiveWeight > 0.99F);

    // Follow-camera changes only the runtime plan. The authored center remains
    // semantic authority and is never mutated by the service.
    policy.followMode =
        VolumeFollowMode::Camera;
    context.cameraPositionInFrameMeters =
        {900.0, -20.0, 70.0};

    const auto followed =
        service.Resolve(
            volume,
            context);

    Check(
        followed.runtimeCenterMeters ==
        context.cameraPositionInFrameMeters);
    Check(
        volume.centerMeters ==
        math::Double3{});

    // Stable address depends on body/frame + double-precision semantic center,
    // not on any GPU floating-origin value.
    const auto stableA =
        followed.stableAddress;
    const auto stableB =
        service.Resolve(
            volume,
            context).
            stableAddress;

    Check(stableA == stableB);
    Check(stableA.frame == context.frame);
    Check(stableA.body == context.body);

    // Unresolved object-follow never silently teleports the domain.
    policy.followMode =
        VolumeFollowMode::Object;
    context.followObjectPositionInFrameMeters.reset();

    const auto unresolvedFollow =
        service.Resolve(
            volume,
            context);

    Check(!unresolvedFollow.followTargetResolved);
    Check(
        unresolvedFollow.runtimeCenterMeters ==
        volume.centerMeters);

    // A resolved important actor may become the runtime center.
    context.followObjectPositionInFrameMeters =
        math::Double3{50.0, 4.0, -12.0};

    const auto actorFollow =
        service.Resolve(
            volume,
            context);

    Check(actorFollow.followTargetResolved);
    Check(
        actorFollow.runtimeCenterMeters ==
        *context.followObjectPositionInFrameMeters);

    // Existing authored representation mode is the force/debug authority.
    volume.representationMode =
        world_model::
            VolumeRepresentationMode::Passive;

    const auto forcedPassive =
        service.Resolve(
            volume,
            context);

    Check(forcedPassive.forced);
    Check(
        forcedPassive.effective ==
        VolumeRuntimeRepresentation::Passive);
    Check(!forcedPassive.allocateDenseFields);

    // M37 has not provided cache playback yet. Forced Baked therefore remains
    // explicit and bounded by a passive fallback rather than allocating live.
    volume.representationMode =
        world_model::
            VolumeRepresentationMode::Baked;

    const auto baked =
        service.Resolve(
            volume,
            context);

    Check(baked.forced);
    Check(
        baked.requested ==
        VolumeRuntimeRepresentation::Baked);
    Check(
        baked.effective ==
        VolumeRuntimeRepresentation::Passive);
    Check(baked.bakedFallbackToPassive);
    Check(!baked.allocateDenseFields);

    return 0;
}

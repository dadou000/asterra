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

    RepresentationInput input{
        .volume = scene::ObjectId{.high=1U,.low=2U},
        .volumeCenterInFrameMeters = {0.0,0.0,0.0},
        .halfExtentsMeters = {10.0,10.0,10.0},
        .observerInFrameMeters = {0.0,0.0,30.0},
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

    // Stable identity is semantic frame/body/object state and is therefore
    // unchanged by presentation-origin or camera-relative rebases.
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

    // A GPU-origin rebase changes neither of these stable frame-space values.
    const auto afterRebase =
        ResolveRepresentation(
            input,
            beforeRebase.representation);
    Check(beforeRebase.stableAddressFingerprint ==
        afterRebase.stableAddressFingerprint);
    Check(std::isfinite(
        afterRebase.projectedDiameterPixels));

    return 0;
}

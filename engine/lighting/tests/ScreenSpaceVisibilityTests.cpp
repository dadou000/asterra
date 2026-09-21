#include <orbit/lighting/Visibility.hpp>

#include <cmath>

int main()
{
    using namespace orbit;
    using namespace orbit::lighting;

    LightingView view;
    view.frame = frames::FrameId{
        .high = 1U,
        .low = 2U};
    view.originInFrameMeters = {
        100.0,
        200.0,
        300.0
    };

    VisibilityQuery query{
        .purpose =
            VisibilityPurpose::Diagnostic,
        .frame = view.frame,
        .body = universe::BodyId{
            .high = 3U,
            .low = 4U},
        .originInFrameMeters = {
            110.0,
            220.0,
            330.0
        },
        .direction = {
            0.0F,
            0.0F,
            2.0F
        },
        .minimumDistanceMeters = 0.25F,
        .maximumDistanceMeters = 125.0F,
        .importance = 1.0F
    };

    const auto encoded =
        EncodeGpuVisibilityQuery(
            query,
            view);

    if (std::abs(
            encoded.
                originMinimumDistance.x -
            10.0F) >
            1.0e-6F ||
        std::abs(
            encoded.
                originMinimumDistance.y -
            20.0F) >
            1.0e-6F ||
        std::abs(
            encoded.
                originMinimumDistance.z -
            30.0F) >
            1.0e-6F ||
        encoded.
                originMinimumDistance.w !=
            0.25F)
    {
        return 1;
    }

    if (std::abs(
            encoded.
                directionMaximumDistance.z -
            1.0F) >
            1.0e-6F ||
        encoded.
                directionMaximumDistance.w !=
            125.0F)
    {
        return 2;
    }

    if (sizeof(GpuVisibilityQuery) != 48U ||
        sizeof(GpuVisibilityResult) != 48U)
    {
        return 3;
    }

    return 0;
}

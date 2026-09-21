#include <orbit/lighting/LightingView.hpp>

#include <cmath>

int main()
{
    using namespace orbit::lighting;

    LightingView a;
    a.frame.high = 1U;
    a.body.high = 2U;
    a.cameraPositionInFrameMeters =
        {1'000'123.0, -5'000.0, 42.0};
    a.gpuOriginInFrameMeters =
        {1'000'000.0, -5'000.0, 0.0};
    a.gpuOriginRevision = 7U;

    const math::Double3 point{
        1'000'160.0,
        -4'992.0,
        64.0
    };

    const auto cellA =
        StableCellForPoint(
            point,
            32.0,
            a);

    LightingView rebased = a;
    rebased.gpuOriginInFrameMeters =
        {1'000'128.0, -5'000.0, 32.0};
    rebased.gpuOriginRevision = 8U;

    const auto cellB =
        StableCellForPoint(
            point,
            32.0,
            rebased);

    if (cellA != cellB)
    {
        return 1;
    }

    const auto relativeA =
        ToLightingCameraRelative(
            point,
            a);
    const auto relativeB =
        ToLightingCameraRelative(
            point,
            rebased);

    if (relativeA == relativeB)
    {
        return 2;
    }

    const auto rebaseChange =
        ClassifyLightingViewChange(
            a,
            rebased);

    if (!HasChange(
            rebaseChange,
            LightingViewChange::GpuOriginRebase) ||
        HasChange(
            rebaseChange,
            LightingViewChange::CameraCut))
    {
        return 3;
    }

    LightingView bodyChanged = rebased;
    bodyChanged.body.low = 99U;

    const auto bodyChange =
        ClassifyLightingViewChange(
            rebased,
            bodyChanged,
            true);

    if (!HasChange(
            bodyChange,
            LightingViewChange::BodyChanged) ||
        !HasChange(
            bodyChange,
            LightingViewChange::CameraCut))
    {
        return 4;
    }

    bool threw = false;
    try
    {
        static_cast<void>(
            StableCellForPoint(
                point,
                0.0,
                a));
    }
    catch (...)
    {
        threw = true;
    }

    return threw ? 0 : 5;
}

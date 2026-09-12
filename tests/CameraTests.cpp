#include <orbit/camera/FreeCamera.hpp>
#include <orbit/math/Matrix.hpp>

#include <cmath>
#include <iostream>
#include <numbers>

namespace
{
bool NearlyEqual(
    const orbit::f64 a,
    const orbit::f64 b,
    const orbit::f64 epsilon = 1.0e-6)
{
    return std::abs(a - b) <= epsilon;
}

orbit::f32 ProjectDepth(
    const orbit::math::Mat4& projection,
    const orbit::f32 viewZ)
{
    const orbit::f32 clipZ =
        viewZ *
            projection.At(2, 2) +
        projection.At(3, 2);

    const orbit::f32 clipW =
        viewZ *
            projection.At(2, 3) +
        projection.At(3, 3);

    return clipZ / clipW;
}
} // namespace

int main()
{
    orbit::camera::FreeCamera camera({
        .mouseSensitivityRadiansPerPixel = 0.01,
        .moveSpeedMetersPerSecond = 100.0,
        .verticalSpeedMetersPerSecond = 50.0,
        .boostMultiplier = 4.0,
        .maximumPitchRadians = 1.0,
        .initialYawRadians = 0.0,
        .initialPitchRadians = 0.0
    });

    const auto forward =
        camera.Update({
            .deltaSeconds = 1.0,
            .moveForward = 1.0
        });

    if (!NearlyEqual(
            forward.tangentMotionMeters.x,
            0.0) ||
        !NearlyEqual(
            forward.tangentMotionMeters.y,
            100.0))
    {
        std::cerr
            << "Free camera forward motion is not aligned with initial heading.\n";
        return 1;
    }

    const auto turned =
        camera.Update({
            .deltaSeconds = 1.0,
            .mouseDeltaX =
                std::numbers::pi /
                (2.0 * 0.01),
            .moveForward = 1.0
        });

    if (!NearlyEqual(
            turned.tangentMotionMeters.x,
            100.0,
            1.0e-5) ||
        !NearlyEqual(
            turned.tangentMotionMeters.y,
            0.0,
            1.0e-5))
    {
        std::cerr
            << "Free camera movement did not rotate with yaw.\n";
        return 1;
    }

    orbit::camera::FreeCamera diagonal({
        .moveSpeedMetersPerSecond = 100.0,
        .verticalSpeedMetersPerSecond = 50.0,
        .boostMultiplier = 4.0,
        .initialPitchRadians = 0.0
    });

    const auto diagonalUpdate =
        diagonal.Update({
            .deltaSeconds = 1.0,
            .moveRight = 1.0,
            .moveForward = 1.0
        });

    const orbit::f64 diagonalDistance =
        std::sqrt(
            diagonalUpdate.
                tangentMotionMeters.x *
            diagonalUpdate.
                tangentMotionMeters.x +
            diagonalUpdate.
                tangentMotionMeters.y *
            diagonalUpdate.
                tangentMotionMeters.y);

    if (!NearlyEqual(
            diagonalDistance,
            100.0,
            1.0e-5))
    {
        std::cerr
            << "Free camera diagonal movement is faster than axial movement.\n";
        return 1;
    }

    const auto boosted =
        diagonal.Update({
            .deltaSeconds = 2.0,
            .moveUp = 1.0,
            .boost = true
        });

    if (!NearlyEqual(
            boosted.verticalMotionMeters,
            400.0))
    {
        std::cerr
            << "Free camera boost did not scale vertical speed.\n";
        return 1;
    }

    orbit::camera::FreeCamera pitch({
        .mouseSensitivityRadiansPerPixel = 1.0,
        .maximumPitchRadians = 1.0,
        .initialPitchRadians = 0.0
    });

    static_cast<void>(
        pitch.Update({
            .mouseDeltaY = -100.0
        }));

    if (!NearlyEqual(
            pitch.PitchRadians(),
            1.0))
    {
        std::cerr
            << "Free camera positive pitch clamp failed.\n";
        return 1;
    }

    static_cast<void>(
        pitch.Update({
            .mouseDeltaY = 100.0
        }));

    if (!NearlyEqual(
            pitch.PitchRadians(),
            -1.0))
    {
        std::cerr
            << "Free camera negative pitch clamp failed.\n";
        return 1;
    }

    constexpr orbit::f32 nearPlane = 10.0F;
    constexpr orbit::f32 farPlane =
        3'000'000.0F;

    const orbit::math::Mat4 reverseProjection =
        orbit::math::PerspectiveReverseZLH(
            1.22173048F,
            16.0F / 9.0F,
            nearPlane,
            farPlane);

    const orbit::f32 nearDepth =
        ProjectDepth(
            reverseProjection,
            nearPlane);

    const orbit::f32 midDepth =
        ProjectDepth(
            reverseProjection,
            100'000.0F);

    const orbit::f32 farDepth =
        ProjectDepth(
            reverseProjection,
            farPlane);

    if (!NearlyEqual(
            nearDepth,
            1.0,
            1.0e-6) ||
        !NearlyEqual(
            farDepth,
            0.0,
            1.0e-6) ||
        !(nearDepth >
          midDepth &&
          midDepth >
          farDepth))
    {
        std::cerr
            << "Reversed-Z projection depth mapping is invalid.\n";
        return 1;
    }

    return 0;
}

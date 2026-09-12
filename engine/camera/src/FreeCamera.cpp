#include <orbit/camera/FreeCamera.hpp>

#include <algorithm>
#include <cmath>
#include <numbers>

namespace orbit::camera
{
FreeCamera::FreeCamera(
    const FreeCameraConfig config)
    : config_(config),
      yawRadians_(config.initialYawRadians),
      pitchRadians_(
          std::clamp(
              config.initialPitchRadians,
              -config.maximumPitchRadians,
              config.maximumPitchRadians))
{
}

FreeCameraUpdate FreeCamera::Update(
    const FreeCameraInput& input) noexcept
{
    yawRadians_ +=
        input.mouseDeltaX *
        config_.
            mouseSensitivityRadiansPerPixel;

    pitchRadians_ -=
        input.mouseDeltaY *
        config_.
            mouseSensitivityRadiansPerPixel;

    yawRadians_ =
        std::remainder(
            yawRadians_,
            2.0 *
                std::numbers::pi);

    pitchRadians_ =
        std::clamp(
            pitchRadians_,
            -config_.maximumPitchRadians,
            config_.maximumPitchRadians);

    f64 moveRight =
        std::clamp(
            input.moveRight,
            -1.0,
            1.0);

    f64 moveForward =
        std::clamp(
            input.moveForward,
            -1.0,
            1.0);

    const f64 planarLength =
        std::sqrt(
            moveRight * moveRight +
            moveForward * moveForward);

    if (planarLength > 1.0)
    {
        moveRight /= planarLength;
        moveForward /= planarLength;
    }

    return BuildUpdate(
        moveRight,
        moveForward,
        std::clamp(
            input.moveUp,
            -1.0,
            1.0),
        std::max(
            input.deltaSeconds,
            0.0),
        input.boost);
}

FreeCameraUpdate FreeCamera::Current() const noexcept
{
    return BuildUpdate(
        0.0,
        0.0,
        0.0,
        0.0,
        false);
}

f64 FreeCamera::YawRadians() const noexcept
{
    return yawRadians_;
}

f64 FreeCamera::PitchRadians() const noexcept
{
    return pitchRadians_;
}

FreeCameraUpdate FreeCamera::BuildUpdate(
    const f64 moveRight,
    const f64 moveForward,
    const f64 moveUp,
    const f64 deltaSeconds,
    const bool boost) const noexcept
{
    const f64 sinYaw =
        std::sin(yawRadians_);

    const f64 cosYaw =
        std::cos(yawRadians_);

    const f64 sinPitch =
        std::sin(pitchRadians_);

    const f64 cosPitch =
        std::cos(pitchRadians_);

    const math::Float3 forward{
        static_cast<f32>(
            sinYaw * cosPitch),
        static_cast<f32>(
            sinPitch),
        static_cast<f32>(
            cosYaw * cosPitch)
    };

    const math::Float3 right{
        static_cast<f32>(
            cosYaw),
        0.0F,
        static_cast<f32>(
            -sinYaw)
    };

    const math::Float3 up =
        math::Normalize(
            math::Cross(
                forward,
                right));

    const f64 speedMultiplier =
        boost
            ? config_.boostMultiplier
            : 1.0;

    const f64 planarDistance =
        config_.
            moveSpeedMetersPerSecond *
        speedMultiplier *
        deltaSeconds;

    const f64 verticalDistance =
        config_.
            verticalSpeedMetersPerSecond *
        speedMultiplier *
        deltaSeconds;

    const f64 eastMotion =
        (moveRight * cosYaw +
         moveForward * sinYaw) *
        planarDistance;

    const f64 northMotion =
        (-moveRight * sinYaw +
         moveForward * cosYaw) *
        planarDistance;

    const f64 verticalMotion =
        moveUp *
        verticalDistance;

    return {
        .tangentMotionMeters = {
            eastMotion,
            northMotion
        },
        .verticalMotionMeters =
            verticalMotion,
        .forward = forward,
        .up = up,
        .moved =
            eastMotion != 0.0 ||
            northMotion != 0.0 ||
            verticalMotion != 0.0
    };
}
} // namespace orbit::camera

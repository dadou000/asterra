#pragma once

#include <orbit/core/Types.hpp>
#include <orbit/math/Vector.hpp>

namespace orbit::camera
{
struct FreeCameraConfig
{
    f64 mouseSensitivityRadiansPerPixel{0.0025};
    f64 moveSpeedMetersPerSecond{400.0};
    f64 verticalSpeedMetersPerSecond{200.0};
    f64 boostMultiplier{10.0};
    f64 maximumPitchRadians{1.5533430342749532};
    f64 initialYawRadians{0.0};
    f64 initialPitchRadians{-0.2730087030867106};
};

struct FreeCameraInput
{
    f64 deltaSeconds{0.0};

    f64 mouseDeltaX{0.0};
    f64 mouseDeltaY{0.0};

    f64 moveRight{0.0};
    f64 moveForward{0.0};
    f64 moveUp{0.0};

    bool boost{false};
};

struct FreeCameraUpdate
{
    math::Double2 tangentMotionMeters{};
    f64 verticalMotionMeters{0.0};

    math::Float3 forward{
        0.0F,
        -0.2696299F,
        0.962964F
    };

    math::Float3 up{
        0.0F,
        0.962964F,
        0.2696299F
    };

    bool moved{false};
};

class FreeCamera
{
public:
    explicit FreeCamera(
        FreeCameraConfig config = {});

    [[nodiscard]] FreeCameraUpdate Update(
        const FreeCameraInput& input) noexcept;

    [[nodiscard]] FreeCameraUpdate Current() const noexcept;

    [[nodiscard]] f64 YawRadians() const noexcept;
    [[nodiscard]] f64 PitchRadians() const noexcept;

private:
    [[nodiscard]] FreeCameraUpdate BuildUpdate(
        f64 moveRight,
        f64 moveForward,
        f64 moveUp,
        f64 deltaSeconds,
        bool boost) const noexcept;

    FreeCameraConfig config_;
    f64 yawRadians_{0.0};
    f64 pitchRadians_{0.0};
};
} // namespace orbit::camera

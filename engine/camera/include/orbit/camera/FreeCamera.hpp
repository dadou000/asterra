#pragma once

#include <orbit/core/Types.hpp>
#include <orbit/math/Vector.hpp>

namespace orbit::camera
{
struct FreeCameraConfig
{
    f64 mouseSensitivityRadiansPerPixel{0.0025};

    // Move speed is log-log interpolated by altitude above the
    // ground, from a walking pace near the surface to an
    // orbit-crossing dash far above it -- the same WASD input feels
    // grounded near terrain and fast once you climb away from it.
    f64 groundSpeedMetersPerSecond{1.94};          // ~7 km/h
    f64 groundBoostSpeedMetersPerSecond{55.56};    // ~200 km/h
    f64 orbitSpeedMetersPerSecond{13'888.9};       // ~50,000 km/h
    f64 orbitBoostSpeedMetersPerSecond{555'555.6}; // ~2,000,000 km/h
    f64 minAltitudeMeters{2.0};
    f64 maxAltitudeMeters{2'000'000.0};

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

    // Current height above the ground directly below the camera.
    // Drives the altitude-based speed curve.
    f64 altitudeMeters{0.0};
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
        bool boost,
        f64 altitudeMeters) const noexcept;

    FreeCameraConfig config_;
    f64 yawRadians_{0.0};
    f64 pitchRadians_{0.0};
};
} // namespace orbit::camera

#pragma once

#include <orbit/math/Vector.hpp>
#include <orbit/time/SimulationTime.hpp>

#include <memory>
#include <string_view>

namespace orbit::celestial_orbits
{
enum class OrbitStateQuality : u8
{
    ExactAnalytic = 0,
    Fixed = 1,
    SampledExact = 2,
    SampledInterpolated = 3
};

struct OrbitState
{
    math::Double3 positionMeters{};
    math::Double3 velocityMetersPerSecond{};
    OrbitStateQuality quality{OrbitStateQuality::ExactAnalytic};
};

struct FixedOrbitState
{
    math::Double3 positionMeters{};
};

struct AnalyticConicElements
{
    // Ellipse: positive semi-major axis, 0 <= e < 1.
    // Hyperbola: positive magnitude of the (negative) semi-major axis, e > 1.
    // Parabola: semiMajorAxisMeters ignored and periapsisDistanceMeters used.
    f64 semiMajorAxisMeters{1.0};
    f64 periapsisDistanceMeters{1.0};
    f64 eccentricity{0.0};

    f64 inclinationRadians{0.0};
    f64 longitudeAscendingNodeRadians{0.0};
    f64 argumentPeriapsisRadians{0.0};

    // Ellipse/hyperbola: conventional mean anomaly at epoch.
    // Parabola: Barker parameter B = D + D^3/3 at epoch.
    f64 phaseAtEpoch{0.0};

    f64 gravitationalParameterM3PerS2{1.0};
    time::SimulationTime epoch{};
};

[[nodiscard]] OrbitState EvaluateFixed(
    const FixedOrbitState& model,
    time::SimulationTime atTime) noexcept;

[[nodiscard]] OrbitState EvaluateAnalyticConic(
    const AnalyticConicElements& elements,
    time::SimulationTime atTime);

class OrbitStateProvider
{
public:
    virtual ~OrbitStateProvider() = default;

    [[nodiscard]] virtual OrbitState EvaluateState(
        time::SimulationTime atTime) const = 0;

    [[nodiscard]] virtual std::string_view ModelName() const noexcept = 0;
};

class FixedOrbitStateProvider final : public OrbitStateProvider
{
public:
    explicit FixedOrbitStateProvider(FixedOrbitState state);

    [[nodiscard]] OrbitState EvaluateState(
        time::SimulationTime atTime) const override;

    [[nodiscard]] std::string_view ModelName() const noexcept override;

private:
    FixedOrbitState state_;
};

class AnalyticConicOrbitStateProvider final : public OrbitStateProvider
{
public:
    explicit AnalyticConicOrbitStateProvider(
        AnalyticConicElements elements);

    [[nodiscard]] OrbitState EvaluateState(
        time::SimulationTime atTime) const override;

    [[nodiscard]] std::string_view ModelName() const noexcept override;

private:
    AnalyticConicElements elements_;
};

[[nodiscard]] std::unique_ptr<OrbitStateProvider>
MakeFixedProvider(FixedOrbitState state);

[[nodiscard]] std::unique_ptr<OrbitStateProvider>
MakeAnalyticConicProvider(AnalyticConicElements elements);
} // namespace orbit::celestial_orbits

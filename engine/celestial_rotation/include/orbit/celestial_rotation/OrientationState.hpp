#pragma once

#include <orbit/celestial_orbits/OrbitState.hpp>
#include <orbit/math/RigidTransform.hpp>
#include <orbit/time/SimulationTime.hpp>

#include <memory>
#include <string_view>

namespace orbit::celestial_rotation
{
struct OrientationState
{
    math::Double3x3 parentFromBodyRotation{};
};

class OrientationProvider
{
public:
    virtual ~OrientationProvider() = default;

    [[nodiscard]] virtual OrientationState EvaluateOrientation(
        time::SimulationTime atTime) const = 0;

    [[nodiscard]] virtual std::string_view ModelName() const noexcept = 0;
};

struct FixedOrientation
{
    math::Double3x3 parentFromBodyRotation{};
};

struct UniformSpinOrientation
{
    math::Double3 axisInParent{0.0, 0.0, 1.0};
    f64 angularVelocityRadiansPerSecond{0.0};
    f64 phaseRadiansAtEpoch{0.0};
    time::SimulationTime epoch{};
};

struct SynchronousOrientation
{
    std::shared_ptr<const celestial_orbits::OrbitStateProvider> orbitState;
    math::Double3 poleInParent{0.0, 0.0, 1.0};
    f64 phaseOffsetRadians{0.0};
};

[[nodiscard]] OrientationState EvaluateFixed(
    const FixedOrientation& model) noexcept;

[[nodiscard]] OrientationState EvaluateUniformSpin(
    const UniformSpinOrientation& model,
    time::SimulationTime atTime);

[[nodiscard]] OrientationState EvaluateSynchronous(
    const SynchronousOrientation& model,
    time::SimulationTime atTime);

class FixedOrientationProvider final : public OrientationProvider
{
public:
    explicit FixedOrientationProvider(FixedOrientation model);

    [[nodiscard]] OrientationState EvaluateOrientation(
        time::SimulationTime atTime) const override;

    [[nodiscard]] std::string_view ModelName() const noexcept override;

private:
    FixedOrientation model_;
};

class UniformSpinOrientationProvider final : public OrientationProvider
{
public:
    explicit UniformSpinOrientationProvider(UniformSpinOrientation model);

    [[nodiscard]] OrientationState EvaluateOrientation(
        time::SimulationTime atTime) const override;

    [[nodiscard]] std::string_view ModelName() const noexcept override;

private:
    UniformSpinOrientation model_;
};

class SynchronousOrientationProvider final : public OrientationProvider
{
public:
    explicit SynchronousOrientationProvider(SynchronousOrientation model);

    [[nodiscard]] OrientationState EvaluateOrientation(
        time::SimulationTime atTime) const override;

    [[nodiscard]] std::string_view ModelName() const noexcept override;

private:
    SynchronousOrientation model_;
};
} // namespace orbit::celestial_rotation

#include <orbit/celestial_rotation/OrientationState.hpp>

#include <cmath>
#include <stdexcept>
#include <type_traits>
#include <utility>

namespace orbit::celestial_rotation
{
namespace
{
[[nodiscard]] math::Double3 RotateAroundAxis(
    const math::Double3& value,
    const math::Double3& axis,
    const f64 radians) noexcept
{
    const f64 c = std::cos(radians);
    const f64 s = std::sin(radians);

    return value * c +
        math::Cross(axis, value) * s +
        axis * (math::Dot(axis, value) * (1.0 - c));
}

// Body-fixed frame convention: local +Y is the spin (north) pole and the
// local XZ plane is the equator. Every body-fixed surface authority (terrain
// climate/wind latitude, giant zonal bands, ring planes, cloud fields) is
// authored Y-up, so the orientation must map local +Y onto the pole in the
// parent frame for those fields to rotate about their own poles.
[[nodiscard]] math::Double3x3 BasisFromAxisPhase(
    const math::Double3 axis,
    const f64 phase)
{
    if (math::Length(axis) <= 0.0)
    {
        throw std::invalid_argument(
            "Orientation axis must be non-zero.");
    }

    const math::Double3 y =
        math::Normalize(axis);

    math::Double3 reference =
        std::abs(y.x) < 0.9
            ? math::Double3{1.0, 0.0, 0.0}
            : math::Double3{0.0, 0.0, 1.0};

    math::Double3 x =
        reference -
        y * math::Dot(reference, y);
    x = math::Normalize(x);

    math::Double3 z =
        math::Normalize(
            math::Cross(x, y));

    x = RotateAroundAxis(x, y, phase);
    z = RotateAroundAxis(z, y, phase);

    return {
        .xAxis = x,
        .yAxis = y,
        .zAxis = z
    };
}
} // namespace

OrientationState EvaluateFixed(
    const FixedOrientation& model) noexcept
{
    return {
        .parentFromBodyRotation =
            model.parentFromBodyRotation
    };
}

OrientationState EvaluateUniformSpin(
    const UniformSpinOrientation& model,
    const time::SimulationTime atTime)
{
    if (!std::isfinite(model.angularVelocityRadiansPerSecond) ||
        !std::isfinite(model.phaseRadiansAtEpoch))
    {
        throw std::invalid_argument(
            "Uniform spin parameters must be finite.");
    }

    const f64 elapsedSeconds =
        static_cast<f64>(
            atTime.microsecondsFromEpoch -
            model.epoch.microsecondsFromEpoch) /
        1'000'000.0;

    const f64 phase =
        model.phaseRadiansAtEpoch +
        model.angularVelocityRadiansPerSecond *
            elapsedSeconds;

    return {
        .parentFromBodyRotation =
            BasisFromAxisPhase(
                model.axisInParent,
                phase)
    };
}

OrientationState EvaluateSynchronous(
    const SynchronousOrientation& model,
    const time::SimulationTime atTime)
{
    if (!model.orbitState)
    {
        throw std::invalid_argument(
            "Synchronous orientation requires an orbit state provider.");
    }

    const auto orbit =
        model.orbitState->EvaluateState(atTime);

    if (math::Length(orbit.positionMeters) <= 0.0)
    {
        throw std::runtime_error(
            "Synchronous orientation cannot face a zero-length parent vector.");
    }

    math::Double3 y =
        math::Normalize(model.poleInParent);

    math::Double3 x =
        math::Normalize(
            orbit.positionMeters * -1.0);

    // Remove any component along the requested pole so X lies in the
    // equatorial plane. For degenerate pole/radius alignment, derive the pole
    // from orbital angular momentum when possible.
    x = x - y * math::Dot(x, y);

    if (math::Length(x) <= 1.0e-12)
    {
        const math::Double3 angularMomentum =
            math::Cross(
                orbit.positionMeters,
                orbit.velocityMetersPerSecond);

        if (math::Length(angularMomentum) <= 1.0e-12)
        {
            throw std::runtime_error(
                "Synchronous orientation requires a non-degenerate orbit.");
        }

        y = math::Normalize(angularMomentum);
        x = math::Normalize(
            orbit.positionMeters * -1.0);
        x = math::Normalize(
            x - y * math::Dot(x, y));
    }
    else
    {
        x = math::Normalize(x);
    }

    // Same body-fixed convention as uniform spin: +Y is the pole, +X faces
    // the parent, Z completes the right-handed equatorial basis.
    math::Double3 z =
        math::Normalize(
            math::Cross(x, y));
    x = math::Normalize(
        math::Cross(y, z));

    if (model.phaseOffsetRadians != 0.0)
    {
        x = RotateAroundAxis(
            x,
            y,
            model.phaseOffsetRadians);
        z = RotateAroundAxis(
            z,
            y,
            model.phaseOffsetRadians);
    }

    return {
        .parentFromBodyRotation = {
            .xAxis = x,
            .yAxis = y,
            .zAxis = z
        }
    };
}

FixedOrientationProvider::FixedOrientationProvider(
    FixedOrientation model)
    : model_(std::move(model))
{
}

OrientationState
FixedOrientationProvider::EvaluateOrientation(
    const time::SimulationTime) const
{
    return EvaluateFixed(model_);
}

std::string_view
FixedOrientationProvider::ModelName() const noexcept
{
    return "Fixed";
}

UniformSpinOrientationProvider::
UniformSpinOrientationProvider(
    UniformSpinOrientation model)
    : model_(std::move(model))
{
}

OrientationState
UniformSpinOrientationProvider::EvaluateOrientation(
    const time::SimulationTime atTime) const
{
    return EvaluateUniformSpin(
        model_,
        atTime);
}

std::string_view
UniformSpinOrientationProvider::ModelName() const noexcept
{
    return "Uniform Spin";
}

SynchronousOrientationProvider::
SynchronousOrientationProvider(
    SynchronousOrientation model)
    : model_(std::move(model))
{
    if (!model_.orbitState)
    {
        throw std::invalid_argument(
            "Synchronous orientation provider requires orbit state.");
    }
}

OrientationState
SynchronousOrientationProvider::EvaluateOrientation(
    const time::SimulationTime atTime) const
{
    return EvaluateSynchronous(
        model_,
        atTime);
}

std::string_view
SynchronousOrientationProvider::ModelName() const noexcept
{
    return "Synchronous";
}
} // namespace orbit::celestial_rotation

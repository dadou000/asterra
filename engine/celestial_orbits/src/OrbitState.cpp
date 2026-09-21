#include <orbit/celestial_orbits/OrbitState.hpp>

#include <algorithm>
#include <cmath>
#include <numbers>
#include <stdexcept>
#include <utility>

namespace orbit::celestial_orbits
{
namespace
{
constexpr f64 kParabolicTolerance = 1.0e-10;
constexpr int kMaximumIterations = 64;
constexpr f64 kSolveTolerance = 1.0e-13;

[[nodiscard]] f64 ElapsedSeconds(
    const time::SimulationTime from,
    const time::SimulationTime to) noexcept
{
    return static_cast<f64>(
        to.microsecondsFromEpoch -
        from.microsecondsFromEpoch) /
        1'000'000.0;
}

[[nodiscard]] f64 SolveEllipticAnomaly(
    const f64 meanAnomaly,
    const f64 eccentricity)
{
    const f64 twoPi =
        2.0 * std::numbers::pi_v<f64>;
    const f64 wrapped =
        std::remainder(meanAnomaly, twoPi);

    f64 eccentricAnomaly =
        eccentricity < 0.8
            ? wrapped
            : std::copysign(
                std::numbers::pi_v<f64>,
                wrapped == 0.0 ? 1.0 : wrapped);

    for (int iteration = 0;
         iteration < kMaximumIterations;
         ++iteration)
    {
        const f64 f =
            eccentricAnomaly -
            eccentricity * std::sin(eccentricAnomaly) -
            wrapped;
        const f64 derivative =
            1.0 -
            eccentricity * std::cos(eccentricAnomaly);
        const f64 delta = f / derivative;
        eccentricAnomaly -= delta;

        if (std::abs(delta) <= kSolveTolerance)
        {
            return eccentricAnomaly;
        }
    }

    throw std::runtime_error(
        "Elliptic Kepler solve did not converge.");
}

[[nodiscard]] f64 SolveHyperbolicAnomaly(
    const f64 meanAnomaly,
    const f64 eccentricity)
{
    f64 hyperbolicAnomaly =
        std::asinh(meanAnomaly / eccentricity);

    for (int iteration = 0;
         iteration < kMaximumIterations;
         ++iteration)
    {
        const f64 f =
            eccentricity * std::sinh(hyperbolicAnomaly) -
            hyperbolicAnomaly -
            meanAnomaly;
        const f64 derivative =
            eccentricity * std::cosh(hyperbolicAnomaly) -
            1.0;
        const f64 delta = f / derivative;
        hyperbolicAnomaly -= delta;

        if (std::abs(delta) <= kSolveTolerance)
        {
            return hyperbolicAnomaly;
        }
    }

    throw std::runtime_error(
        "Hyperbolic Kepler solve did not converge.");
}

[[nodiscard]] f64 SolveBarkerParameter(
    const f64 barkerMean)
{
    // D + D^3/3 = B has one real root. Newton converges quickly from cbrt
    // at large |B| and B near periapsis.
    f64 d =
        std::abs(barkerMean) < 1.0
            ? barkerMean
            : std::cbrt(3.0 * barkerMean);

    for (int iteration = 0;
         iteration < kMaximumIterations;
         ++iteration)
    {
        const f64 f =
            d + (d * d * d) / 3.0 -
            barkerMean;
        const f64 derivative =
            1.0 + d * d;
        const f64 delta =
            f / derivative;
        d -= delta;

        if (std::abs(delta) <= kSolveTolerance)
        {
            return d;
        }
    }

    throw std::runtime_error(
        "Parabolic Barker solve did not converge.");
}

[[nodiscard]] math::Double3 RotatePerifocalToReference(
    const math::Double3 value,
    const f64 longitudeAscendingNode,
    const f64 inclination,
    const f64 argumentPeriapsis) noexcept
{
    const f64 cosO = std::cos(longitudeAscendingNode);
    const f64 sinO = std::sin(longitudeAscendingNode);
    const f64 cosI = std::cos(inclination);
    const f64 sinI = std::sin(inclination);
    const f64 cosW = std::cos(argumentPeriapsis);
    const f64 sinW = std::sin(argumentPeriapsis);

    const math::Double3 p{
        cosO * cosW - sinO * sinW * cosI,
        sinO * cosW + cosO * sinW * cosI,
        sinW * sinI
    };
    const math::Double3 q{
        -cosO * sinW - sinO * cosW * cosI,
        -sinO * sinW + cosO * cosW * cosI,
        cosW * sinI
    };

    return p * value.x +
        q * value.y;
}

void Validate(const AnalyticConicElements& e)
{
    const auto finite =
        [](const f64 value)
        {
            return std::isfinite(value);
        };

    if (!finite(e.eccentricity) ||
        e.eccentricity < 0.0 ||
        !finite(e.gravitationalParameterM3PerS2) ||
        e.gravitationalParameterM3PerS2 <= 0.0)
    {
        throw std::invalid_argument(
            "Conic eccentricity and gravitational parameter are invalid.");
    }

    if (!finite(e.inclinationRadians) ||
        !finite(e.longitudeAscendingNodeRadians) ||
        !finite(e.argumentPeriapsisRadians) ||
        !finite(e.phaseAtEpoch))
    {
        throw std::invalid_argument(
            "Conic orientation/phase must be finite.");
    }

    if (std::abs(e.eccentricity - 1.0) <= kParabolicTolerance)
    {
        if (!finite(e.periapsisDistanceMeters) ||
            e.periapsisDistanceMeters <= 0.0)
        {
            throw std::invalid_argument(
                "Parabolic orbit requires positive periapsis distance.");
        }
    }
    else if (!finite(e.semiMajorAxisMeters) ||
             e.semiMajorAxisMeters <= 0.0)
    {
        throw std::invalid_argument(
            "Elliptic/hyperbolic orbit requires positive semi-major-axis magnitude.");
    }
}
} // namespace

OrbitState EvaluateFixed(
    const FixedOrbitState& model,
    const time::SimulationTime) noexcept
{
    return {
        .positionMeters = model.positionMeters,
        .velocityMetersPerSecond = {},
        .quality = OrbitStateQuality::Fixed
    };
}

OrbitState EvaluateAnalyticConic(
    const AnalyticConicElements& elements,
    const time::SimulationTime atTime)
{
    Validate(elements);

    const f64 dt =
        ElapsedSeconds(elements.epoch, atTime);
    math::Double3 positionPerifocal{};
    math::Double3 velocityPerifocal{};

    if (elements.eccentricity <
        1.0 - kParabolicTolerance)
    {
        const f64 a =
            elements.semiMajorAxisMeters;
        const f64 e =
            elements.eccentricity;
        const f64 meanMotion =
            std::sqrt(
                elements.gravitationalParameterM3PerS2 /
                (a * a * a));
        const f64 meanAnomaly =
            elements.phaseAtEpoch +
            meanMotion * dt;
        const f64 E =
            SolveEllipticAnomaly(
                meanAnomaly,
                e);
        const f64 root =
            std::sqrt(1.0 - e * e);
        const f64 denominator =
            1.0 - e * std::cos(E);

        positionPerifocal = {
            a * (std::cos(E) - e),
            a * root * std::sin(E),
            0.0
        };

        const f64 rate =
            meanMotion / denominator;
        velocityPerifocal = {
            -a * std::sin(E) * rate,
            a * root * std::cos(E) * rate,
            0.0
        };
    }
    else if (elements.eccentricity >
             1.0 + kParabolicTolerance)
    {
        const f64 a =
            elements.semiMajorAxisMeters;
        const f64 e =
            elements.eccentricity;
        const f64 meanMotion =
            std::sqrt(
                elements.gravitationalParameterM3PerS2 /
                (a * a * a));
        const f64 meanAnomaly =
            elements.phaseAtEpoch +
            meanMotion * dt;
        const f64 H =
            SolveHyperbolicAnomaly(
                meanAnomaly,
                e);
        const f64 root =
            std::sqrt(e * e - 1.0);
        const f64 denominator =
            e * std::cosh(H) - 1.0;
        const f64 rate =
            meanMotion / denominator;

        positionPerifocal = {
            a * (e - std::cosh(H)),
            a * root * std::sinh(H),
            0.0
        };
        velocityPerifocal = {
            -a * std::sinh(H) * rate,
            a * root * std::cosh(H) * rate,
            0.0
        };
    }
    else
    {
        const f64 q =
            elements.periapsisDistanceMeters;
        const f64 barkerRate =
            std::sqrt(
                elements.gravitationalParameterM3PerS2 /
                (2.0 * q * q * q));
        const f64 barkerMean =
            elements.phaseAtEpoch +
            barkerRate * dt;
        const f64 D =
            SolveBarkerParameter(
                barkerMean);
        const f64 dDot =
            barkerRate /
            (1.0 + D * D);

        positionPerifocal = {
            q * (1.0 - D * D),
            2.0 * q * D,
            0.0
        };
        velocityPerifocal = {
            -2.0 * q * D * dDot,
            2.0 * q * dDot,
            0.0
        };
    }

    return {
        .positionMeters =
            RotatePerifocalToReference(
                positionPerifocal,
                elements.longitudeAscendingNodeRadians,
                elements.inclinationRadians,
                elements.argumentPeriapsisRadians),
        .velocityMetersPerSecond =
            RotatePerifocalToReference(
                velocityPerifocal,
                elements.longitudeAscendingNodeRadians,
                elements.inclinationRadians,
                elements.argumentPeriapsisRadians),
        .quality = OrbitStateQuality::ExactAnalytic
    };
}

FixedOrbitStateProvider::FixedOrbitStateProvider(
    FixedOrbitState state)
    : state_(std::move(state))
{
}

OrbitState FixedOrbitStateProvider::EvaluateState(
    const time::SimulationTime atTime) const
{
    return EvaluateFixed(state_, atTime);
}

std::string_view
FixedOrbitStateProvider::ModelName() const noexcept
{
    return "Fixed";
}

AnalyticConicOrbitStateProvider::
AnalyticConicOrbitStateProvider(
    AnalyticConicElements elements)
    : elements_(std::move(elements))
{
    Validate(elements_);
}

OrbitState
AnalyticConicOrbitStateProvider::EvaluateState(
    const time::SimulationTime atTime) const
{
    return EvaluateAnalyticConic(
        elements_,
        atTime);
}

std::string_view
AnalyticConicOrbitStateProvider::ModelName() const noexcept
{
    return "Analytic Conic";
}

std::unique_ptr<OrbitStateProvider>
MakeFixedProvider(FixedOrbitState state)
{
    return std::make_unique<FixedOrbitStateProvider>(
        std::move(state));
}

std::unique_ptr<OrbitStateProvider>
MakeAnalyticConicProvider(
    AnalyticConicElements elements)
{
    return std::make_unique<AnalyticConicOrbitStateProvider>(
        std::move(elements));
}
} // namespace orbit::celestial_orbits

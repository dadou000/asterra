#include <orbit/studio_session/SimulationClock.hpp>

#include <cmath>
#include <limits>
#include <stdexcept>

namespace orbit::studio_session
{
time::SimulationTime
SimulationClock::Time() const noexcept
{
    return time_;
}

void SimulationClock::SetTime(
    const time::SimulationTime value) noexcept
{
    time_ = value;
    fractionalMicroseconds_ = 0.0;
}

bool SimulationClock::Playing() const noexcept
{
    return playing_;
}

void SimulationClock::SetPlaying(
    const bool playing) noexcept
{
    playing_ = playing;
}

f64 SimulationClock::Rate() const noexcept
{
    return rate_;
}

void SimulationClock::SetRate(
    const f64 simulationSecondsPerRealSecond)
{
    if (!std::isfinite(
            simulationSecondsPerRealSecond))
    {
        throw std::invalid_argument(
            "Simulation clock rate must be finite.");
    }

    rate_ =
        simulationSecondsPerRealSecond;
}

void SimulationClock::Advance(
    const f64 realSeconds)
{
    if (!playing_)
    {
        return;
    }

    if (!std::isfinite(realSeconds) ||
        realSeconds < 0.0)
    {
        throw std::invalid_argument(
            "Simulation clock real delta must be finite and non-negative.");
    }

    StepSeconds(
        realSeconds * rate_);
}

void SimulationClock::StepSeconds(
    const f64 simulationSeconds)
{
    if (!std::isfinite(simulationSeconds))
    {
        throw std::invalid_argument(
            "Simulation clock step must be finite.");
    }

    const f64 requestedMicroseconds =
        simulationSeconds * 1'000'000.0 +
        fractionalMicroseconds_;

    const f64 rounded =
        std::trunc(requestedMicroseconds);

    if (rounded >
            static_cast<f64>(
                std::numeric_limits<i64>::max()) ||
        rounded <
            static_cast<f64>(
                std::numeric_limits<i64>::min()))
    {
        throw std::overflow_error(
            "Simulation clock step exceeds int64 range.");
    }

    const i64 whole =
        static_cast<i64>(rounded);

    if ((whole > 0 &&
         time_.microsecondsFromEpoch >
             std::numeric_limits<i64>::max() - whole) ||
        (whole < 0 &&
         time_.microsecondsFromEpoch <
             std::numeric_limits<i64>::min() - whole))
    {
        throw std::overflow_error(
            "Simulation clock time exceeds int64 range.");
    }

    time_.microsecondsFromEpoch += whole;
    fractionalMicroseconds_ =
        requestedMicroseconds -
        static_cast<f64>(whole);
}
} // namespace orbit::studio_session

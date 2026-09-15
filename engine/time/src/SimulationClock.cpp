#include <orbit/time/SimulationClock.hpp>

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace orbit::time
{
SimulationClock::SimulationClock(
    const SimulationClockDesc& desc)
    : fixedTime_(desc.epoch),
      fixedStep_(desc.fixedStep),
      maximumFixedStepsPerAdvance_(
          desc.maximumFixedStepsPerAdvance)
{
    if (fixedStep_.count() <= 0)
    {
        throw std::invalid_argument(
            "SimulationClock fixed step must be positive.");
    }

    if (maximumFixedStepsPerAdvance_ == 0)
    {
        throw std::invalid_argument(
            "SimulationClock maximum fixed steps must be positive.");
    }
}

void SimulationClock::SetPaused(const bool paused) noexcept
{
    paused_ = paused;
}

bool SimulationClock::Paused() const noexcept
{
    return paused_;
}

void SimulationClock::SetTimeScale(const f64 scale)
{
    if (!std::isfinite(scale) || scale < 0.0)
    {
        throw std::invalid_argument(
            "SimulationClock time scale must be finite and non-negative.");
    }

    timeScale_ = scale;
}

f64 SimulationClock::TimeScale() const noexcept
{
    return timeScale_;
}

std::chrono::microseconds
SimulationClock::FixedStep() const noexcept
{
    return fixedStep_;
}

SimulationTime SimulationClock::FixedTime() const noexcept
{
    return fixedTime_;
}

SimulationTime SimulationClock::RenderTime() const noexcept
{
    return {
        .microsecondsFromEpoch =
            fixedTime_.microsecondsFromEpoch +
            accumulatorMicroseconds_
    };
}

f64 SimulationClock::InterpolationAlpha() const noexcept
{
    return std::clamp(
        static_cast<f64>(
            accumulatorMicroseconds_) /
            static_cast<f64>(
                fixedStep_.count()),
        0.0,
        1.0);
}

SimulationAdvance SimulationClock::Advance(
    const std::chrono::microseconds realElapsed)
{
    if (realElapsed.count() < 0)
    {
        throw std::invalid_argument(
            "SimulationClock cannot advance by negative real time.");
    }

    if (!paused_ && timeScale_ > 0.0)
    {
        const long double scaled =
            static_cast<long double>(
                realElapsed.count()) *
            static_cast<long double>(
                timeScale_);

        const i64 scaledMicroseconds =
            static_cast<i64>(
                std::llround(scaled));

        accumulatorMicroseconds_ +=
            scaledMicroseconds;
    }

    u32 fixedSteps = 0;

    while (accumulatorMicroseconds_ >=
               fixedStep_.count() &&
           fixedSteps <
               maximumFixedStepsPerAdvance_)
    {
        fixedTime_.microsecondsFromEpoch +=
            fixedStep_.count();

        accumulatorMicroseconds_ -=
            fixedStep_.count();

        ++fixedSteps;
    }

    return Snapshot(fixedSteps);
}

SimulationAdvance SimulationClock::Step(
    const u32 fixedStepCount)
{
    fixedTime_.microsecondsFromEpoch +=
        fixedStep_.count() *
        static_cast<i64>(fixedStepCount);

    return Snapshot(fixedStepCount);
}

SimulationAdvance SimulationClock::Snapshot(
    const u32 fixedSteps) const noexcept
{
    return {
        .fixedSteps = fixedSteps,
        .fixedTime = fixedTime_,
        .renderTime = RenderTime(),
        .interpolationAlpha =
            InterpolationAlpha()
    };
}
} // namespace orbit::time
